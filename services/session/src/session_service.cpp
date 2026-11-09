#include "agent_checkpoint.hpp"
#include "attention_audit.hpp"
#include "attention_journal.hpp"
#include "claude_observer.hpp"
#include "history_worker.hpp"
#include "hook_relay.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/pty_process.hpp"
#include "rewidth.hpp"
#include "terminal_hook_relay.hpp"
#include "terminal_hooks.hpp"
#include "transport/attention_protocol.hpp"
#include "transport/local_protocol.hpp"
#include "transport/update_pacing.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QRandomGenerator>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace lapis::session {
class AttentionServiceTestAccess;
}

namespace {
using namespace lapis::session;
constexpr int terminal_sync_timeout_ms = 3000;
// Admission is atomic at the 64 KiB service boundary once sustained pressure
// opens. Until then ordinary batches parse immediately, so pressure mode has a
// hard retained bound of one admission limit plus one maximum PTY batch.
constexpr qsizetype output_admission_limit = qsizetype{64} * 1024;
// A quiet batch belongs to interactive output and parses without an admission
// delay. This much output in one frame is sustained pressure, so the bounded
// 64 KiB admission window may batch it instead.
constexpr qsizetype output_pressure_open_limit = qsizetype{16} * 1024;
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
// The Codex app-server earlier services ran for a Codex agent listened beside
// the endpoint, through a symlink Codex 0.156 removes on a clean exit. A
// power loss leaves the link or socket behind; nothing listens there now.
void remove_stale_codex_endpoint(const QString& path) {
    const QFileInfo info(path);
    if (!info.isSymLink() && !info.exists())
        return;
    QLocalSocket existing;
    existing.connectToServer(path);
    if (!existing.waitForConnected(100))
        static_cast<void>(QFile::remove(path));
}
// Whether Codex's arguments set a config key with -c/--config before `--`.
bool codex_sets(const QStringList& arguments, QLatin1StringView key) {
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments.at(index);
        if (argument == QLatin1String("--"))
            break; // Following words are literal prompt data.
        QString setting;
        if ((argument == QLatin1String("-c") || argument == QLatin1String("--config")) &&
            index + 1 < arguments.size())
            setting = arguments.at(++index);
        else if (argument.startsWith(QLatin1String("--config=")))
            setting = argument.sliced(9);
        else if (argument.startsWith(QLatin1String("-c")) && argument.size() > 2)
            setting = argument.sliced(2);
        if (setting.section(QLatin1Char('='), 0, 0).trimmed() == key)
            return true;
    }
    return false;
}
// A fresh 32-hex-digit nonce for the terminal hook channel.
QString hook_nonce() {
    std::array<quint32, 4> words{};
    QRandomGenerator::system()->fillRange(words.data(), words.size());
    QString text;
    for (const auto word : words)
        text += QStringLiteral("%1").arg(word, 8, 16, QLatin1Char('0'));
    return text;
}

class SessionService final : public QObject {
  public:
    SessionService(const QString& endpoint, const QByteArray& requested_session_id,
                   const LaunchSpec& launch, const QByteArray& requested_session_epoch = {})
        : lock_(endpoint + QStringLiteral(".lock")), terminal_(launch.size, limits()),
          fingerprint_(launch_fingerprint(launch)),
          identity_{requested_session_id, resolve_session_epoch(requested_session_epoch)},
          history_(history_root(endpoint), QString::fromLatin1(requested_session_id.toHex()),
                   history_limits()) {
        configure_history();
        resume_endpoint_ = endpoint;
        checkpoint_agent_ = checkpoint_agent_for_launch(launch);
        if (auto saved = read_resume_record(endpoint))
            resume_ = *saved;
        current_size_ = launch.size;
        terminal_.feed("\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\");
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock(0))
            throw std::runtime_error("Session service already owns endpoint");
        // Only the endpoint owner may trim its append log; racing GUI launches
        // must not truncate the log of an already-running service.
        QFile log(endpoint + QStringLiteral(".log"));
        trim_endpoint_log(log);
        if (launch.agent != AgentMode::terminal)
            open_attention_journal(endpoint);
        claude_ = launch.agent == AgentMode::claude;
        bind_endpoint(endpoint);
        snapshot_clock_.start();
        attention_clock_.start();
        configure_timers();
        connect_session_signals();
        start_agent(endpoint, launch);
    }

    ~SessionService() override {
        stopping_ = true;
        attention_timer_.stop();
        if (claude_observer_)
            claude_observer_->stop();
        disconnect(&pty_, nullptr, this, nullptr);
        disconnect(&server_, nullptr, this, nullptr);
        disconnect(&timer_, nullptr, this, nullptr);
        if (client_)
            disconnect(client_, nullptr, this, nullptr);
        for (const auto& view : views_)
            if (view->socket)
                disconnect(view->socket, nullptr, this, nullptr);
        for (auto* retired : retired_)
            disconnect(retired, nullptr, this, nullptr);
        for (auto* pending : pending_)
            disconnect(pending, nullptr, this, nullptr);
    }

    friend class ::lapis::session::AttentionServiceTestAccess;

