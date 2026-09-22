#include "claude_observer.hpp"
#include "history_worker.hpp"
#include "hook_relay.hpp"
#include "observer.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/process_group_guard.hpp"
#include "platform/posix/pty_process.hpp"
#include "transport/attention_protocol.hpp"
#include "transport/local_protocol.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace lapis::session;
constexpr int codex_sync_timeout_ms = 15000;
constexpr int terminal_sync_timeout_ms = 3000;
constexpr int backend_probe_interval_ms = 25;
constexpr int backend_probe_attempts = 400;
quint64 monotonic_ns() {
    return static_cast<quint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}
int hex_nibble(char16_t value) {
    if (value >= u'0' && value <= u'9')
        return value - u'0';
    if (value >= u'a' && value <= u'f')
        return value - u'a' + 10;
    if (value >= u'A' && value <= u'F')
        return value - u'A' + 10;
    return -1;
}
QByteArray parse_session_id(const QString& value) {
    QByteArray id;
    if (value.size() != 32)
        throw std::invalid_argument("Session ID must be exactly 32 hexadecimal characters");
    for (qsizetype index = 0; index < value.size(); index += 2) {
        const int high = hex_nibble(value[index].unicode());
        const int low = hex_nibble(value[index + 1].unicode());
        if (high < 0 || low < 0)
            throw std::invalid_argument("Session ID must be exactly 32 hexadecimal characters");
        id.append(static_cast<char>((static_cast<unsigned int>(high) << 4U) |
                                    static_cast<unsigned int>(low)));
    }
    if (id == QByteArray(16, '\0'))
        throw std::invalid_argument("Session ID cannot be zero");
    return id;
}
class SessionService final : public QObject {
  public:
    SessionService(const QString& endpoint, const QByteArray& requested_session_id,
                   const LaunchSpec& launch)
        : lock_(endpoint + QStringLiteral(".lock")), terminal_(launch.size, limits()),
          fingerprint_(launch_fingerprint(launch)), identity_{requested_session_id, wire::new_id()},
          history_(qEnvironmentVariable("LAPIS_HISTORY_ROOT",
                                        QStringLiteral(LAPIS_DEFAULT_HISTORY_ROOT)),
                   QString::fromLatin1(requested_session_id.toHex()), history_limits()) {
        configure_history();
        current_size_ = launch.size;
        terminal_.feed("\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\");
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock(0))
            throw std::runtime_error("Session service already owns endpoint");
        // Only the endpoint owner may trim its append log; racing GUI launches
        // must not truncate the log of an already-running service.
        QFile log(endpoint + QStringLiteral(".log"));
        if (log.size() > qint64{1024} * 1024 && log.open(QIODevice::ReadWrite))
            static_cast<void>(log.resize(0));
        if (QFileInfo::exists(endpoint)) {
            QLocalSocket existing;
            existing.connectToServer(endpoint);
            if (existing.waitForConnected(100) ||
                (existing.error() != QLocalSocket::ConnectionRefusedError &&
                 existing.error() != QLocalSocket::ServerNotFoundError))
                throw std::runtime_error("Socket endpoint is already in use or inaccessible");
        }
        QLocalServer::removeServer(endpoint);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(endpoint))
            throw std::runtime_error(server_.errorString().toStdString());
        struct stat bound_socket{};
        if (::lstat(QFile::encodeName(endpoint).constData(), &bound_socket) != 0 ||
            !S_ISSOCK(bound_socket.st_mode) || bound_socket.st_uid != ::getuid() ||
            (static_cast<unsigned int>(bound_socket.st_mode) & 0077U) != 0U)
            throw std::runtime_error("Bound socket is not private to the current user");
        timer_.setSingleShot(true);
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, [this] { publish(); });
        ack_timer_.setSingleShot(true);
        ack_timer_.setInterval(launch.agent == AgentMode::codex ? codex_sync_timeout_ms
                                                                : terminal_sync_timeout_ms);
        attention_timer_.setSingleShot(true);
        attention_timer_.setInterval(16);
        connect(&attention_timer_, &QTimer::timeout, this, [this] { publish_attention(); });
        connect(&ack_timer_, &QTimer::timeout, this, [this] {
            if (client_ && !ready_) {
                send_status(client_, wire::StatusCode::rejected,
                            "Attachment ready acknowledgement timed out");
                detach_client();
            }
        });
        connect(&server_, &QLocalServer::newConnection, this, [this] { attach(); });
        connect(&pty_, &posix::PtyProcess::started, this, [this] {
            process_started_ = true;
            hello();
        });
        connect(&pty_, &posix::PtyProcess::output, this, [this](const QByteArray& bytes) {
            timing_.pty_read_ns = monotonic_ns();
            if (pending_output_.size() + bytes.size() > qsizetype{64} * 1024) {
                stop(QStringLiteral("PTY output queue overflow"));
                return;
            }
            pending_output_ += bytes;
            pty_.pauseOutput(true);
            process_output();
        });
        connect(&pty_, &posix::PtyProcess::failure, this,
                [this](const QString& message) { stop(message); });
        connect(&pty_, &posix::PtyProcess::finished, this,
                [this](int code, QProcess::ExitStatus status) {
                    if (status == QProcess::CrashExit)
                        finish_session(
                            QStringLiteral("Process terminated by signal (%1)").arg(code),
                            128 + code);
                    else
                        finish_session(QStringLiteral("Process exited (%1)").arg(code), code);
                });
        if (launch.agent == AgentMode::codex)
            start_codex(endpoint, launch);
        else if (launch.agent == AgentMode::claude)
            start_claude(launch);
        else
            pty_.start(launch);
    }

    ~SessionService() override {
        stopping_ = true;
        stop_codex();
        if (claude_observer_)
            claude_observer_->stop();
        disconnect(&pty_, nullptr, this, nullptr);
        disconnect(&server_, nullptr, this, nullptr);
        disconnect(&timer_, nullptr, this, nullptr);
        if (client_)
            disconnect(client_, nullptr, this, nullptr);
        for (auto* retired : retired_)
            disconnect(retired, nullptr, this, nullptr);
        for (auto* pending : pending_)
            disconnect(pending, nullptr, this, nullptr);
    }

  private:
    attention::State* attention_state() const {
        return codex_state_ ? codex_state_.get() : claude_state_.get();
    }
    void start_claude(const LaunchSpec& launch) {
        claude_state_ = std::make_unique<attention::State>(
            identity_.session_id.toHex().toStdString(), "claude-code");
        claude_observer_ = std::make_unique<lapis::claude::Observer>(*claude_state_);
        connect(claude_observer_.get(), &lapis::claude::Observer::changed, this, [this] {
            attention_dirty_ = true;
            schedule_attention();
        });
        auto terminal = launch;
        terminal.agent = AgentMode::terminal;
        terminal.arguments = claude_observer_->launchArguments(
            launch.arguments, QCoreApplication::applicationFilePath());
        pty_requested_ = true;
        pty_.start(terminal);
    }
    void schedule_attention() {
        if (attention_state() && attention_dirty_ && client_ && ready_ && !stopping_ &&
            !attention_timer_.isActive())
            attention_timer_.start();
    }
    void publish_attention() {
        const auto* state = attention_state();
        if (!state || !client_ || !ready_ || stopping_)
            return;
        QLocalSocket* const destination = client_;
        const auto owner = attachment_;
        try {
            wire::AttentionSnapshot snapshot{.attachment = owner,
                                             .available = true,
                                             .connected = state->connected(),
                                             .ready = state->ready(),
                                             .source_epoch = state->epoch(),
                                             .activity = state->activity(),
                                             .diagnostic =
                                                 !decision_error_.isEmpty() ? decision_error_
                                                 : !codex_error_.isEmpty()  ? codex_error_
                                                 : codex_observer_ ? codex_observer_->diagnostic()
                                                                   : claude_observer_->diagnostic(),
                                             .requests = {}};
            for (const auto& [id, pending] : state->pending())
                snapshot.requests.push_back({pending, codex_observer_
                                                          ? codex_observer_->details(id)
                                                          : claude_observer_->details(id)});
            const auto bytes = wire::frame(wire::Kind::attention_snapshot,
                                           wire::encode_attention_snapshot(snapshot));
            if (destination->bytesToWrite() + bytes.size() > wire::max_frame_bytes ||
                destination->write(bytes) != bytes.size())
                throw std::runtime_error("Attention output queue unavailable");
            attention_dirty_ = false;
        } catch (const std::exception& error) {
            if (client_ == destination && attachment_ == owner) {
                send_status(destination, wire::StatusCode::overloaded,
                            QString::fromUtf8(error.what()));
                detach_client();
            }
        }
    }
    void stop_codex() {
        backend_wait_.stop();
        backend_probe_.abort();
        attention_timer_.stop();
        if (codex_observer_)
            codex_observer_->stop();
        backend_guard_control_.reset();
        backend_guard_read_.reset();
        if (codex_backend_.state() != QProcess::NotRunning &&
            !codex_backend_.waitForFinished(500)) {
            codex_backend_.kill();
            static_cast<void>(codex_backend_.waitForFinished(500));
        }
    }
    void start_codex_tui(const LaunchSpec& launch, const QString& backend_socket) {
        if (pty_requested_ || stopping_)
            return;
        auto tui = launch;
        tui.agent = AgentMode::terminal;
        tui.arguments.prepend(QStringLiteral("unix://") + backend_socket);
        tui.arguments.prepend(QStringLiteral("--remote"));
        pty_requested_ = true;
        pty_.start(tui);
    }
    static QStringList codex_arguments(const LaunchSpec& launch, const QString& backend_socket) {
        QStringList arguments{QStringLiteral("app-server"), QStringLiteral("--listen"),
                              QStringLiteral("unix://") + backend_socket};
        // Forward explicit provider/config definitions to the server as well as
        // the TUI. Production otherwise inherits the user's ordinary Codex policy.
        for (qsizetype index = 0; index < launch.arguments.size(); ++index) {
            const auto& argument = launch.arguments.at(index);
            if (argument == QStringLiteral("--"))
                break;
            if (argument == QStringLiteral("-c") || argument == QStringLiteral("--config") ||
                argument == QStringLiteral("--enable") || argument == QStringLiteral("--disable")) {
                ++index;
                if (index == launch.arguments.size() ||
                    launch.arguments.at(index) == QStringLiteral("--"))
                    throw std::invalid_argument("Codex config option requires a value");
                arguments << argument << launch.arguments.at(index);
            } else if (argument.startsWith(QStringLiteral("--config=")) ||
                       argument.startsWith(QStringLiteral("--enable=")) ||
                       argument.startsWith(QStringLiteral("--disable=")) ||
                       argument == QStringLiteral("--strict-config")) {
                arguments << argument;
            }
        }
        return arguments;
    }
    void start_codex(const QString& endpoint, const LaunchSpec& launch) {
        const auto backend_socket = posix::prepare_endpoint(endpoint + QStringLiteral(".codex"));
        if (QFileInfo::exists(backend_socket)) {
            QLocalSocket existing;
            existing.connectToServer(backend_socket);
            if (existing.waitForConnected(100) ||
                (existing.error() != QLocalSocket::ConnectionRefusedError &&
                 existing.error() != QLocalSocket::ServerNotFoundError))
                throw std::runtime_error("Codex endpoint already in use or inaccessible");
            if (!QFile::remove(backend_socket))
                throw std::runtime_error("Cannot remove stale Codex endpoint");
        }
        QFile executable(launch.program);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!executable.open(QIODevice::ReadOnly) || !hash.addData(&executable))
            throw std::runtime_error("Cannot identify Codex executable");
        const QString binary_hash = QString::fromLatin1(hash.result().toHex());
        codex_state_ =
            std::make_unique<attention::State>(identity_.session_id.toHex().toStdString(), "codex");
        codex_observer_ = std::make_unique<lapis::codex::Observer>(*codex_state_);
        connect(codex_observer_.get(), &lapis::codex::Observer::changed, this, [this] {
            decision_error_.clear();
            attention_dirty_ = true;
            schedule_attention();
        });
        const auto arguments = codex_arguments(launch, backend_socket);
        codex_backend_.setProgram(launch.program);
        codex_backend_.setArguments(arguments);
        codex_backend_.setWorkingDirectory(launch.directory);
        codex_backend_.setStandardInputFile(QProcess::nullDevice());
        codex_backend_.setStandardOutputFile(QProcess::nullDevice());
        codex_backend_.setStandardErrorFile(QProcess::nullDevice());
        const int descriptor_limit = ::getdtablesize();
        if (descriptor_limit <= 0 ||
            !posix::open_guard_pipe(backend_guard_read_, backend_guard_control_))
            throw std::runtime_error("Cannot create Codex process-group guard");
        const std::array<int, 2> guard{backend_guard_read_.get(), backend_guard_control_.get()};
        codex_backend_.setChildProcessModifier([guard, descriptor_limit] {
            if (::setsid() < 0 || !posix::start_group_guard(guard, descriptor_limit))
                ::_exit(127);
        });
        connect(&codex_backend_, &QProcess::started, this, [this] { backend_guard_read_.reset(); });
        connect(&codex_backend_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart)
                        stop(QStringLiteral("Could not start dedicated Codex server"));
                });
        connect(&codex_backend_, &QProcess::finished, this,
                [this](int code, QProcess::ExitStatus status) {
                    if (stopping_)
                        return;
                    stop_codex();
                    const QString exit_kind =
                        status == QProcess::CrashExit
                            ? QStringLiteral("exited abnormally (crash)")
                            : QStringLiteral("exited normally with code %1").arg(code);
                    codex_error_ =
                        QStringLiteral("Codex server %1; responses are disabled").arg(exit_kind);
                    attention_dirty_ = true;
                    schedule_attention();
                    if (!process_started_)
                        stop(codex_error_);
                });
        codex_backend_.start();
        wait_for_codex(launch, backend_socket, binary_hash);
    }
    void wait_for_codex(const LaunchSpec& launch, const QString& backend_socket,
                        const QString& binary_hash) {
        // A pathname can exist after bind(), before the server calls listen().
        // Probe asynchronously and launch the TUI only once a connection succeeds.
        connect(&backend_probe_, &QLocalSocket::connected, this,
                [this, launch, backend_socket, binary_hash] {
                    backend_wait_.stop();
                    backend_probe_.abort();
                    if (stopping_)
                        return;
                    codex_observer_->start(backend_socket, binary_hash);
                    start_codex_tui(launch, backend_socket);
                });
        backend_wait_.setInterval(backend_probe_interval_ms);
        connect(&backend_wait_, &QTimer::timeout, this,
                [this, backend_socket, attempts = 0]() mutable {
                    if (stopping_)
                        return;
                    if (++attempts >= backend_probe_attempts) {
                        stop(QStringLiteral("Dedicated Codex server did not become ready"));
                        return;
                    }
                    if (backend_probe_.state() == QLocalSocket::UnconnectedState)
                        backend_probe_.connectToServer(backend_socket);
                });
        backend_wait_.start();
    }
    void configure_history() {
        history_.failure = [this](const QString& error) {
            history_error_ = QStringLiteral("History recording paused: ") + error;
            history_failed_ = true;
            qWarning().noquote() << history_error_;
        };
        history_.progress = [this] {
            if (stopping_)
                return;
            try {
                try_pending_history();
                if (output_waiting_) {
                    output_waiting_ = false;
                    process_output();
                }
            } catch (const std::exception& error) {
                stop(QString::fromUtf8(error.what()));
            }
        };
        history_.received = [this](wire::HistoryReply reply) {
            if (!client_ || !ready_ || reply.attachment != attachment_)
                return;
            if (!history_error_.isEmpty())
                reply.message = history_error_ + QStringLiteral(". ") + reply.message;
            send_history(reply);
        };
    }
    static TerminalLimits limits() {
        TerminalLimits result;
        result.max_cells = wire::max_cells;
        result.max_grapheme_codepoints = wire::max_codepoints;
        result.max_input_bytes = std::size_t{64} * 1024U;
        return result;
    }
    static HistoryLimits history_limits() {
        HistoryLimits result;
        const auto read_limit = [](const char* name, quint64 fallback) {
            const auto value = qEnvironmentVariable(name);
            if (value.isEmpty())
                return fallback;
            bool valid{};
            const auto bytes = value.toULongLong(&valid);
            if (!valid || bytes == 0 || bytes > quint64{4} * 1024 * 1024 * 1024)
                throw std::invalid_argument("Invalid history byte budget");
            return bytes;
        };
        result.session_bytes = read_limit("LAPIS_HISTORY_SESSION_BYTES", result.session_bytes);
        result.global_bytes = read_limit("LAPIS_HISTORY_GLOBAL_BYTES", result.global_bytes);
        return result;
    }
    static TerminalSnapshot archive_page(TerminalSnapshot page, std::size_t rows) {
        page.size.rows = static_cast<std::uint16_t>(rows);
        page.cells.resize(rows * page.size.columns);
        std::size_t codepoints{};
        for (const auto& cell : page.cells)
            codepoints =
                std::max(codepoints, static_cast<std::size_t>(cell.text_offset) + cell.text_length);
        page.graphemes.resize(codepoints);
        page.cursor = {};
        page.history = {rows, 0, rows, true};
        return page;
    }
    bool archive_history(bool force) {
        if (history_failed_) {
            if (harvest_offset_) {
                terminal_.clear_history();
                harvest_offset_.reset();
            }
            return true;
        }
        // // Live memory remains bounded; the archive gap is explicit.
        const auto metadata = terminal_.history_metadata();
        if (!metadata.primary_available || metadata.total_rows <= metadata.viewport_rows)
            return true;
        const auto rows = metadata.total_rows - metadata.viewport_rows;
        const auto threshold =
            std::min(std::size_t{256}, std::size_t{32768} / current_size_.columns);
        if (!force && !harvest_offset_ && rows < threshold)
            return true;
        if (!harvest_offset_)
            harvest_offset_ = 0;
        // Keep the terminal frozen while enqueueing a large harvest in bounded
        // pieces. This also handles a one-row viewport and resize reflow.
        while (*harvest_offset_ < rows) {
            const auto count = std::min(metadata.viewport_rows, rows - *harvest_offset_);
            auto page = archive_page(terminal_.history_snapshot(*harvest_offset_), count);
            if (!history_.append({std::move(page)})) {
                pty_.pauseOutput(true);
                output_waiting_ = true;
                return false;
            }
            *harvest_offset_ += count;
        }
        harvest_offset_.reset();
        terminal_.clear_history();
        return true;
    }
    void process_output() {
        if (stopping_ || processing_output_)
            return;
        processing_output_ = true;
        try {
            if (harvest_offset_ && !archive_history(true)) {
                processing_output_ = false;
                return;
            }
            if (pending_resize_) {
                const auto size = *pending_resize_;
                pending_resize_.reset();
                apply_resize(size);
            }
            int budget = 16;
            while (!pending_output_.isEmpty() && budget-- > 0) {
                if (!archive_history(false)) {
                    output_waiting_ = true;
                    break;
                }
                const auto chunk = static_cast<qsizetype>(std::max(
                    std::size_t{1},
                    std::min(std::size_t{256},
                             std::size_t{32768} / (std::size_t{4} * current_size_.columns))));
                const auto size = std::min(pending_output_.size(), chunk);
                terminal_.feed(
                    std::string_view(pending_output_.constData(), static_cast<std::size_t>(size)));
                pending_output_.remove(0, size);
                dirty_ = true;
                timing_.parse_end_ns = monotonic_ns();
            }
            const auto replies = terminal_.take_replies();
            if (!replies.empty() && pty_.processId() != 0 &&
                !pty_.writeBytes(
                    QByteArray(replies.data(), static_cast<qsizetype>(replies.size()))))
                throw std::runtime_error("PTY reply queue overflow");
            schedule();
            if (!output_waiting_) {
                if (pending_output_.isEmpty())
                    pty_.pauseOutput(false);
                else
                    QTimer::singleShot(0, this, [this] { process_output(); });
            }
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
        }
        processing_output_ = false;
    }
    void send_history(const wire::HistoryReply& reply) {
        if (!client_ || !ready_ || reply.attachment != attachment_)
            return;
        try {
            auto bytes = wire::frame(wire::Kind::history_page, wire::encode_history_reply(reply));
            if (client_->bytesToWrite() + bytes.size() > wire::max_frame_bytes)
                throw std::runtime_error("History response queue full");
            if (client_->write(bytes) != bytes.size())
                throw std::runtime_error("History response write failed");
        } catch (const std::exception& error) {
            send_status(client_, wire::StatusCode::overloaded, QString::fromUtf8(error.what()));
            detach_client();
        }
    }
    void try_pending_history() {
        if (!pending_history_)
            return;
        const auto pending = *pending_history_;
        if (archive_history(true) && history_.read(pending.first, pending.second))
            pending_history_.reset();
    }
    void request_history(const QByteArray& payload) {
        const auto request = wire::decode_history_request(payload);
        if (pending_history_) {
            send_history({attachment_,
                          request.request_id,
                          0,
                          QStringLiteral("History is busy; try again"),
                          {}});
            return;
        }
        // Browsing retries storage after a recoverable filesystem failure.
        history_failed_ = false;
        history_error_.clear();
        pending_history_ = std::pair{attachment_, request};
        try_pending_history();
    }
    void finish_session(const QString& message, int exit_code, int remaining = 120) {
        if (stopping_)
            return;
        bool archived = false;
        try {
            archived = archive_history(true);
        } catch (const std::exception& error) {
            history_error_ = QString::fromUtf8(error.what());
            history_failed_ = true;
            archived = true;
        }
        if (remaining > 0 && (!archived || !history_.idle())) {
            QTimer::singleShot(25, this, [this, message, exit_code, remaining] {
                finish_session(message, exit_code, remaining - 1);
            });
            return;
        }
        if (!history_.idle())
            qWarning("History flush deadline reached; queued tail pages may be unavailable");
        stop(message, exit_code, std::chrono::milliseconds{0});
    }
    void stop(const QString& message, int exit_code = 1,
              std::chrono::milliseconds drain_timeout = std::chrono::seconds{3}) {
        if (stopping_)
            return;
        stopping_ = true;
        stop_codex();
        if (claude_observer_)
            claude_observer_->stop();
        pty_.pauseOutput(true);
        timer_.stop();
        ack_timer_.stop();
        pending_history_.reset();
        qInfo().noquote() << message;
        if (client_) {
            send_status(client_, wire::StatusCode::ended, message);
            client_->flush();
        }
        history_.drain(
            [this, exit_code] {
                if (!history_.idle())
                    qWarning("History shutdown deadline reached; queued tail may be unavailable");
                QTimer::singleShot(50, QCoreApplication::instance(),
                                   [exit_code] { QCoreApplication::exit(exit_code); });
            },
            static_cast<int>(drain_timeout.count()));
    }
    void attach() {
        auto* incoming = server_.nextPendingConnection();
        if (!incoming)
            return;
        connect(incoming, &QLocalSocket::disconnected, incoming, &QObject::deleteLater);
        if (stopping_ || pending_.size() >= 8) {
            if (!stopping_)
                send_status(incoming, wire::StatusCode::overloaded, "Too many pending attachments");
            incoming->disconnectFromServer();
            return;
        }
        pending_.insert(incoming);
        incoming->setReadBufferSize(75); // v3 attach is exactly 74 framed bytes.
        const auto bytes = std::make_shared<QByteArray>();
        connect(incoming, &QLocalSocket::readyRead, this,
                [this, incoming, bytes] { authenticate(incoming, *bytes); });
        connect(incoming, &QLocalSocket::disconnected, this,
                [this, incoming] { pending_.remove(incoming); });
        const QPointer<QLocalSocket> guarded(incoming);
        QTimer::singleShot(3000, this, [this, guarded] {
            if (guarded && pending_.contains(guarded))
                guarded->disconnectFromServer();
        });
        if (incoming->bytesAvailable())
            authenticate(incoming, *bytes);
    }
    void authenticate(QLocalSocket* incoming, QByteArray& bytes) {
        try {
            bytes += incoming->readAll();
            if (bytes.size() < 5)
                return;
            QDataStream header(bytes);
            quint32 frame_size{};
            quint8 kind{};
            header >> frame_size >> kind;
            if (frame_size != 70 || kind != static_cast<quint8>(wire::Kind::attach) ||
                bytes.size() > 74)
                throw std::runtime_error("Launch mismatch or incompatible attachment protocol");
            wire::Frame frame;
            if (!wire::take_frame(bytes, frame))
                return;
            // Preserve a useful incompatible-protocol diagnostic; decode_attach
            // also validates the version but reports generic malformed input.
            QDataStream attachment_header(frame.payload);
            quint32 protocol_version{};
            attachment_header >> protocol_version;
            if (protocol_version != wire::version)
                throw std::runtime_error("Launch mismatch or incompatible attachment protocol");
            const auto request = wire::decode_attach(frame.payload);
            if (request.fingerprint != fingerprint_)
                throw std::runtime_error("Launch mismatch or incompatible attachment protocol");
            if ((request.mode == wire::AttachMode::reconnect && request.expected != identity_) ||
                (request.mode == wire::AttachMode::create &&
                 request.expected.session_id != identity_.session_id)) {
                send_status(incoming, wire::StatusCode::replaced,
                            "Session identity mismatch: the endpoint belongs to another session");
                incoming->disconnectFromServer();
                return;
            }
            pending_.remove(incoming);
            disconnect(incoming, nullptr, this, nullptr);
            activate(incoming);
        } catch (const std::exception& error) {
            reject_attachment(incoming, QString::fromUtf8(error.what()));
        }
    }
    void activate(QLocalSocket* incoming) {
        if (generation_ == std::numeric_limits<quint64>::max()) {
            send_status(incoming, wire::StatusCode::overloaded, "Attachment generation overflow");
            incoming->disconnectFromServer();
            return;
        }
        ++generation_;
        attachment_ = {.identity = identity_, .generation = generation_};
        ready_ = false;
        snapshot_in_flight_ = false;
        ack_timer_.start(); // Bound synchronization even if hello cannot drain.
        if (client_) {
            send_status(client_, wire::StatusCode::replaced, "Session client replaced");
            disconnect(client_, nullptr, this, nullptr);
            retire(client_);
        }
        client_ = incoming;
        attention_dirty_ = true;
        if (codex_observer_ && pty_requested_ && !codex_state_->connected() &&
            codex_backend_.state() == QProcess::Running)
            codex_observer_->reconnect();
        buffer_.clear();
        incoming->setReadBufferSize(wire::max_frame_bytes + 4);
        connect(incoming, &QLocalSocket::readyRead, this, [this, incoming] {
            if (client_ == incoming)
                receive();
        });
        connect(incoming, &QLocalSocket::bytesWritten, this, [this] { schedule(); });
        connect(incoming, &QLocalSocket::disconnected, this, [this, incoming] {
            if (client_ == incoming)
                detach_client();
        });
        hello();
    }
    void hello() {
        if (!client_ || !process_started_ || stopping_)
            return;
        client_->write(wire::frame(
            wire::Kind::hello,
            wire::encode_hello({.attachment = attachment_, .pid = quint64(pty_.processId())})));
        dirty_ = true;
        schedule();
    }
    void schedule() {
        if (client_ && process_started_ && (!snapshot_in_flight_ || ready_) && dirty_ &&
            !timer_.isActive() && !stopping_)
            timer_.start();
    }
    void publish() {
        if (!client_ || client_->state() != QLocalSocket::ConnectedState || !dirty_)
            return;
        if (!ready_ && snapshot_in_flight_)
            return;
        if (client_->bytesToWrite() != 0)
            return; // one replaceable snapshot in flight
        try {
            if (snapshot_sequence_ == std::numeric_limits<quint64>::max())
                throw std::overflow_error("Snapshot sequence overflow");
            const quint64 sequence = ++snapshot_sequence_;
            timing_.publish_ns = monotonic_ns();
            const auto bytes =
                wire::frame(wire::Kind::snapshot,
                            wire::encode_snapshot_message({.attachment = attachment_,
                                                           .sequence = sequence,
                                                           .snapshot = terminal_.snapshot(),
                                                           .timing = timing_}));
            if (client_->write(bytes) < 0)
                throw std::runtime_error("Session socket write failed");
            dirty_ = false;
            if (!ready_) {
                ready_sequence_ = sequence;
                snapshot_in_flight_ = true;
            }
        } catch (const std::length_error&) {
            dirty_ = false;
            send_status(client_, wire::StatusCode::overloaded,
                        "Snapshot limit exceeded; session is still running");
            detach_client();
        } catch (const std::overflow_error&) {
            dirty_ = false;
            send_status(client_, wire::StatusCode::overloaded, "Snapshot sequence overflow");
            detach_client();
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
        }
    }
    void receive() {
        QLocalSocket* const receiving_client = client_;
        const wire::Attachment receiving_attachment = attachment_;
        if (!receiving_client || receiving_client->state() != QLocalSocket::ConnectedState)
            return;
        try {
            buffer_ += client_->read(wire::max_frame_bytes + 4 - buffer_.size());
            wire::Frame frame;
            qsizetype consumed{};
            for (int processed = 0; processed < 64; ++processed) {
                if (!wire::take_frame(buffer_, consumed, frame)) {
                    if (consumed != 0)
                        buffer_.remove(0, consumed);
                    return;
                }
                handle(frame);
                if (client_ != receiving_client || attachment_ != receiving_attachment ||
                    stopping_) {
                    return;
                }
            }
            if (buffer_.size() > wire::max_frame_bytes + 4)
                throw std::runtime_error("Session input overflow");
            if (consumed != 0)
                buffer_.remove(0, consumed);
            const auto attachment = client_;
            QTimer::singleShot(0, this, [this, attachment] {
                if (attachment && client_ == attachment)
                    receive();
            });
        } catch (const std::exception& error) {
            if (client_ == receiving_client && attachment_ == receiving_attachment && !stopping_) {
                send_status(client_, wire::StatusCode::rejected, QString::fromUtf8(error.what()));
                if (client_ == receiving_client && attachment_ == receiving_attachment)
                    detach_client();
            }
        }
    }
    void acknowledge(const QByteArray& payload) {
        const auto ready = wire::decode_ready(payload);
        if (ready.attachment != attachment_ || ready.sequence != ready_sequence_)
            throw std::runtime_error("Stale or mismatched ready acknowledgement");
        if (!ready_ && !snapshot_in_flight_)
            throw std::runtime_error("Unexpected ready acknowledgement");
        ready_ = true;
        snapshot_in_flight_ = false;
        ack_timer_.stop();
        schedule_attention();
        schedule();
    }
    void allow_decision_retry(wire::AttentionDecision decision) {
        if (!codex_state_ || !codex_state_->ready() ||
            codex_state_->epoch() != decision.source_epoch)
            return;
        const auto found = codex_state_->pending().find(decision.request_id);
        if (found == codex_state_->pending().end())
            return;
        const auto& pending = found->second;
        if (pending.revision != decision.revision || pending.submitted ||
            pending.status != attention::RequestStatus::pending)
            return;
        // This is an explicit rejection before source submission. An older
        // queued snapshot alone must never unblock the desktop's duplicate guard.
        decision.answers = {};
        const auto bytes = wire::frame(
            wire::Kind::attention_retry,
            wire::encode_control({attachment_, wire::encode_attention_decision(decision)}));
        if (client_->bytesToWrite() + bytes.size() > wire::max_frame_bytes ||
            client_->write(bytes) != bytes.size())
            throw std::runtime_error("Decision rejection queue failed");
    }
    void handle(const wire::Frame& frame) {
        if (!process_started_ || stopping_)
            throw std::runtime_error("Session is not ready for input");
        if (frame.kind == wire::Kind::ready) {
            acknowledge(frame.payload);
            return;
        }
        const auto control = wire::decode_control(frame.payload);
        if (control.attachment != attachment_ || !ready_)
            throw std::runtime_error("Stale attachment or session is not ready for input");
        if (control.payload.size() > qsizetype{64} * 1024)
            throw std::runtime_error("Input message too large");
        QByteArray bytes;
        switch (frame.kind) {
        case wire::Kind::attention_decision: {
            if (!codex_observer_)
                throw std::runtime_error(
                    "Attention decisions are unsupported for terminal sessions");
            const auto decision = wire::decode_attention_decision(control.payload);
            if (!codex_observer_->decide(decision.source_epoch, decision.request_id,
                                         decision.revision, decision.choice, decision.answers)) {
                allow_decision_retry(decision);
                decision_error_ =
                    QStringLiteral("Request changed or response is unsupported; refresh attention");
                attention_dirty_ = true;
                schedule_attention();
            }
            return;
        }
        case wire::Kind::history_request:
            request_history(control.payload);
            return;
        case wire::Kind::text:
            bytes = control.payload;
            break;
        case wire::Kind::paste: {
            const auto encoded = terminal_.encode_paste(std::string_view(
                control.payload.constData(), static_cast<std::size_t>(control.payload.size())));
            bytes = QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size()));
            break;
        }
        case wire::Kind::key: {
            if (control.payload.size() != 2)
                throw std::runtime_error("Invalid key message");
            const auto key_value = static_cast<unsigned char>(control.payload[0]);
            if (key_value > static_cast<unsigned char>(TerminalKey::escape))
                throw std::runtime_error("Unknown key code");
            const auto key = static_cast<TerminalKey>(key_value);
            const auto mods = static_cast<unsigned char>(control.payload[1]);
            const auto encoded = terminal_.encode_key(
                key, {(mods & 1U) != 0, (mods & 2U) != 0, (mods & 4U) != 0, (mods & 8U) != 0});
            bytes = QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size()));
            break;
        }
        case wire::Kind::resize: {
            if (control.payload.size() != 4)
                throw std::runtime_error("Invalid resize message");
            QDataStream in(control.payload);
            quint16 columns{}, rows{};
            in >> columns >> rows;
            TerminalSize size{columns, rows};
            if (columns == 0 || rows == 0 || quint32(columns) * rows > wire::max_cells)
                throw std::runtime_error("Invalid terminal geometry");
            if (harvest_offset_)
                pending_resize_ = size;
            else
                apply_resize(size);
            return;
        }
        default:
            throw std::runtime_error("Unexpected client message");
        }
        if (!pty_.writeBytes(bytes))
            throw std::runtime_error("PTY input queue full");
    }
    void apply_resize(TerminalSize size) {
        if (!pty_.resize(size))
            throw std::runtime_error("PTY resize failed");
        try {
            terminal_.resize(size);
            current_size_ = size;
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
            return; // A failed engine resize cannot be presented as synchronized.
        }
        const auto replies = terminal_.take_replies();
        if (!replies.empty() &&
            !pty_.writeBytes(QByteArray(replies.data(), static_cast<qsizetype>(replies.size()))))
            throw std::runtime_error("PTY reply queue overflow");
        dirty_ = true;
        schedule();
    }
    void send_status(QLocalSocket* socket, wire::StatusCode code, const QString& message) const {
        socket->write(wire::frame(wire::Kind::status,
                                  wire::encode_status({.code = code, .message = message})));
    }
    void reject_attachment(QLocalSocket* incoming, const QString& message = {}) {
        send_status(incoming, wire::StatusCode::rejected,
                    message.isEmpty() ? QStringLiteral("Invalid attachment request") : message);
        incoming->disconnectFromServer();
    }
    void retire(QLocalSocket* socket) {
        // Let a typed final status drain, but bound both time and retired sockets.
        if (retired_.size() >= 8) {
            auto* oldest = *retired_.begin();
            retired_.remove(oldest);
            oldest->abort();
            oldest->deleteLater();
        }
        retired_.insert(socket);
        connect(socket, &QObject::destroyed, this, [this, socket] { retired_.remove(socket); });
        socket->disconnectFromServer();
        if (socket->state() == QLocalSocket::UnconnectedState)
            socket->deleteLater();
        else
            QTimer::singleShot(1000, socket, &QObject::deleteLater);
    }
    void detach_client() {
        if (!client_)
            return;
        disconnect(client_, nullptr, this, nullptr);
        retire(client_);
        client_ = nullptr;
        pending_history_.reset();
        buffer_.clear();
        ready_ = false;
        snapshot_in_flight_ = false;
        ack_timer_.stop();
    }
    QLockFile lock_;
    Terminal terminal_;
    posix::PtyProcess pty_;
    QLocalServer server_;
    QPointer<QLocalSocket> client_;
    QSet<QLocalSocket*> pending_;
    QSet<QLocalSocket*> retired_;
    QByteArray fingerprint_;
    wire::SessionIdentity identity_;
    wire::Attachment attachment_;
    QByteArray buffer_;
    QTimer timer_;
    QTimer ack_timer_;
    quint64 generation_{};
    quint64 snapshot_sequence_{};
    quint64 ready_sequence_{};
    bool dirty_{true};
    bool ready_{};
    bool snapshot_in_flight_{};
    bool process_started_{};
    bool stopping_{};
    HistoryWorker history_;
    QByteArray pending_output_;
    QString history_error_;
    bool processing_output_{};
    bool output_waiting_{};
    bool history_failed_{};
    std::optional<std::pair<wire::Attachment, wire::HistoryRequest>> pending_history_;
    std::optional<std::size_t> harvest_offset_;
    std::optional<TerminalSize> pending_resize_;
    TerminalSize current_size_;
    wire::SnapshotTiming timing_;
    std::unique_ptr<attention::State> claude_state_;
    std::unique_ptr<lapis::claude::Observer> claude_observer_;
    std::unique_ptr<attention::State> codex_state_;
    std::unique_ptr<lapis::codex::Observer> codex_observer_;
    QProcess codex_backend_;
    QTimer backend_wait_;
    QLocalSocket backend_probe_;
    QTimer attention_timer_;
    posix::UniqueFd backend_guard_read_;
    posix::UniqueFd backend_guard_control_;
    bool pty_requested_{};
    bool attention_dirty_{};
    QString codex_error_;
    QString decision_error_;
};
} // namespace
int main(int argc, char** argv) {
    QStringList arguments;
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));
    int application_argc = 1;
    QCoreApplication app(application_argc, argv);
    if (arguments.value(1) == QStringLiteral("--claude-hook")) {
        if (arguments.size() != 4)
            return 0;
        return lapis::claude::run_hook_relay(arguments.at(2), arguments.at(3));
    }
    try {
        QByteArray session_id;
        auto agent = AgentMode::terminal;
        qsizetype socket_index = 1;
        while (socket_index < arguments.size()) {
            const auto& option = arguments.at(socket_index);
            if (option == QStringLiteral("--session-id")) {
                if (!session_id.isEmpty() || socket_index + 1 >= arguments.size())
                    throw std::invalid_argument("Expected one --session-id HEX32");
                session_id = parse_session_id(arguments.at(++socket_index));
            } else if (option == QStringLiteral("--codex") ||
                       option == QStringLiteral("--claude")) {
                if (agent != AgentMode::terminal)
                    throw std::invalid_argument("Expected one agent mode");
                agent = option == QStringLiteral("--codex") ? AgentMode::codex : AgentMode::claude;
            } else {
                break;
            }
            ++socket_index;
        }
        if (arguments.size() - socket_index < 3)
            throw std::invalid_argument(
                "Usage: lapis_session_service [--session-id HEX32] [--codex | --claude] "
                "SOCKET DIRECTORY PROGRAM [ARG ...]");
        if (session_id.isEmpty())
            session_id = wire::new_id();
        const auto program_index = socket_index + 2;
        const auto launch = validate_launch({.program = arguments.at(program_index),
                                             .arguments = arguments.mid(program_index + 1),
                                             .directory = arguments.at(socket_index + 1),
                                             .agent = agent});
        SessionService service(posix::prepare_endpoint(arguments.at(socket_index)), session_id,
                               launch);
        return app.exec();
    } catch (const std::exception& error) {
        qCritical().noquote() << error.what();
        return 1;
    }
}