  private:
    // A service without a requested epoch starts a new identity.
    static QByteArray resolve_session_epoch(const QByteArray& requested_session_epoch) {
        return requested_session_epoch.isEmpty() ? wire::new_id() : requested_session_epoch;
    }
    static void trim_endpoint_log(QFile& log) {
        if (log.size() > qint64{1024} * 1024 && log.open(QIODevice::ReadWrite))
            static_cast<void>(log.resize(0));
    }
    // Refuse to start while another service still owns the endpoint socket.
    static void reject_existing_endpoint(const QString& endpoint) {
        if (!QFileInfo::exists(endpoint))
            return;
        QLocalSocket existing;
        existing.connectToServer(endpoint);
        if (existing.waitForConnected(100) ||
            (existing.error() != QLocalSocket::ConnectionRefusedError &&
             existing.error() != QLocalSocket::ServerNotFoundError))
            throw std::runtime_error("Socket endpoint is already in use or inaccessible");
    }
    // The bound socket must be a Unix socket owned by this user and must not
    // be reachable by anyone else.
    static void require_private_bound_socket(const QString& endpoint) {
        struct stat bound_socket{};
        if (::lstat(QFile::encodeName(endpoint).constData(), &bound_socket) != 0 ||
            !S_ISSOCK(bound_socket.st_mode) || bound_socket.st_uid != ::getuid() ||
            (static_cast<unsigned int>(bound_socket.st_mode) & 0077U) != 0U)
            throw std::runtime_error("Bound socket is not private to the current user");
    }
    // Claims the endpoint socket for this service.
    void bind_endpoint(const QString& endpoint) {
        reject_existing_endpoint(endpoint);
        QLocalServer::removeServer(endpoint);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(endpoint))
            throw std::runtime_error(server_.errorString().toStdString());
        require_private_bound_socket(endpoint);
    }
    // Arms the service's periodic clocks.
    void configure_timers() {
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] { publish_paced_output(); });
        settle_timer_.setSingleShot(true);
        connect(&settle_timer_, &QTimer::timeout, this, [this] { publish_paced_output(); });
        ack_timer_.setSingleShot(true);
        ack_timer_.setInterval(terminal_sync_timeout_ms);
        attention_timer_.setSingleShot(true);
        attention_timer_.setInterval(16);
        connect(&attention_timer_, &QTimer::timeout, this, [this] { publish_attention(); });
    }
    // Wires the service's timers, socket and PTY to their handlers.
    void connect_session_signals() {
        connect(&ack_timer_, &QTimer::timeout, this, [this] {
            if (client_ && !ready_) {
                send_status(client_, wire::StatusCode::rejected,
                            "Attachment ready acknowledgement timed out");
                detach_client();
            }
        });
        connect(&server_, &QLocalServer::newConnection, this, [this] { attach(); });
        output_admission_timer_.setSingleShot(true);
        connect(&output_admission_timer_, &QTimer::timeout, this, [this] {
            output_admission_expired_ = true;
            process_output();
        });
        connect(&pty_, &posix::PtyProcess::started, this, [this] {
            process_started_ = true;
            hello();
        });
        connect(&pty_, &posix::PtyProcess::output, this,
                [this](const QByteArray& bytes) { receive_output(bytes); });
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
    }
    // Starts the agent this service supervises.
    void start_agent(const QString& endpoint, const LaunchSpec& launch) {
        if (remote_transport(launch) || omp_launch(launch))
            terminal_hooks_.emplace();
        if (launch.agent == AgentMode::codex) {
            remove_stale_codex_endpoint(endpoint + QStringLiteral(".codex"));
            start_codex(launch);
        } else if (launch.agent == AgentMode::claude)
            start_claude(launch);
        else
            pty_.start(launch);
    }
    // An agent on another machine reports its hooks through this terminal;
    // see terminal_hooks.hpp.
    static bool remote_transport(const LaunchSpec& launch) {
        return launch.agent == AgentMode::terminal &&
               QFileInfo(launch.program).fileName() == QStringLiteral("ssh");
    }
    // A local OMP agent reports its turns through the same terminal sequences,
    // printed by the extension lapis's OMP overlay loads.
    static bool omp_launch(const LaunchSpec& launch) {
        return launch.agent == AgentMode::terminal &&
               QFileInfo(launch.program).fileName() == QStringLiteral("omp");
    }
    const attention::State* attention_state() const {
        return claude_state_ ? claude_state_.get() : notify_state_.get();
    }
    QString attention_diagnostic() const {
        return claude_observer_ ? claude_observer_->diagnostic()
               : notify_turns_  ? notify_turns_->diagnostic()
                                : QStringLiteral("Codex notify turns are not connected; status is "
                                                 "estimated from output");
    }
    QJsonObject attention_details(const attention::RequestId& id) const {
        return claude_observer_ ? claude_observer_->details(id) : QJsonObject{};
    }
    void observer_changed() {
        decision_error_.clear();
        attention_dirty_ = true;
        journal_attention();
        schedule_attention();
    }
    // A hook relayed through the terminal: from another machine, from lapis's
    // OMP extension, or from a local Codex agent's notify program. The service
    // owns the attention state as for a local agent. A remote launch names its
    // conversation in its own command, so only a local Codex agent's notify
    // event records a resume identity (see note_codex_thread).
    void receive_terminal_hook(const TerminalHookEvent& hook) {
        if (hook.cli == QLatin1String("claude")) {
            if (!claude_observer_) {
                claude_state_ = std::make_unique<attention::State>(
                    identity_.session_id.toHex().toStdString(), "claude-code");
                claude_observer_ = std::make_unique<lapis::claude::Observer>(
                    *claude_state_, lapis::claude::Observer::Transport::terminal);
                connect(claude_observer_.get(), &lapis::claude::Observer::changed, this,
                        [this] { observer_changed(); });
            }
            claude_observer_->receiveRelayed(hook.source);
        } else if (hook.cli == QLatin1String("codex")) {
            if (!notify_turns_) {
                notify_state_ = std::make_unique<attention::State>(
                    identity_.session_id.toHex().toStdString(), "codex-notify");
                notify_turns_ = std::make_unique<NotifyTurns>(*notify_state_);
            }
            if (codex_notify_)
                note_codex_thread(hook.source.value(QStringLiteral("thread-id")).toString());
            if (notify_turns_->completed(hook.source, attention_tick()))
                observer_changed();
        } else if (hook.cli == QLatin1String("omp")) {
            // agent_end ends the person's turn; the start of the next one is
            // their submitted input, as for Codex (note_submitted).
            if (!notify_turns_) {
                notify_state_ = std::make_unique<attention::State>(
                    identity_.session_id.toHex().toStdString(), "omp-extension");
                notify_turns_ =
                    std::make_unique<NotifyTurns>(*notify_state_, QStringLiteral("OMP extension"));
            }
            const auto session = hook.source.value(QStringLiteral("session_id")).toString();
            if (!session.isEmpty()) {
                omp_extension_ = true;
                note_conversation(QStringLiteral("omp"), session, ResumeSource::terminal);
            }
            if (hook.source.value(QStringLiteral("type")) == QLatin1String("agent_end") &&
                notify_turns_->completed(
                    {{QStringLiteral("type"), QStringLiteral("agent-turn-complete")}},
                    attention_tick()))
                observer_changed();
        }
    }
    static attention::Tick attention_tick() {
        return static_cast<attention::Tick>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count());
    }
    // Submitted input starts a turn Codex's notify program will not report.
    void note_submitted() {
        if (notify_turns_ && notify_turns_->submitted())
            observer_changed();
    }
    void start_claude(const LaunchSpec& launch) {
        claude_state_ = std::make_unique<attention::State>(
            identity_.session_id.toHex().toStdString(), "claude-code");
        claude_observer_ = std::make_unique<lapis::claude::Observer>(*claude_state_);
        connect(claude_observer_.get(), &lapis::claude::Observer::changed, this, [this] {
            note_conversation(QStringLiteral("claude"), claude_observer_->sessionId());
            observer_changed();
        });
        auto terminal = launch;
        terminal.agent = AgentMode::terminal;
        terminal.arguments = claude_observer_->launchArguments(
            launch.arguments, QCoreApplication::applicationFilePath());
        pty_.start(terminal);
    }
    void receive_output(const QByteArray& output) {
        timing_.pty_read_ns = monotonic_ns();
        QByteArray bytes = output;
        if (terminal_hooks_) {
            std::vector<TerminalHookEvent> hooks;
            bytes = terminal_hooks_->filter(output, hooks);
            for (const auto& hook : hooks)
                receive_terminal_hook(hook);
            if (bytes.isEmpty())
                return;
        }
        pending_output_ += bytes;
        observe_output_pressure(bytes);
        if (output_pressure_) {
            output_admission_timer_.stop();
            output_admission_expired_ = false;
            pty_.pauseOutput(true);
            if (pending_output_.size() < output_admission_limit) {
                if (!output_admission_timer_.isActive())
                    output_admission_timer_.start(static_cast<int>(frame_ms));
                schedule_output_admission();
            } else {
                if (!output_retention_logged_) {
                    output_retention_logged_ = true;
                    qWarning().noquote()
                        << "PTY output retention paused at" << output_admission_limit
                        << "bytes; the child remains alive and output stays ordered";
                }
                schedule_output_processing();
            }
        } else {
            schedule_output_processing();
        }
        // OMP subagents print their own checkpoints into the same terminal;
        // once lapis's extension reports the main session, those cannot win.
        if (const auto found = checkpoint_scanner_.scan(bytes);
            found && found->agent == checkpoint_agent_ && !omp_extension_)
            note_conversation(found->agent, found->session_id, ResumeSource::terminal);
    }
    void observe_output_pressure(const QByteArray& bytes) {
        if (!output_pressure_clock_.isValid() || output_pressure_clock_.elapsed() > frame_ms) {
            output_pressure_clock_.start();
            output_pressure_bytes_ = 0;
        }
        output_pressure_bytes_ += bytes.size();
        if (output_pressure_bytes_ >= output_pressure_open_limit)
            output_pressure_ = true;
    }
    void schedule_output_admission() {
        if (output_admission_scheduled_)
            return;
        output_admission_scheduled_ = true;
        QTimer::singleShot(0, this, [this] {
            output_admission_scheduled_ = false;
            if (stopping_ || output_waiting_ || pending_output_.size() >= output_admission_limit)
                return;
            pty_.pauseOutput(false);
        });
    }
    void schedule_output_processing() {
        if (output_processing_scheduled_)
            return;
        output_processing_scheduled_ = true;
        QTimer::singleShot(0, this, [this] {
            output_processing_scheduled_ = false;
            process_output();
        });
    }
    // The conversation to resume if this service stops without the agent being
    // closed (see agent_checkpoint.hpp). Failure to save is not fatal.
    void note_conversation(const QString& agent, const QString& session_id,
                           ResumeSource source = ResumeSource::observer) {
        // Printed bytes cannot impersonate the managed observer or downgrade
        // an identity already learned through its independent protocol or an
        // older Codex/Claude legacy record.
        if (source == ResumeSource::terminal &&
            (codex_notify_ || claude_observer_ || resume_.source == ResumeSource::observer ||
             (resume_.source == ResumeSource::legacy && observer_backed_agent(checkpoint_agent_))))
            return;
        if (!valid_resume_identity(session_id) ||
            (resume_.agent == agent && resume_.session_id == session_id &&
             resume_.source == source))
            return;
        const ResumeRecord candidate{agent, session_id, source};
        try {
            write_resume_record(resume_endpoint_, candidate);
            resume_ = candidate;
        } catch (const std::exception& error) {
            qWarning().noquote() << "Resume record not saved:" << error.what();
        }
    }
    void schedule_attention() {
        if (attention_state() && attention_dirty_ && client_ && ready_ && !stopping_ &&
            !attention_timer_.isActive()) {
            attention_timer_.start(
                static_cast<int>(attention_pace_.wait_ms(attention_clock_.elapsed())));
        }
    }
    // The durable audit trail for attention requests. Surfacing needs no
    // barrier because surfacing never approves, and lapis forwards no
    // decisions. A question left open by a previous run, including one an
    // older service forwarded a decision for, ends with an unknown outcome.
    void open_attention_journal(const QString& endpoint) {
        try {
            attention_journal_ = std::make_unique<AttentionJournal>(std::filesystem::path(
                QFile::encodeName(endpoint + QStringLiteral(".attention")).constData()));
        } catch (const std::exception& error) {
            qWarning().noquote() << "Attention journal unavailable:" << error.what();
            attention_journal_failed_ = true;
            return;
        }
        const auto open = open_questions(attention_journal_->entries());
        for (const auto& question : open) {
            try {
                attention_journal_->append(outcome_unknown_for(question));
            } catch (const std::exception& error) {
                qWarning().noquote() << "Attention outcome not recorded:" << error.what();
                attention_journal_failed_ = true;
                break;
            }
        }
        attention_journal_->release_entries();
        if (!open.empty())
            qWarning().noquote() << open.size()
                                 << "attention request(s) from a previous run ended with an "
                                    "unknown outcome; see the attention journal";
    }
    // Audit hook for source state changes and the coalesced publish. Asked
    // records are soft:
    // a failed append never hides a request from the user. A journaled id that
    // leaves the pending set is recorded as resolved by the agent; a failed
    // append leaves the id journaled so the next publish retries it.
    void journal_attention() {
        const auto* state = attention_state();
        if (!state || !attention_journal_ || attention_journal_failed_)
            return;
        try {
            lapis::session::journal_attention(*attention_journal_, *state, journaled_);
            journal_append_failed_ = false;
        } catch (const std::exception& error) {
            if (!journal_append_failed_) {
                journal_append_failed_ = true;
                qWarning().noquote() << "Attention journal append failed:" << error.what();
            }
        }
    }
    void publish_attention() {
        const auto* state = attention_state();
        if (!state || !client_ || !ready_ || stopping_)
            return;
        QLocalSocket* const destination = client_;
        const auto owner = attachment_;
        journal_attention();
        try {
            wire::AttentionSnapshot snapshot{
                .attachment = owner,
                .available = true,
                .connected = state->connected(),
                .ready = state->ready(),
                .source_epoch = state->epoch(),
                .activity = state->activity(),
                .observation_phase = attention::ObservationPhase::unknown,
                .diagnostic = !decision_error_.isEmpty() ? decision_error_ : attention_diagnostic(),
                .requests = {}};
            for (const auto& [id, pending] : state->pending())
                snapshot.requests.push_back({pending, attention_details(id)});
            const auto bytes =
                wire::frame(wire::Kind::attention_snapshot,
                            wire::encode_attention_snapshot(snapshot, client_attention_phase_));
            if (destination->bytesToWrite() + bytes.size() > wire::max_frame_bytes ||
                destination->write(bytes) != bytes.size())
                throw std::runtime_error("Attention output queue unavailable");
            attention_pace_.published(attention_clock_.elapsed());
            attention_dirty_ = false;
        } catch (const std::exception& error) {
            if (client_ == destination && attachment_ == owner) {
                send_status(destination, wire::StatusCode::overloaded,
                            QString::fromUtf8(error.what()));
                detach_client();
            }
        }
    }
    // A local Codex agent is Codex's own TUI. After each finished turn Codex
    // runs its notify program: lapis's relay, which writes the event to the
    // agent's terminal with a nonce this service made and put only in the
    // agent's environment, and then runs the person's own notify program from
    // config.toml. TerminalHookChannel takes the event out of the output. When
    // the arguments set notify themselves, nothing is added and the status
    // stays estimated from output.
    void start_codex(const LaunchSpec& launch) {
        auto tui = launch;
        tui.agent = AgentMode::terminal;
        if (!codex_sets(tui.arguments, QLatin1StringView("notify"))) {
            const auto nonce = hook_nonce();
            terminal_hooks_.emplace();
            if (!terminal_hooks_->bind("codex", nonce.toLatin1()))
                throw std::logic_error("Codex notify nonce did not bind");
            codex_notify_ = true;
            // Codex takes -c before any subcommand: first.
            tui.arguments.prepend(QStringLiteral(
                R"(notify=["sh","-c","exec python3 -c \"$LAPIS_HOOK_RELAY\" codex \"$1\" 2>/dev/null","lapis"])"));
            tui.arguments.prepend(QStringLiteral("-c"));
            pty_.setEnvironment(
                {{QStringLiteral("LAPIS_HOOK_NONCE"), nonce},
                 {QStringLiteral("LAPIS_HOOK_RELAY"), QString::fromUtf8(kTerminalHookRelay)}},
                QStringLiteral("LAPIS_HOOK_TTY"));
        }
        pty_.start(tui);
    }
    // The conversation a local Codex agent's notify event names, kept for
    // resume. The nonce is never printed, so the event is as trustworthy as an
    // observer. Subagent threads report their own finished turns; their
    // rollout says so, and `codex resume` of one is not this agent's
    // conversation. A thread whose rollout cannot be read yet is not recorded
    // until a later turn finds it.
    void note_codex_thread(const QString& thread) {
        if (thread.isEmpty() || subagent_threads_.contains(thread) ||
            (thread == resume_.session_id && resume_.source == ResumeSource::observer))
            return;
        const auto main_thread = codex_main_thread(codex_home(), thread);
        if (!main_thread)
            return;
        if (!*main_thread) {
            if (subagent_threads_.size() < 256)
                subagent_threads_.insert(thread);
            return;
        }
        note_conversation(QStringLiteral("codex"), thread, ResumeSource::observer);
    }
    static QString codex_home() {
        return qEnvironmentVariable("CODEX_HOME", QDir::homePath() + QStringLiteral("/.codex"));
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
                if (!try_rewidth_archive())
                    return;
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
            if (!history_error_.isEmpty())
                reply.message = history_error_ + QStringLiteral(". ") + reply.message;
            send_history(reply);
        };
    }
    static TerminalLimits limits() {
        TerminalLimits result;
        result.max_cells = wire::max_cells;
        result.max_grapheme_codepoints = wire::max_codepoints;
        result.max_input_bytes = static_cast<std::size_t>(wire::max_paste_bytes);
        return result;
    }
    // Beside the service's endpoint, in lapis's runtime folder, unless
    // LAPIS_HISTORY_ROOT names another; never a path fixed when it was built.
    static QString history_root(const QString& endpoint) {
        const auto chosen = qEnvironmentVariable("LAPIS_HISTORY_ROOT");
        return chosen.isEmpty()
                   ? QFileInfo(endpoint).absoluteDir().filePath(QStringLiteral("history"))
                   : chosen;
    }
    static HistoryLimits history_limits() {
        HistoryLimits result;
        const auto read_limit = [](const char* name, quint64 fallback) {
            const auto value = qEnvironmentVariable(name);
            if (value.isEmpty())
                return fallback;
            bool valid{};
            const auto bytes = value.toULongLong(&valid);
            if (!valid || bytes == 0 || bytes > quint64{64} * 1024 * 1024 * 1024)
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
        std::erase_if(page.hyperlinks,
                      [&](const auto& link) { return link.first_cell >= page.cells.size(); });
        for (auto& link : page.hyperlinks)
            link.cell_count = static_cast<std::uint32_t>(
                std::min<std::size_t>(link.cell_count, page.cells.size() - link.first_cell));
        std::size_t codepoints{};
        for (const auto& cell : page.cells)
            codepoints =
                std::max(codepoints, static_cast<std::size_t>(cell.text_offset) + cell.text_length);
        page.graphemes.resize(codepoints);
        page.cursor = {};
        page.history = {rows, 0, rows, true};
        return page;
    }
    static TerminalSnapshot archive_window(TerminalSnapshot page, std::size_t first,
                                           std::size_t rows) {
        if (first >= page.size.rows)
            throw std::invalid_argument("Archive window starts outside the screen");
        rows = std::min(rows, static_cast<std::size_t>(page.size.rows) - first);
        const auto columns = static_cast<std::size_t>(page.size.columns);
        const std::size_t cell_first = first * columns;
        const std::size_t old_cells = page.cells.size();
        if (cell_first > old_cells)
            throw std::invalid_argument("Archive snapshot has too few cells");
        const std::size_t cell_last = std::min(old_cells, cell_first + rows * columns);

        std::vector<TerminalCell> cells(
            page.cells.begin() + static_cast<std::ptrdiff_t>(cell_first),
            page.cells.begin() + static_cast<std::ptrdiff_t>(cell_last));
        std::vector<TerminalHyperlink> links;
        links.reserve(page.hyperlinks.size());
        for (auto& link : page.hyperlinks) {
            const std::size_t link_first = link.first_cell;
            const std::size_t link_last =
                std::min(old_cells, link_first + static_cast<std::size_t>(link.cell_count));
            const std::size_t kept_first = std::max(link_first, cell_first);
            const std::size_t kept_last = std::min(link_last, cell_last);
            if (kept_first < kept_last)
                links.push_back({static_cast<std::uint32_t>(kept_first - cell_first),
                                 static_cast<std::uint32_t>(kept_last - kept_first),
                                 std::move(link.uri)});
        }

        const std::u32string old_graphemes = std::move(page.graphemes);
        std::u32string graphemes;
        graphemes.reserve(cells.size());
        for (auto& cell : cells) {
            const auto text =
                std::u32string_view(old_graphemes).substr(cell.text_offset, cell.text_length);
            cell.text_offset = static_cast<std::uint32_t>(graphemes.size());
            graphemes.append(text.begin(), text.end());
        }

        page.size.rows = static_cast<std::uint16_t>(rows);
        page.cells = std::move(cells);
        page.graphemes = std::move(graphemes);
        page.hyperlinks = std::move(links);
        page.cursor = {};
        page.history = {rows, 0, rows, true};
        return page;
    }
    bool archive_history(bool force) {
        // A width change's redraw is sorted out by finish_rewidth.
        if (rewidth_)
            return true;
        if (pending_rewidth_archive_)
            return try_rewidth_archive();
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
        const bool admission_deadline = output_admission_expired_;
        output_admission_expired_ = false;
        if (defer_output_for_admission(admission_deadline)) {
            processing_output_ = false;
            return;
        }
        output_admission_timer_.stop();
        try {
            if (harvest_offset_ && !archive_history(true)) {
                processing_output_ = false;
                return;
            }
            apply_pending_resize();
            const bool parser_waiting = feed_pending_output();
            flush_terminal_replies();
            schedule();
            resume_output(parser_waiting);
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
        }
        processing_output_ = false;
    }
    // Sustained pressure waits for its real admission boundary, but the
    // timer remains the bound if the producer quiets below it.
    bool defer_output_for_admission(bool admission_deadline) {
        if (!output_pressure_ || admission_deadline ||
            pending_output_.size() >= output_admission_limit)
            return false;
        if (!output_admission_timer_.isActive())
            output_admission_timer_.start(static_cast<int>(frame_ms));
        schedule_output_admission();
        return true;
    }
    // A pending resize waits until the rewidth settles before it is applied.
    void apply_pending_resize() {
        if (!pending_resize_ || rewidth_)
            return;
        const auto size = *pending_resize_;
        pending_resize_.reset();
        apply_resize(size);
    }
    // Bounds one parser feed so a large backlog cannot monopolize a frame.
    qsizetype output_chunk_size() const {
        return static_cast<qsizetype>(
            std::max(std::size_t{1},
                     std::min(std::size_t{256},
                              std::size_t{32768} / (std::size_t{4} * current_size_.columns))));
    }
    // Feeds bytes into the terminal parser and records the parse boundary.
    void feed_output_bytes(qsizetype size) {
        terminal_.feed(
            std::string_view(pending_output_.constData(), static_cast<std::size_t>(size)));
        pending_output_.remove(0, size);
        if (rewidth_)
            rewidth_->saw_output = true;
        mark_dirty();
        timing_.parse_end_ns = monotonic_ns();
    }
    // Feeds the bytes that precede an incomplete synchronized marker.
    // Returns false when the marker cannot be fed safely yet.
    bool feed_synchronized_holdback(qsizetype chunk) {
        const auto safe =
            std::min(chunk, pending_output_.size() - synchronized_output_holdback(pending_output_));
        if (safe <= 0)
            return false;
        feed_output_bytes(safe);
        return true;
    }
    // Feeds pending PTY output into the terminal parser under a bounded
    // budget. Returns true when the parser waits for the rest of a
    // synchronized marker.
    bool feed_pending_output() {
        int budget = 16;
        bool parser_waiting = false;
        while (!pending_output_.isEmpty() && budget-- > 0) {
            if (!archive_history(false)) {
                output_waiting_ = true;
                break;
            }
            const auto chunk = output_chunk_size();
            const auto sync_limit = synchronized_output_limit(pending_output_);
            if (sync_limit < 0) {
                if (feed_synchronized_holdback(chunk))
                    continue;
                parser_waiting = true;
                break;
            }
            // `sync_limit` ends at a complete DEC 2026 marker. Feeding only
            // `chunk` bytes could split that marker and lose both edges.
            const auto size = sync_limit < pending_output_.size()
                                  ? sync_limit
                                  : std::min(pending_output_.size(), chunk);
            feed_output_bytes(size);
            if (follow_synchronized_update())
                break; // a whole frame is on screen: show it before the next one starts
        }
        return parser_waiting;
    }
    // Writes the terminal's queued replies back to the PTY.
    void flush_terminal_replies() {
        const auto replies = terminal_.take_replies();
        if (replies.empty() || pty_.processId() == 0)
            return;
        if (!pty_.writeBytes(QByteArray(replies.data(), static_cast<qsizetype>(replies.size()))))
            throw std::runtime_error("PTY reply queue overflow");
    }
    void resume_output(bool parser_waiting) {
        if (parser_waiting) {
            // The remainder must arrive from the PTY; never spin on the same
            // partial marker, even when output pressure paused reads.
            if (output_pressure_) {
                output_pressure_ = false;
                output_retention_logged_ = false;
                output_pressure_bytes_ = 0;
                output_pressure_clock_.invalidate();
            }
            pty_.pauseOutput(false);
            return;
        }
        if (output_waiting_)
            return;
        if (!pending_output_.isEmpty()) {
            if (timer_.isActive())
                return; // a paced whole-frame publication owns the retained screen
            if (settle_timer_.isActive()) {
                QTimer::singleShot(0, this, [this] { process_output(); });
                return;
            }
            if (output_publication_blocked()) {
                schedule();
                return;
            }
            QTimer::singleShot(0, this, [this] { process_output(); });
            return;
        }
        if (output_pressure_) {
            output_pressure_ = false;
            output_retention_logged_ = false;
            output_pressure_bytes_ = 0;
            output_pressure_clock_.invalidate();
            qInfo().noquote() << "PTY output retention drained; output resumed";
        }
        pty_.pauseOutput(false);
    }
    // History replies go to whichever client or joined view asked.
    void send_history(const wire::HistoryReply& reply) {
        const bool to_client = client_ && ready_ && reply.attachment == attachment_;
        const auto* view = to_client ? nullptr : view_for(reply.attachment);
        QLocalSocket* const destination =
            to_client ? client_.data() : (view && view->ready ? view->socket.data() : nullptr);
        if (!destination)
            return;
        const auto view_id = view ? view->id : 0;
        try {
            auto bytes = wire::frame(wire::Kind::history_page,
                                     wire::encode_history_reply(
                                         reply, to_client ? client_hyperlinks_ : view->hyperlinks));
            if (destination->bytesToWrite() + bytes.size() > wire::max_frame_bytes)
                throw std::runtime_error("History response queue full");
            if (destination->write(bytes) != bytes.size())
                throw std::runtime_error("History response write failed");
        } catch (const std::exception& error) {
            if (!to_client) {
                drop_view(view_id, wire::StatusCode::overloaded, QString::fromUtf8(error.what()));
                return;
            }
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
    void request_history(const wire::Attachment& requester, const QByteArray& payload) {
        const auto request = wire::decode_history_request(payload);
        if (pending_history_) {
            send_history({requester,
                          request.request_id,
                          0,
                          QStringLiteral("History is busy; try again"),
                          {}});
            return;
        }
        // Browsing retries storage after a recoverable filesystem failure.
        history_failed_ = false;
        history_error_.clear();
        pending_history_ = std::pair{requester, request};
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
        output_admission_timer_.stop();
        attention_timer_.stop();
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
        for (const auto& view : views_) {
            if (view->socket) {
                send_status(view->socket, wire::StatusCode::ended, message);
                view->socket->flush();
            }
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
        posix::widen_socket_buffers(incoming->socketDescriptor());
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
            // A peer may finish authentication after stop() notified existing
            // views. Neither join nor takeover may enter a draining session.
            if (stopping_) {
                send_status(incoming, wire::StatusCode::ended, "Session is stopping");
                incoming->disconnectFromServer();
                return;
            }
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
            if (request.mode == wire::AttachMode::join)
                join(incoming, request.hyperlinks, request.paste_transactions);
            else
                activate(incoming, request.hyperlinks, request.attention_phase,
                         request.paste_transactions);
        } catch (const std::exception& error) {
            reject_attachment(incoming, QString::fromUtf8(error.what()));
        }
    }
    void activate(QLocalSocket* incoming, bool hyperlinks, bool attention_phase,
                  bool paste_transactions) {
        if (generation_ == std::numeric_limits<quint64>::max()) {
            send_status(incoming, wire::StatusCode::overloaded, "Attachment generation overflow");
            incoming->disconnectFromServer();
            return;
        }
        // Takeover retires the previous client's deferred history request while
        // its attachment is still known. Waiting for detach_client() is too
        // late: attachment_ has already advanced, so the old owner cannot be
        // matched and the next client sees a stale busy slot.
        if (pending_history_ && pending_history_->first == attachment_)
            pending_history_.reset();
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
        client_hyperlinks_ = hyperlinks;
        client_attention_phase_ = attention_phase;
        client_paste_transactions_ = paste_transactions;
        client_paste_id_ = 0;
        client_wanted_.reset();
        // A replacement stream is first-of-burst: do not let the previous
        // client's last publish defer this client's opening screen or
        // attention snapshot by up to a frame.
        snapshot_pace_.reset();
        attention_pace_.reset();
        attention_dirty_ = true;
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
        client_->write(
            wire::frame(wire::Kind::hello,
                        wire::encode_hello({.attachment = attachment_,
                                            .pid = quint64(pty_.processId()),
                                            .paste_transactions = client_paste_transactions_})));
        dirty_ = true;
        schedule();
    }
    // A change after a quiet frame goes out as soon as the event loop is free,
    // so a typed key echoes without waiting; changes within a frame of the
    // last screen share the next one.
    void schedule() {
        if (!process_started_ || stopping_ || timer_.isActive() || settle_timer_.isActive())
            return;
        if ((client_ && (!snapshot_in_flight_ || ready_) && dirty_) || views_due())
            timer_.start(static_cast<int>(snapshot_pace_.wait_ms(snapshot_clock_.elapsed())));
    }
    void publish_paced_output() {
        timer_.stop();
        publish();
        // This is the only safe point to let an already-queued synchronized
        // frame mutate the terminal retained by the publication that fired.
        if (!pending_output_.isEmpty() && !output_waiting_ && !processing_output_ &&
            !output_publication_blocked())
            QTimer::singleShot(0, this, [this] { process_output(); });
    }
    // Full-screen programs such as Claude Code bracket each repaint in a
    // synchronized update (DEC mode 2026). A screen published inside one is
    // half drawn: scrolling such a program showed its transcript being
    // painted in. Returns true when an update just ended, so the caller
    // publishes that whole frame before feeding more output.
    bool follow_synchronized_update() {
        const bool now = terminal_.synchronizing();
        if (now == in_sync_)
            return false;
        in_sync_ = now;
        if (now) {
            sync_since_.start();
            return false;
        }
        sync_since_.invalidate();
        // A program that marks its frames finished needs no startup settling.
        settle_ = Settle::settled;
        // Replace any hold timer. A completed frame is retained until the next
        // paced publication; a later update must not overwrite it first.
        timer_.stop();
        if (const qint64 wait = snapshot_pace_.wait_ms(snapshot_clock_.elapsed()); wait > 0) {
            timer_.start(static_cast<int>(wait));
            return true;
        }
        if (output_publication_blocked()) {
            schedule();
            return true;
        }
        publish();
        return true;
    }
    [[nodiscard]] bool output_publication_blocked() const {
        if (dirty_ && client_ && client_->bytesToWrite() != 0)
            return true;
        return std::any_of(views_.cbegin(), views_.cend(), [](const auto& view) {
            return view->socket && view->dirty && view->socket->bytesToWrite() != 0;
        });
    }
    // A program's first text settles before it is shown. Claude Code's classic
    // renderer prints a resumed conversation in one burst (200 KB in about
    // 0.4 s); shown frame by frame, the tab opened with the transcript
    // running down the screen instead of at its end. Blank screens before the
    // first text go out as usual; from the first text, the screen waits until
    // output has been quiet for settle_quiet_ms, at most settle_limit_ms.
    // Returns how long to keep waiting.
    qint64 settle_wait_ms() {
        if (settle_ == Settle::settled)
            return 0;
        if (settle_ == Settle::blank) {
            if (!shows_text(terminal_.snapshot()))
                return 0;
            settle_ = Settle::settling;
            settle_since_.start();
        }
        const qint64 quiet = last_output_.isValid() ? last_output_.elapsed() : settle_quiet_ms;
        const qint64 settling = settle_since_.elapsed();
        if (quiet >= settle_quiet_ms || settling >= settle_limit_ms) {
            settle_ = Settle::settled;
            return 0;
        }
        return std::min(settle_quiet_ms - quiet, settle_limit_ms - settling);
    }
    // Claude Code's classic renderer answers a width change by erasing the
    // screen and drawing the end of its conversation again at the new width,
    // more than a screen of it; it never reprints what had scrolled away.
    // Left alone, the terminal reflows the old screen into history first, so
    // the conversation showed twice, and lines narrowed earlier stayed broken
    // mid-word. Instead the old screen is set aside and blanked, the screen is
    // held until the redraw has been quiet for rewidth_quiet_ms (at most
    // rewidth_limit_ms), and finish_rewidth keeps each part once. Narrowing
    // archives the kept rows first, at the width they were drawn. Returns
    // false when the resize must wait for that archive.
    bool begin_rewidth(TerminalSize size) {
        if (!claude_ || rewidth_ || size.columns == current_size_.columns)
            return true;
        // A failed archive cannot preserve the old rows that narrowing would
        // set aside. Keep the terminal's reflow copy instead of optimizing the
        // width change until history recording has recovered.
        if (history_failed_)
            return true;
        auto old_screen = terminal_.snapshot();
        if (old_screen.alternate_screen || terminal_.synchronizing() || !shows_text(old_screen))
            return true;
        const bool widening = size.columns > current_size_.columns;
        if (!widening && !archive_history(true)) {
            pending_resize_ = size;
            return false;
        }
        // CAN ends any escape sequence the last output left open.
        terminal_.feed("\x18\x1b[H\x1b[J");
        rewidth_ = Rewidth{.old_screen = std::move(old_screen),
                           .widening = widening,
                           .history_rows = 0,
                           .since = {},
                           .saw_output = false};
        return true;
    }
    // How long the screen still waits for a width change's redraw.
    qint64 rewidth_wait_ms() {
        if (!rewidth_)
            return 0;
        const qint64 held = rewidth_->since.elapsed();
        if (!rewidth_->saw_output || terminal_.synchronizing()) {
            // The resize's local erase and mark_dirty are not redraw activity.
            // Wait for a PTY-fed byte (or a complete frame), while bounding a
            // renderer that never answers.
            if (held < rewidth_limit_ms)
                return std::min(qint64{50}, rewidth_limit_ms - held);
            finish_rewidth();
            return 0;
        }
        const qint64 quiet = last_output_.isValid() ? last_output_.elapsed() : rewidth_quiet_ms;
        if (quiet < rewidth_quiet_ms && held < rewidth_limit_ms)
            return std::min(rewidth_quiet_ms - quiet, rewidth_limit_ms - held);
        finish_rewidth();
        return 0;
    }
    // Keep terminal scrollback as the failure copy until every page from the
    // width change has been accepted by the bounded history queue. One page at
    // a time prevents a rejected vector append from destroying the only copies.
    bool try_rewidth_archive() {
        if (!pending_rewidth_archive_)
            return true;
        if (history_failed_) {
            pty_.pauseOutput(true);
            output_waiting_ = true;
            return false;
        }
        auto& pages = *pending_rewidth_archive_;
        while (!pages.empty()) {
            std::vector<TerminalSnapshot> batch;
            batch.reserve(1);
            batch.push_back(pages.front());
            if (!history_.append(std::move(batch))) {
                pty_.pauseOutput(true);
                output_waiting_ = true;
                return false;
            }
            pages.erase(pages.begin());
        }
        if (!history_.idle()) {
            pty_.pauseOutput(true);
            output_waiting_ = true;
            return false;
        }
        terminal_.clear_history();
        pending_rewidth_archive_.reset();
        return true;
    }
    void delete_repeated_rows(const TerminalSnapshot& screen, std::size_t scrolled,
                              const std::optional<rewidth::Plan>& plan,
                              std::size_t drop_screen_rows) {
        if (!plan || drop_screen_rows == 0 || screen.alternate_screen || terminal_.synchronizing())
            return;
        const std::size_t screen_drop_at = plan->drop_at > scrolled ? plan->drop_at - scrolled : 0;
        const std::size_t max_screen_drop =
            static_cast<std::size_t>(screen.size.rows) - screen_drop_at;
        drop_screen_rows = std::min(drop_screen_rows, max_screen_drop);
        if (drop_screen_rows == 0)
            return;
        // Delete this repeated redraw interval from the screen; the cursor
        // moves with the rows below it, so relative moves land.
        const auto cursor = screen.cursor.row;
        const auto row = cursor >= screen_drop_at + drop_screen_rows
                             ? cursor - drop_screen_rows
                             : std::min(cursor, static_cast<std::uint16_t>(screen_drop_at));
        terminal_.feed("\x18\x1b[" + std::to_string(screen_drop_at + 1) + ";1H\x1b[" +
                       std::to_string(drop_screen_rows) + "M\x1b[" + std::to_string(row + 1) + ";" +
                       std::to_string(screen.cursor.column + 1) + "H");
    }
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void finish_rewidth() {
        namespace rw = rewidth;
        if (!rewidth_)
            return;
        const auto state = std::move(*rewidth_);
        rewidth_.reset();
        try {
            const auto screen = terminal_.snapshot();
            const auto metadata = terminal_.history_metadata();
            const std::size_t rows = metadata.viewport_rows;
            const std::size_t kept = metadata.total_rows > rows ? metadata.total_rows - rows : 0;
            const std::size_t before = std::min(state.history_rows, kept);
            const std::size_t scrolled = kept - before;
            rw::Text history, redraw;
            for (std::size_t first = 0; first < kept; first += rows) {
                const auto page = terminal_.history_snapshot(first);
                for (std::size_t row = 0; row < rows && first + row < kept; ++row)
                    rw::append_rows(first + row < before ? history : redraw, page, row, 1);
            }
            rw::append_rows(redraw, screen, 0, screen.size.rows);
            std::vector<TerminalSnapshot> pages;
            const auto keep = [&](std::size_t first, std::size_t last) {
                for (std::size_t offset = first; offset < last; offset += rows)
                    pages.push_back(archive_page(terminal_.history_snapshot(offset),
                                                 std::min(rows, last - offset)));
            };
            std::size_t drop_screen_rows = 0;
            const auto kept_history =
                state.widening && redraw.first_words && !screen.alternate_screen
                    ? rw::history_kept(history, redraw)
                    : std::nullopt;
            const auto plan = redraw.first_words && !screen.alternate_screen
                                  ? rw::plan(rw::screen_text(state.old_screen), redraw)
                                  : std::nullopt;
            if (kept_history) {
                // Wider, with the older copy still in reach: keep the wider redraw.
                keep(0, std::min(*kept_history, before));
                keep(before, kept);
            } else if (plan) {
                keep(0, before);
                if (plan->keep_old_rows)
                    pages.push_back(
                        archive_window(state.old_screen, plan->keep_at, plan->keep_old_rows));
                const std::size_t dropped_from_history = std::min(
                    plan->drop_new_rows, scrolled > plan->drop_at ? scrolled - plan->drop_at : 0);
                keep(before, before + std::min(plan->drop_at, scrolled));
                keep(before + plan->drop_at + dropped_from_history, kept);
                drop_screen_rows = plan->drop_new_rows - dropped_from_history;
            } else {
                // No redraw to line up with: keep the old conversation, not lose it.
                keep(0, before);
                if (const auto old_rows = rw::conversation_rows(state.old_screen))
                    pages.push_back(archive_page(state.old_screen, old_rows));
                keep(before, kept);
            }
            pending_rewidth_archive_ = std::move(pages);
            if (!try_rewidth_archive())
                qWarning().noquote()
                    << "History after a width change is waiting for archive capacity";
            delete_repeated_rows(screen, scrolled, plan, drop_screen_rows);
        } catch (const std::exception& error) {
            qWarning().noquote() << "Width change history:" << error.what();
        } catch (...) {
            qWarning().noquote() << "Width change history: unknown exception";
        }
        mark_dirty();
        // A resize that arrived meanwhile.
        if (pending_resize_)
            QTimer::singleShot(0, this, [this] { process_output(); });
    }
    static bool shows_text(const lapis::session::TerminalSnapshot& screen) {
        for (std::size_t index = 0; index < screen.cells.size(); ++index)
            for (const char32_t c : screen.text(index))
                if (c != U' ' && c != U' ')
                    return true;
        return false;
    }
    void publish() {
        qint64 wait = 0;
        try {
            wait = std::max(rewidth_wait_ms(), settle_wait_ms());
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
            return;
        }
        if (wait > 0) {
            // Not timer_: that timer means a frame is ready to show, and
            // resume_output() would stop feeding the startup burst (or a width
            // change's redraw) until it fired.
            settle_timer_.start(static_cast<int>(wait));
            return;
        }
        // Mid-update: hold the screen until the update ends (which publishes
        // at once) or until sync_hold_ms, so a program that never ends one
        // cannot freeze its views.
        if (in_sync_ && sync_since_.isValid() && sync_since_.elapsed() < sync_hold_ms) {
            timer_.start(static_cast<int>(sync_hold_ms - sync_since_.elapsed()));
            return;
        }
        if (output_publication_blocked())
            return;
        snapshot_pace_.published(snapshot_clock_.elapsed());
        publish_views();
        publish_client();
    }
    void publish_client() {
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
                                                           .timing = timing_},
                                                          client_hyperlinks_));
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
        const auto* state = attention_state();
        if (!state || !state->ready() || state->epoch() != decision.source_epoch)
            return;
        const auto found = state->pending().find(decision.request_id);
        if (found == state->pending().end())
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
    void decide_attention(const QByteArray& payload) {
        if (!attention_state())
            throw std::runtime_error("Attention decisions are unsupported for terminal sessions");
        // Every observer lapis runs is observation only: a decision from an
        // older desktop is refused before it reaches the agent, which takes
        // the answer in its own terminal.
        const auto decision = wire::decode_attention_decision(payload);
        allow_decision_retry(decision);
        decision_error_ = claude_observer_
                              ? QStringLiteral("Answer Claude requests in the terminal")
                              : QStringLiteral("Answer Codex requests in the terminal");
        attention_dirty_ = true;
        schedule_attention();
    }
    void handle(const wire::Frame& frame) {
        if (!process_started_ || stopping_)
            throw std::runtime_error("Session is not ready for input");
        if (frame.kind == wire::Kind::ready) {
            acknowledge(frame.payload);
            return;
        }
        if (frame.kind == wire::Kind::paste_request) {
            if (!ready_ || !client_paste_transactions_)
                throw std::runtime_error("Paste transaction was not negotiated or synchronized");
            const auto request = wire::decode_paste_request(frame.payload);
            if (request.attachment != attachment_)
                throw std::runtime_error("Stale paste attachment");
            claim_size(client_wanted_);
            paste_input(client_, request, client_paste_id_);
            return;
        }
        const auto control = wire::decode_control(frame.payload);
        if (control.attachment != attachment_ || !ready_)
            throw std::runtime_error("Stale attachment or session is not ready for input");
        if (control.payload.size() > wire::max_input_bytes)
            throw std::runtime_error("Input message too large");
        QByteArray bytes;
        switch (frame.kind) {
        case wire::Kind::attention_decision:
            decide_attention(control.payload);
            return;
        case wire::Kind::history_request:
            request_history(attachment_, control.payload);
            return;
        case wire::Kind::terminate:
            end_agent(control.payload);
            return;
        case wire::Kind::wheel:
            // Scrolling reads; it does not take the size as typing does.
            write_input(client_.data(), frame.kind, control.payload);
            return;
        case wire::Kind::text:
        case wire::Kind::paste:
        case wire::Kind::key:
            claim_size(client_wanted_);
            write_input(client_.data(), frame.kind, control.payload);
            return;
        case wire::Kind::resize:
            client_wanted_ = decode_size(control.payload);
            claim_size(client_wanted_);
            return;
        default:
            throw std::runtime_error("Unexpected client message");
        }
    }
    QByteArray input_bytes(wire::Kind kind, const QByteArray& payload) {
        if (kind == wire::Kind::text)
            return payload;
        if (kind == wire::Kind::paste) {
            const auto encoded = terminal_.encode_paste(
                std::string_view(payload.constData(), static_cast<std::size_t>(payload.size())));
            return {encoded.data(), static_cast<qsizetype>(encoded.size())};
        }
        if (kind == wire::Kind::wheel) {
            const auto wheel = wire::decode_wheel(payload);
            const auto encoded = terminal_.encode_wheel({wheel.steps, wheel.column, wheel.row});
            return {encoded.data(), static_cast<qsizetype>(encoded.size())};
        }
        if (payload.size() != 2)
            throw std::runtime_error("Invalid key message");
        const auto key_value = static_cast<unsigned char>(payload[0]);
        if (key_value > static_cast<unsigned char>(TerminalKey::escape))
            throw std::runtime_error("Unknown key code");
        const auto mods = static_cast<unsigned char>(payload[1]);
        const auto encoded = terminal_.encode_key(
            static_cast<TerminalKey>(key_value),
            {(mods & 1U) != 0, (mods & 2U) != 0, (mods & 4U) != 0, (mods & 8U) != 0});
        return {encoded.data(), static_cast<qsizetype>(encoded.size())};
    }
    void write_input(QLocalSocket* source, wire::Kind kind, const QByteArray& payload) {
        // A wheel over the primary screen sends the program nothing.
        const auto bytes = input_bytes(kind, payload);
        if (!bytes.isEmpty() && !pty_.writeBytes(bytes)) {
            if (pty_.terminalClosed())
                return;
            if (source)
                send_status(source, wire::StatusCode::overloaded,
                            QStringLiteral("PTY input queue full; input was not queued"));
            return;
        }
        if ((kind == wire::Kind::key || kind == wire::Kind::text) && bytes.contains('\r'))
            note_submitted();
    }
    void paste_input(QLocalSocket* destination, const wire::PasteRequest& request,
                     quint64& last_id) {
        if (request.request_id <= last_id)
            throw std::runtime_error("Paste request was already submitted; it was not repeated");
        last_id = request.request_id;
        wire::PasteResult result{request.attachment, request.request_id, false, {}};
        try {
            if (request.submit && attention_state() && attention_state()->blocking())
                throw std::runtime_error(
                    "Agent has a pending request; automatic paste-plus-Return was refused");
            auto bytes = input_bytes(wire::Kind::paste, request.text);
            if (request.submit) {
                const auto enter = terminal_.encode_key(TerminalKey::enter, {});
                bytes.append(enter.data(), static_cast<qsizetype>(enter.size()));
            }
            // One queue admission includes both bracket markers and optional
            // Return. Existing pending input competes for this same budget.
            result.queued = pty_.writeBytes(bytes);
            if (result.queued && request.submit)
                note_submitted();
            if (!result.queued)
                result.message = QStringLiteral(
                    "PTY input queue full or child unavailable; paste was not queued");
        } catch (const std::exception& error) {
            result.message = QString::fromUtf8(error.what()).left(1000);
        }
        const auto frame = wire::frame(wire::Kind::paste_result, wire::encode_paste_result(result));
        if (!destination || destination->bytesToWrite() + frame.size() > wire::max_frame_bytes ||
            destination->write(frame) != frame.size())
            throw std::runtime_error(
                "Paste result could not be delivered; do not retry automatically");
    }
    static TerminalSize decode_size(const QByteArray& payload) {
        if (payload.size() != 4)
            throw std::runtime_error("Invalid resize message");
        QDataStream in(payload);
        quint16 columns{}, rows{};
        in >> columns >> rows;
        if (columns == 0 || rows == 0 || quint32(columns) * rows > wire::max_cells)
            throw std::runtime_error("Invalid terminal geometry");
        return {columns, rows};
    }
    void request_resize(TerminalSize size) {
        if (harvest_offset_ || rewidth_)
            pending_resize_ = size;
        else
            apply_resize(size);
    }
    // Several devices can show one agent; the one that last resized or typed
    // sets the size, as tmux does with window-size latest. `view` is 0 for
    // the attached client.
    void claim_size(const std::optional<TerminalSize>& wanted, quint64 view = 0) {
        if (!wanted)
            return;
        size_view_ = view;
        if (*wanted != pending_resize_.value_or(current_size_))
            request_resize(*wanted);
    }
    // Only an explicit close from an attached client ends the agent; detaching
    // or GUI exit never does. The ordinary exit path then reports `ended`.
    void end_agent(const QByteArray& payload) {
        if (!payload.isEmpty())
            throw std::runtime_error("Terminate message must be empty");
        // Reaped children may still be draining output/history before `ended`.
        if (pty_.processId() != 0 && !pty_.hangup())
            throw std::runtime_error("Agent process could not be ended");
    }
    void apply_resize(TerminalSize size) {
        if (pty_.terminalClosed())
            return;
        if (!begin_rewidth(size))
            return; // after the kept rows are archived
        if (!pty_.resize(size))
            throw std::runtime_error("PTY resize failed");
        try {
            terminal_.resize(size);
            current_size_ = size;
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
            return; // A failed engine resize cannot be presented as synchronized.
        }
        if (rewidth_) {
            const auto metadata = terminal_.history_metadata();
            rewidth_->history_rows = metadata.total_rows > metadata.viewport_rows
                                         ? metadata.total_rows - metadata.viewport_rows
                                         : 0;
            rewidth_->since.start();
        }
        const auto replies = terminal_.take_replies();
        if (!replies.empty() &&
            !pty_.writeBytes(QByteArray(replies.data(), static_cast<qsizetype>(replies.size()))))
            throw std::runtime_error("PTY reply queue overflow");
        mark_dirty();
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
            auto* oldest = retired_.front();
            retired_.pop_front();
            oldest->abort();
            oldest->deleteLater();
        }
        retired_.push_back(socket);
        connect(socket, &QObject::destroyed, this,
                [this, socket] { std::erase(retired_, socket); });
        socket->disconnectFromServer();
        if (socket->state() == QLocalSocket::UnconnectedState)
            socket->deleteLater();
        else
            QTimer::singleShot(1000, socket, &QObject::deleteLater);
    }
    // A view joined beside the client (AttachMode::join), such as the phone
    // gateway. It receives screens and may type and resize, but never retires
    // the client, and the client's reattachment never retires it.
    struct View {
        quint64 id{};
        QPointer<QLocalSocket> socket;
        wire::Attachment attachment;
        QByteArray buffer;
        quint64 ready_sequence{};
        bool ready{};
        bool hyperlinks{};
        bool paste_transactions{};
        quint64 paste_id{};
        bool in_flight{};
        bool dirty{true};
        std::optional<TerminalSize> wanted;
    };
    static constexpr std::size_t max_views = 4;
    View* find_view(quint64 id) {
        const auto found = std::find_if(views_.begin(), views_.end(),
                                        [id](const auto& view) { return view->id == id; });
        return found == views_.end() ? nullptr : found->get();
    }
    const View* view_for(const wire::Attachment& attachment) const {
        const auto found = std::find_if(views_.begin(), views_.end(), [&](const auto& view) {
            return view->attachment == attachment;
        });
        return found == views_.end() ? nullptr : found->get();
    }
    void mark_dirty() {
        last_output_.start();
        dirty_ = true;
        for (const auto& view : views_)
            view->dirty = true;
    }

    // One DEC 2026 control sequence, possibly split by PTY reads. The
    // service observes the edge only when the whole sequence is fed, so the
    // parser carries a partial prefix instead of accidentally crossing a
    // frame end. A carry is the number of buffered bytes to hold back.
    static qsizetype synchronized_output_limit(const QByteArray& pending) {
        static constexpr std::array<std::string_view, 2> markers{{"\x1b[?2026h", "\x1b[?2026l"}};

        for (qsizetype offset = 0; offset < pending.size(); ++offset) {
            for (const auto& marker : markers) {
                const auto available =
                    std::min(static_cast<qsizetype>(marker.size()), pending.size() - offset);
                if (!std::equal(pending.cbegin() + offset, pending.cbegin() + offset + available,
                                marker.cbegin()))
                    continue;
                return available == static_cast<qsizetype>(marker.size())
                           ? offset + static_cast<qsizetype>(marker.size())
                           : -static_cast<qsizetype>(marker.size());
            }
        }
        return pending.size();
    }

    static qsizetype synchronized_output_holdback(const QByteArray& pending) {
        static constexpr std::array<std::string_view, 2> markers{{"\x1b[?2026h", "\x1b[?2026l"}};
        for (const auto& marker : markers) {
            const auto marker_size = static_cast<qsizetype>(marker.size());
            for (auto length = std::min(marker_size - 1, pending.size()); length > 0; --length)
                if (std::equal(pending.cend() - length, pending.cend(), marker.cbegin()))
                    return length;
        }
        return 0;
    }

    [[nodiscard]] bool views_due() const {
        return std::any_of(views_.begin(), views_.end(), [](const auto& view) {
            return view->socket && view->dirty && (view->ready || !view->in_flight);
        });
    }
    void join(QLocalSocket* incoming, bool hyperlinks, bool paste_transactions) {
        if (!process_started_ || views_.size() >= max_views ||
            generation_ == std::numeric_limits<quint64>::max()) {
            send_status(incoming, wire::StatusCode::overloaded,
                        process_started_ ? QStringLiteral("Too many joined views")
                                         : QStringLiteral("Session is starting; try again"));
            incoming->disconnectFromServer();
            return;
        }
        ++generation_;
        auto view = std::make_unique<View>();
        view->id = generation_;
        view->socket = incoming;
        view->hyperlinks = hyperlinks;
        view->paste_transactions = paste_transactions;
        view->attachment = {.identity = identity_, .generation = generation_};
        const auto id = view->id;
        views_.push_back(std::move(view));
        incoming->setReadBufferSize(wire::max_frame_bytes + 4);
        connect(incoming, &QLocalSocket::readyRead, this, [this, id] { receive_view(id); });
        connect(incoming, &QLocalSocket::bytesWritten, this, [this] { schedule(); });
        connect(incoming, &QLocalSocket::disconnected, this, [this, id] { drop_view(id); });
        incoming->write(wire::frame(
            wire::Kind::hello, wire::encode_hello({.attachment = views_.back()->attachment,
                                                   .pid = quint64(pty_.processId()),
                                                   .paste_transactions = paste_transactions})));
        QTimer::singleShot(ack_timer_.interval(), this, [this, id] {
            if (const auto* view = find_view(id); view && !view->ready)
                drop_view(id, wire::StatusCode::rejected,
                          QStringLiteral("Attachment ready acknowledgement timed out"));
        });
        schedule();
    }
    void drop_view(quint64 id, std::optional<wire::StatusCode> code = std::nullopt,
                   const QString& message = {}) {
        const auto found = std::find_if(views_.begin(), views_.end(),
                                        [id](const auto& view) { return view->id == id; });
        if (found == views_.end())
            return;
        if (pending_history_ && (*found)->attachment == pending_history_->first)
            pending_history_.reset();
        const QPointer<QLocalSocket> socket = (*found)->socket;
        views_.erase(found);
        // A phone that leaves hands the size back to the desktop.
        if (size_view_ == id) {
            size_view_ = 0;
            if (client_ && ready_ && process_started_ && !stopping_) {
                try {
                    claim_size(client_wanted_);
                } catch (const std::exception& error) {
                    qWarning().noquote() << "Size not restored:" << error.what();
                }
            }
        }
        if (!socket)
            return;
        disconnect(socket, nullptr, this, nullptr);
        if (code)
            send_status(socket, *code, message);
        retire(socket);
    }
    void publish_views() {
        std::optional<TerminalSnapshot> snapshot;
        std::vector<quint64> failed;
        for (const auto& view : views_) {
            if (!view->socket || view->socket->state() != QLocalSocket::ConnectedState ||
                !view->dirty || (!view->ready && view->in_flight) ||
                view->socket->bytesToWrite() != 0)
                continue;
            try {
                if (!snapshot)
                    snapshot = terminal_.snapshot();
                if (snapshot_sequence_ == std::numeric_limits<quint64>::max())
                    throw std::overflow_error("Snapshot sequence overflow");
                const quint64 sequence = ++snapshot_sequence_;
                timing_.publish_ns = monotonic_ns();
                const auto bytes =
                    wire::frame(wire::Kind::snapshot,
                                wire::encode_snapshot_message({.attachment = view->attachment,
                                                               .sequence = sequence,
                                                               .snapshot = *snapshot,
                                                               .timing = timing_},
                                                              view->hyperlinks));
                if (view->socket->write(bytes) < 0)
                    throw std::runtime_error("View socket write failed");
                view->dirty = false;
                if (!view->ready) {
                    view->ready_sequence = sequence;
                    view->in_flight = true;
                }
            } catch (const std::exception& error) {
                qWarning().noquote()
                    << "Snapshot unavailable for view" << view->id << ":" << error.what();
                failed.push_back(view->id);
            }
        }
        for (const auto id : failed)
            drop_view(id, wire::StatusCode::overloaded,
                      QStringLiteral("Snapshot unavailable for this view"));
    }
    void receive_view(quint64 id) {
        auto* view = find_view(id);
        if (!view || !view->socket || stopping_)
            return;
        try {
            view->buffer += view->socket->read(wire::max_frame_bytes + 4 - view->buffer.size());
            wire::Frame frame;
            qsizetype consumed{};
            for (int processed = 0; processed < 64; ++processed) {
                if (!wire::take_frame(view->buffer, consumed, frame)) {
                    if (consumed != 0)
                        view->buffer.remove(0, consumed);
                    return;
                }
                handle_view(*view, frame);
                view = find_view(id);
                if (!view || stopping_)
                    return;
            }
            if (view->buffer.size() > wire::max_frame_bytes + 4)
                throw std::runtime_error("Session input overflow");
            if (consumed != 0)
                view->buffer.remove(0, consumed);
            QTimer::singleShot(0, this, [this, id] { receive_view(id); });
        } catch (const std::exception& error) {
            if (!stopping_)
                drop_view(id, wire::StatusCode::rejected, QString::fromUtf8(error.what()));
        }
    }
    void handle_view(View& view, const wire::Frame& frame) {
        if (!process_started_ || stopping_)
            throw std::runtime_error("Session is not ready for input");
        if (frame.kind == wire::Kind::ready) {
            const auto ready = wire::decode_ready(frame.payload);
            if (ready.attachment != view.attachment || ready.sequence != view.ready_sequence ||
                view.ready || !view.in_flight)
                throw std::runtime_error("Stale or mismatched ready acknowledgement");
            view.ready = true;
            view.in_flight = false;
            schedule();
            return;
        }
        if (frame.kind == wire::Kind::paste_request) {
            if (!view.ready || !view.paste_transactions)
                throw std::runtime_error("Paste transaction was not negotiated or synchronized");
            const auto request = wire::decode_paste_request(frame.payload);
            if (request.attachment != view.attachment)
                throw std::runtime_error("Stale paste attachment");
            claim_size(view.wanted, view.id);
            paste_input(view.socket, request, view.paste_id);
            return;
        }
        const auto control = wire::decode_control(frame.payload);
        if (control.attachment != view.attachment || !view.ready)
            throw std::runtime_error("Stale attachment or view is not ready for input");
        if (control.payload.size() > wire::max_input_bytes)
            throw std::runtime_error("Input message too large");
        if (frame.kind == wire::Kind::resize) {
            view.wanted = decode_size(control.payload);
            claim_size(view.wanted, view.id);
            return;
        }
        if (frame.kind == wire::Kind::history_request) {
            request_history(view.attachment, control.payload);
            return;
        }
        if (frame.kind == wire::Kind::wheel) {
            write_input(view.socket.data(), frame.kind, control.payload);
            return;
        }
        if (frame.kind != wire::Kind::text && frame.kind != wire::Kind::paste &&
            frame.kind != wire::Kind::key)
            throw std::runtime_error(
                "A joined view may only type, scroll, resize and page history");
        claim_size(view.wanted, view.id);
        write_input(view.socket.data(), frame.kind, control.payload);
    }
    void detach_client() {
        if (!client_)
            return;
        disconnect(client_, nullptr, this, nullptr);
        retire(client_);
        client_ = nullptr;
        if (pending_history_ && pending_history_->first == attachment_)
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
    std::optional<TerminalSize> client_wanted_;
    quint64 size_view_{}; // the joined view whose size applies; 0 for the client
    std::vector<std::unique_ptr<View>> views_;
    QSet<QLocalSocket*> pending_;
    std::deque<QLocalSocket*> retired_;
    QByteArray fingerprint_;
    wire::SessionIdentity identity_;
    wire::Attachment attachment_;
    QByteArray buffer_;
    QTimer timer_;
    QTimer settle_timer_;
    QTimer output_admission_timer_;
    static constexpr qint64 frame_ms = 16;
    QElapsedTimer attention_clock_;
    wire::UpdatePace attention_pace_{frame_ms};
    QElapsedTimer snapshot_clock_;
    wire::UpdatePace snapshot_pace_{frame_ms};
    QElapsedTimer sync_since_; // when the current synchronized update began
    bool in_sync_{};
    static constexpr qint64 sync_hold_ms = 250;
    enum class Settle : std::uint8_t { blank, settling, settled };
    Settle settle_{Settle::blank};
    QElapsedTimer settle_since_; // when the program first showed text
    QElapsedTimer last_output_;  // when the screen last changed
    static constexpr qint64 settle_quiet_ms = 150;
    static constexpr qint64 settle_limit_ms = 2000;
    QTimer ack_timer_;
    quint64 generation_{};
    quint64 snapshot_sequence_{};
    quint64 ready_sequence_{};
    bool dirty_{true};
    bool ready_{};
    bool client_hyperlinks_{};
    bool client_attention_phase_{};
    bool client_paste_transactions_{};
    quint64 client_paste_id_{};
    bool snapshot_in_flight_{};
    bool process_started_{};
    bool stopping_{};
    HistoryWorker history_;
    QByteArray pending_output_;
    QString history_error_;
    bool processing_output_{};
    bool output_admission_scheduled_{};
    bool output_processing_scheduled_{};
    bool output_admission_expired_{};
    bool output_waiting_{};
    bool output_pressure_{};
    bool output_retention_logged_{};
    qsizetype output_pressure_bytes_{};
    QElapsedTimer output_pressure_clock_;
    bool history_failed_{};
    std::optional<std::pair<wire::Attachment, wire::HistoryRequest>> pending_history_;
    std::optional<std::size_t> harvest_offset_;
    std::optional<TerminalSize> pending_resize_;
    TerminalSize current_size_;
    // Claude Code's classic renderer redraws the end of its conversation on a
    // width change; see begin_rewidth.
    bool claude_{};
    struct Rewidth {
        TerminalSnapshot old_screen;
        bool widening{};
        std::size_t history_rows{}; // kept rows from before the resize
        QElapsedTimer since;
        bool saw_output{}; // only bytes fed from the PTY, not the local erase
    };
    std::optional<Rewidth> rewidth_;
    std::optional<std::vector<TerminalSnapshot>> pending_rewidth_archive_;
    static constexpr qint64 rewidth_quiet_ms = 120;
    static constexpr qint64 rewidth_limit_ms = 1000;
    wire::SnapshotTiming timing_;
    std::unique_ptr<attention::State> claude_state_;
    std::unique_ptr<lapis::claude::Observer> claude_observer_;
    std::optional<TerminalHookChannel> terminal_hooks_;
    std::unique_ptr<attention::State> notify_state_;
    std::unique_ptr<NotifyTurns> notify_turns_;
    QString resume_endpoint_;
    CheckpointScanner checkpoint_scanner_;
    QString checkpoint_agent_;
    bool omp_extension_ = false;
    ResumeRecord resume_;
    // A local Codex agent whose notify program this service set.
    bool codex_notify_{};
    QSet<QString> subagent_threads_;
    QTimer attention_timer_;
    bool attention_dirty_{};
    std::unique_ptr<AttentionJournal> attention_journal_;
    JournaledAsks journaled_;
    bool journal_append_failed_{};
    bool attention_journal_failed_{};
    QString decision_error_;
};

// A desktop opened from inside another agent's terminal passes that agent's
// per-session markers down to this service. Agents lapis launches are
// top-level sessions of every harness: with Claude Code's markers a new agent
// reports to the parent's messaging socket and does not save its transcript,
// and other harnesses similarly detect a parent agent, sandbox or tool bridge.
// Only markers a harness sets for its own children are removed; user settings
// such as CLAUDE_CODE_EFFORT_LEVEL or GROK_DEFAULT_MODEL stay intact.
[[maybe_unused]] void clear_parent_session_markers() {
    for (const char* name :
         {// Claude Code, and the cross-harness marker it sets.
          "CLAUDECODE", "CLAUDE_CODE_CHILD_SESSION", "CLAUDE_CODE_SESSION_ID",
          "CLAUDE_CODE_SESSION_ATTENDED", "CLAUDE_CODE_ENTRYPOINT", "CLAUDE_CODE_EXECPATH",
          "CLAUDE_CODE_MESSAGING_SOCKET", "CLAUDE_CODE_MESSAGING_TOKEN", "CLAUDE_PID",
          "CLAUDE_EFFORT", "AI_AGENT",
          // Codex sandboxed commands.
          "CODEX_SANDBOX", "CODEX_SANDBOX_NETWORK_DISABLED",
          // Grok, OpenCode and OMP child sessions and tool bridges.
          "GROK_AGENT", "GROK_SESSION_ID", "OPENCODE", "OPENCODE_PID", "PI_SESSION_FILE",
          "PI_TOOL_BRIDGE_SESSION", "PI_TOOL_BRIDGE_URL", "PI_TOOL_BRIDGE_TOKEN"})
        qunsetenv(name);
}
} // namespace
// The terminal the CLI starts in, COLUMNSxROWS, as the view that will show
// it: no resize just after its first frame.
TerminalSize parse_size(const QString& text) {
    const auto parts = text.split(QLatin1Char('x'));
    bool columns_ok = false;
    bool rows_ok = false;
    const auto columns = parts.value(0).toUShort(&columns_ok);
    const auto rows = parts.value(1).toUShort(&rows_ok);
    if (parts.size() != 2 || !columns_ok || !rows_ok)
        throw std::invalid_argument("Expected --size COLUMNSxROWS");
    return {columns, rows};
}

// The options before SOCKET; `socket` is left at the first other argument.
struct ServiceOptions {
    QByteArray session_id;
    QByteArray session_epoch;
    AgentMode agent{AgentMode::terminal};
    std::optional<TerminalSize> size;
    qsizetype socket{1};
};
// Argument parsing follows the service's documented option grammar; the
// dedicated complexity phase owns any split.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ServiceOptions parse_options(const QStringList& arguments) {
    ServiceOptions options;
    const auto value = [&arguments, &options](const char* usage) {
        if (options.socket + 1 >= arguments.size())
            throw std::invalid_argument(usage);
        return arguments.at(++options.socket);
    };
    for (; options.socket < arguments.size(); ++options.socket) {
        const auto& option = arguments.at(options.socket);
        if (option == QStringLiteral("--session-id")) {
            if (!options.session_id.isEmpty())
                throw std::invalid_argument("Expected one --session-id HEX32");
            options.session_id = parse_session_id(value("Expected one --session-id HEX32"));
        } else if (option == QStringLiteral("--session-epoch")) {
            if (!options.session_epoch.isEmpty())
                throw std::invalid_argument("Expected one --session-epoch HEX32");
            const QString epoch_value = value("Expected one --session-epoch HEX32");
            try {
                options.session_epoch = parse_session_id(epoch_value);
            } catch (const std::invalid_argument&) {
                throw std::invalid_argument(
                    "Session epoch must be exactly 32 hexadecimal characters and nonzero");
            }
        } else if (option == QStringLiteral("--codex") || option == QStringLiteral("--claude")) {
            if (options.agent != AgentMode::terminal)
                throw std::invalid_argument("Expected one agent mode");
            options.agent =
                option == QStringLiteral("--codex") ? AgentMode::codex : AgentMode::claude;
        } else if (option == QStringLiteral("--size")) {
            if (options.size)
                throw std::invalid_argument("Expected one --size COLUMNSxROWS");
            options.size = parse_size(value("Expected one --size COLUMNSxROWS"));
        } else {
            break;
        }
    }
    return options;
}

#ifndef LAPIS_SESSION_SERVICE_TEST
int main(int argc, char** argv) {
    // Before any agent inherits the environment.
    clear_parent_session_markers();
    QStringList arguments;
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));
    int application_argc = 1;
    QCoreApplication app(application_argc, argv);
    if (arguments.value(1) == QStringLiteral("--claude-hook")) {
        // A service started before relay contracts passes no fourth value.
        if (arguments.size() != 4 && arguments.size() != 5)
            return 0;
        return lapis::claude::run_hook_relay(arguments.at(2), arguments.at(3), arguments.value(4));
    }
    try {
        auto options = parse_options(arguments);
        const auto socket_index = options.socket;
        if (arguments.size() - socket_index < 3)
            throw std::invalid_argument(
                "Usage: lapis_session_service [--session-id HEX32] [--codex | --claude] "
                "[--session-epoch HEX32] "
                "[--size COLUMNSxROWS] SOCKET DIRECTORY PROGRAM [ARG ...]");
        if (options.session_id.isEmpty())
            options.session_id = wire::new_id();
        const auto program_index = socket_index + 2;
        const auto launch = validate_launch({.program = arguments.at(program_index),
                                             .arguments = arguments.mid(program_index + 1),
                                             .directory = arguments.at(socket_index + 1),
                                             .size = options.size.value_or(TerminalSize{100, 30}),
                                             .agent = options.agent});
        SessionService service(posix::prepare_endpoint(arguments.at(socket_index)),
                               options.session_id, launch, options.session_epoch);
        return app.exec();
    } catch (const std::exception& error) {
        qCritical().noquote() << error.what();
        return 1;
    }
}
#endif
