#include "agent_checkpoint.hpp"
#include "attention_audit.hpp"
#include "attention_journal.hpp"
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
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace lapis::session {
class AttentionServiceTestAccess;
}

namespace {
using namespace lapis::session;
constexpr int codex_sync_timeout_ms = 15000;
constexpr int terminal_sync_timeout_ms = 3000;
constexpr int backend_probe_interval_ms = 25;
constexpr int backend_probe_attempts = 400;
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
// Codex 0.156 listens through a symlink it removes on a clean exit. A power
// loss leaves the link behind (pointing into a /tmp a reboot clears); a dead
// link at this service's own path is removed.
void remove_dead_codex_link(const QString& path) {
    if (!QFileInfo(path).isSymLink())
        return;
    QLocalSocket existing;
    existing.connectToServer(path);
    if (existing.waitForConnected(100))
        throw std::runtime_error("Codex endpoint already in use");
    if (!QFile::remove(path))
        throw std::runtime_error("Cannot remove stale Codex endpoint");
}
// Codex root options before the subcommand that take the next argument as a
// value. Keep this synchronized with the installed `codex --help` surface.
bool codex_option_takes_value(const QString& argument) {
    static const QSet<QString> options{QStringLiteral("-c"),
                                       QStringLiteral("--config"),
                                       QStringLiteral("--enable"),
                                       QStringLiteral("--disable"),
                                       QStringLiteral("--remote"),
                                       QStringLiteral("--remote-auth-token-env"),
                                       QStringLiteral("-i"),
                                       QStringLiteral("--image"),
                                       QStringLiteral("-m"),
                                       QStringLiteral("--model"),
                                       QStringLiteral("--local-provider"),
                                       QStringLiteral("-p"),
                                       QStringLiteral("--profile"),
                                       QStringLiteral("-s"),
                                       QStringLiteral("--sandbox"),
                                       QStringLiteral("-C"),
                                       QStringLiteral("--cd"),
                                       QStringLiteral("--add-dir"),
                                       QStringLiteral("-a"),
                                       QStringLiteral("--ask-for-approval")};
    return options.contains(argument);
}
// A Codex approval or sandbox option: its config key and value, or nothing.
// `consumed` is how many arguments it spans. Values are Codex's own words,
// never anything that could break out of a TOML string.
struct CodexPermission {
    QStringList config; // key=value pairs
    qsizetype consumed = 0;
    bool recognized = false;
    QString error;
};
struct CodexPermissionSpec {
    QString key;
    bool cli_option;
    QStringList shared_values;
    QStringList config_only_values;

    [[nodiscard]] QStringList option_values() const { return shared_values; }
    [[nodiscard]] QStringList config_values() const { return shared_values + config_only_values; }
};
const std::vector<CodexPermissionSpec>& codex_permission_specs() {
    static const std::vector<CodexPermissionSpec> specs{
        {QStringLiteral("approval_policy"),
         true,
         {QStringLiteral("on-request"), QStringLiteral("never")},
         {QStringLiteral("untrusted")}},
        {QStringLiteral("sandbox_mode"),
         true,
         {QStringLiteral("read-only"), QStringLiteral("workspace-write"),
          QStringLiteral("danger-full-access")},
         {}},
        {QStringLiteral("approvals_reviewer"),
         false,
         {},
         {QStringLiteral("user"), QStringLiteral("auto_review"),
          QStringLiteral("guardian_subagent")}},
    };
    return specs;
}
const CodexPermissionSpec* codex_permission_spec(const QString& key) {
    const auto& specs = codex_permission_specs();
    const auto found = std::find_if(specs.cbegin(), specs.cend(),
                                    [&](const auto& spec) { return spec.key == key; });
    return found == specs.cend() ? nullptr : &*found;
}
QString codex_permission_key_pattern() {
    QStringList keys;
    for (const auto& spec : codex_permission_specs())
        keys << QRegularExpression::escape(spec.key);
    return keys.join(QLatin1Char('|'));
}
bool codex_permission_value_valid(const QStringList& values, const QString& value) {
    return std::find(values.cbegin(), values.cend(), value) != values.cend();
}
CodexPermission codex_permission(const QStringList& arguments, qsizetype index) {
    const auto& argument = arguments.at(index);
    if (argument == QStringLiteral("--dangerously-bypass-approvals-and-sandbox"))
        return {{QStringLiteral("approval_policy=\"never\""),
                 QStringLiteral("sandbox_mode=\"danger-full-access\"")},
                1,
                true,
                {}};
    if (argument == QStringLiteral("--full-auto"))
        return {{QStringLiteral("approval_policy=\"on-request\""),
                 QStringLiteral("sandbox_mode=\"workspace-write\"")},
                1,
                true,
                {}};
    if (argument == QStringLiteral("--yolo"))
        return {{QStringLiteral("approval_policy=\"never\""),
                 QStringLiteral("sandbox_mode=\"danger-full-access\"")},
                1,
                true,
                {}};
    if (argument == QStringLiteral("--approve-for-me"))
        return {{QStringLiteral("approval_policy=\"on-request\""),
                 QStringLiteral("sandbox_mode=\"workspace-write\""),
                 QStringLiteral("approvals_reviewer=\"auto_review\"")},
                1,
                true,
                {}};
    static const std::array<std::array<const char*, 3>, 2> valued{
        {{"-a", "--ask-for-approval", "approval_policy"}, {"-s", "--sandbox", "sandbox_mode"}}};
    for (const auto& [short_name, long_name, key] : valued) {
        QString value;
        qsizetype consumed = 0;
        if (argument == QLatin1String(short_name) || argument == QLatin1String(long_name)) {
            if (index + 1 == arguments.size())
                return {{}, 1, true, QStringLiteral("Codex permission option requires a value")};
            value = arguments.at(index + 1);
            consumed = 2;
        } else if (argument.startsWith(QLatin1String(long_name) + QLatin1Char('=')) ||
                   argument.startsWith(QLatin1String(short_name) + QLatin1Char('=')) ||
                   (argument.startsWith(QLatin1String(short_name)) &&
                    argument.size() > QLatin1String(short_name).size())) {
            const auto prefix = argument.startsWith(QLatin1String(long_name) + QLatin1Char('='))
                                    ? qsizetype(std::strlen(long_name)) + 1
                                : argument.startsWith(QLatin1String(short_name) + QLatin1Char('='))
                                    ? qsizetype(std::strlen(short_name)) + 1
                                    : qsizetype(std::strlen(short_name));
            value = argument.mid(prefix);
            consumed = 1;
        } else {
            continue;
        }
        const auto* spec = codex_permission_spec(QLatin1String(key));
        if (!spec || !spec->cli_option)
            return {{},
                    consumed,
                    true,
                    QStringLiteral("Codex permission option cannot be set from the CLI: %1")
                        .arg(QLatin1String(key))};
        const auto accepted = spec->option_values();
        if (!codex_permission_value_valid(accepted, value))
            return {{},
                    consumed,
                    true,
                    QStringLiteral("Codex permission value is invalid for %1; accepted: %2")
                        .arg(QLatin1String(key), accepted.join(QLatin1String(", ")))};
        return {{QStringLiteral("%1=\"%2\"").arg(QLatin1String(key), value)}, consumed, true, {}};
    }
    return {};
}
// `-c key=value` carrying a permission key is an explicit override to Codex, so
// a resuming TUI must not carry it; the server still receives the whole pair.
// `consumed` is how many arguments the pair spans, 0 when it is not one.
CodexPermission codex_permission_config(const QStringList& arguments, qsizetype index) {
    const auto& argument = arguments.at(index);
    QString value;
    if (argument == QStringLiteral("-c") || argument == QStringLiteral("--config")) {
        if (index + 1 == arguments.size())
            return {};
        value = arguments.at(index + 1);
    } else if (argument.startsWith(QStringLiteral("--config="))) {
        value = argument.mid(qsizetype(std::strlen("--config=")));
    } else if (argument.startsWith(QStringLiteral("-c="))) {
        value = argument.mid(qsizetype(std::strlen("-c=")));
    } else {
        return {};
    }
    static const QRegularExpression permission_key(
        QStringLiteral("^\\s*(") + codex_permission_key_pattern() + QStringLiteral(")\\s*="));
    const auto key_match = permission_key.match(value);
    if (!key_match.hasMatch())
        return {};
    const qsizetype consumed = argument.contains(QLatin1Char('=')) ? 1 : 2;
    const auto key = key_match.capturedView(1).toString();
    const auto* spec = codex_permission_spec(key);
    if (!spec)
        return {{},
                consumed,
                true,
                QStringLiteral("Codex permission key has no registered values: %1").arg(key)};
    auto rhs = value.mid(key_match.capturedEnd(0)).trimmed();
    QString configured;
    if (rhs.startsWith(QLatin1Char('"'))) {
        static const QRegularExpression quoted_value(QStringLiteral("^\"([^\"]*)\"$"));
        const auto quoted_match = quoted_value.match(rhs);
        if (!quoted_match.hasMatch())
            return {{},
                    consumed,
                    true,
                    QStringLiteral(
                        "Codex permission config syntax is invalid for %1; expected key=value or "
                        "key=\"value\"")
                        .arg(key)};
        configured = quoted_match.capturedView(1).toString();
    } else {
        configured = rhs;
    }
    const auto accepted = spec->config_values();
    if (!codex_permission_value_valid(accepted, configured))
        return {{},
                consumed,
                true,
                QStringLiteral("Codex permission value is invalid for %1; accepted: %2")
                    .arg(key, accepted.join(QLatin1String(", ")))};
    return {{QStringLiteral("%1=\"%2\"").arg(key, configured)}, consumed, true, {}};
}
// Whether the arguments resume or fork a saved Codex thread.
bool codex_continues_thread(const QStringList& arguments) {
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments.at(index);
        if (argument == QStringLiteral("--"))
            return false;
        if (codex_option_takes_value(argument)) {
            ++index;
            continue;
        }
        if (!argument.startsWith(QLatin1Char('-')))
            return argument == QStringLiteral("resume") || argument == QStringLiteral("fork");
    }
    return false;
}

class SessionService final : public QObject {
  public:
    SessionService(const QString& endpoint, const QByteArray& requested_session_id,
                   const LaunchSpec& launch, const QByteArray& requested_session_epoch = {})
        : lock_(endpoint + QStringLiteral(".lock")), terminal_(launch.size, limits()),
          fingerprint_(launch_fingerprint(launch)),
          identity_{requested_session_id,
                    requested_session_epoch.isEmpty() ? wire::new_id() : requested_session_epoch},
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
        if (log.size() > qint64{1024} * 1024 && log.open(QIODevice::ReadWrite))
            static_cast<void>(log.resize(0));
        if (launch.agent != AgentMode::terminal)
            open_attention_journal(endpoint);
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
        connect(&timer_, &QTimer::timeout, this, [this] { publish(); });
        ack_timer_.setSingleShot(true);
        ack_timer_.setInterval(launch.agent == AgentMode::codex ? codex_sync_timeout_ms
                                                                : terminal_sync_timeout_ms);
        attention_timer_.setSingleShot(true);
        attention_timer_.setInterval(16);
        connect(&attention_timer_, &QTimer::timeout, this, [this] { publish_attention(); });
        rollout_timer_.setInterval(5000);
        connect(&rollout_timer_, &QTimer::timeout, this, [this] { probe_rollout(); });
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
    const attention::State* attention_state() const {
        return codex_state_ ? codex_state_.get() : claude_state_.get();
    }
    void start_claude(const LaunchSpec& launch) {
        claude_state_ = std::make_unique<attention::State>(
            identity_.session_id.toHex().toStdString(), "claude-code");
        claude_observer_ = std::make_unique<lapis::claude::Observer>(*claude_state_);
        connect(claude_observer_.get(), &lapis::claude::Observer::changed, this, [this] {
            note_conversation(QStringLiteral("claude"), claude_observer_->sessionId());
            decision_error_.clear();
            attention_dirty_ = true;
            journal_attention();
            schedule_attention();
        });
        auto terminal = launch;
        terminal.agent = AgentMode::terminal;
        terminal.arguments = claude_observer_->launchArguments(
            launch.arguments, QCoreApplication::applicationFilePath());
        pty_.start(terminal);
    }
    void receive_output(const QByteArray& bytes) {
        timing_.pty_read_ns = monotonic_ns();
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
        if (const auto found = checkpoint_scanner_.scan(bytes);
            found && found->agent == checkpoint_agent_)
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
            (codex_observer_ || claude_observer_ || resume_.source == ResumeSource::observer ||
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
            const auto since =
                last_attention_publish_.isValid() ? last_attention_publish_.elapsed() : frame_ms;
            attention_timer_.start(since >= frame_ms ? 0 : static_cast<int>(frame_ms - since));
        }
    }
    // The durable audit trail for attention requests. A decision is recorded
    // before it is forwarded (durable-before-effect), so an approval the agent
    // acted on is never missing from the journal. Surfacing needs no barrier
    // because surfacing never approves; only the decision path gates.
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
                .observation_phase = codex_observer_ ? codex_observer_->observationPhase()
                                                     : attention::ObservationPhase::unknown,
                .diagnostic = !decision_error_.isEmpty() ? decision_error_
                              : !codex_error_.isEmpty()  ? codex_error_
                              : codex_observer_          ? codex_observer_->diagnostic()
                                                         : claude_observer_->diagnostic(),
                .requests = {}};
            for (const auto& [id, pending] : state->pending())
                snapshot.requests.push_back({pending, codex_observer_
                                                          ? codex_observer_->details(id)
                                                          : claude_observer_->details(id)});
            const auto bytes =
                wire::frame(wire::Kind::attention_snapshot,
                            wire::encode_attention_snapshot(snapshot, client_attention_phase_));
            if (destination->bytesToWrite() + bytes.size() > wire::max_frame_bytes ||
                destination->write(bytes) != bytes.size())
                throw std::runtime_error("Attention output queue unavailable");
            last_attention_publish_.start();
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
        rollout_timer_.stop();
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
        // Codex refuses approval and sandbox options when a remote TUI resumes
        // or forks a thread ("Permission overrides are not supported when
        // resuming a remote task"). The dedicated server already holds them as
        // config, so the TUI goes without.
        if (codex_continues_thread(tui.arguments)) {
            QStringList kept;
            for (qsizetype index = 0; index < tui.arguments.size(); ++index) {
                if (tui.arguments.at(index) == QStringLiteral("--")) {
                    kept += tui.arguments.mid(index);
                    break;
                }
                if (const auto config = codex_permission_config(tui.arguments, index);
                    config.consumed > 0) {
                    index += config.consumed - 1;
                    continue;
                }
                if (const auto permission = codex_permission(tui.arguments, index);
                    permission.consumed > 0) {
                    index += permission.consumed - 1;
                    continue;
                }
                kept << tui.arguments.at(index);
                if (codex_option_takes_value(tui.arguments.at(index)) &&
                    index + 1 < tui.arguments.size())
                    kept << tui.arguments.at(++index);
            }
            tui.arguments = kept;
        }
        tui.arguments.prepend(QStringLiteral("unix://") + backend_socket);
        tui.arguments.prepend(QStringLiteral("--remote"));
        pty_requested_ = true;
        pty_.start(tui);
    }
    static QStringList codex_arguments(const LaunchSpec& launch, const QString& backend_socket) {
        QStringList arguments{QStringLiteral("app-server"), QStringLiteral("--listen"),
                              QStringLiteral("unix://") + backend_socket};
        // Forward explicit provider/config definitions to the server as well as
        // the TUI, and approval/sandbox options as config, since a resumed
        // thread takes them only from the server. Production otherwise inherits
        // the user's ordinary Codex policy.
        for (qsizetype index = 0; index < launch.arguments.size(); ++index) {
            const auto& argument = launch.arguments.at(index);
            if (argument == QStringLiteral("--"))
                break;
            if (const auto permission = codex_permission(launch.arguments, index);
                permission.consumed > 0) {
                if (permission.recognized && !permission.error.isEmpty())
                    throw std::invalid_argument(permission.error.toStdString());
                for (const auto& pair : permission.config)
                    arguments << QStringLiteral("-c") << pair;
                index += permission.consumed - 1;
            } else if (const auto config = codex_permission_config(launch.arguments, index);
                       config.consumed > 0) {
                if (config.recognized && !config.error.isEmpty())
                    throw std::invalid_argument(config.error.toStdString());
                for (const auto& pair : config.config)
                    arguments << QStringLiteral("-c") << pair;
                index += config.consumed - 1;
            } else if (argument == QStringLiteral("-c") || argument == QStringLiteral("--config") ||
                       argument == QStringLiteral("--enable") ||
                       argument == QStringLiteral("--disable")) {
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
        const auto backend_path = endpoint + QStringLiteral(".codex");
        remove_dead_codex_link(backend_path);
        const auto backend_socket = posix::prepare_endpoint(backend_path);
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
            note_conversation(QStringLiteral("codex"), codex_observer_->threadId());
            decision_error_.clear();
            attention_dirty_ = true;
            journal_attention();
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
        // The backend holds each loaded thread's rollout open. Reading its open
        // files names the conversation whatever Codex build runs, so resuming
        // after a reboot does not depend on the observer accepting this build.
        rollout_timer_.start();
        wait_for_codex(launch, backend_socket, binary_hash);
    }
    void probe_rollout() {
        const auto pid = codex_backend_.processId();
        if (pid <= 0 || stopping_ || rollout_probing_ ||
            (codex_observer_ && !codex_observer_->threadId().isEmpty()))
            return;
#ifdef Q_OS_MACOS
        auto* probe = new QProcess(this);
        rollout_probing_ = true;
        connect(probe, &QProcess::finished, this, [this, probe] {
            rollout_probing_ = false;
            note_rollout(QString::fromUtf8(probe->readAllStandardOutput()));
            probe->deleteLater();
        });
        connect(probe, &QProcess::errorOccurred, this, [this, probe](QProcess::ProcessError) {
            rollout_probing_ = false;
            probe->deleteLater();
        });
        probe->setStandardInputFile(QProcess::nullDevice());
        probe->start(QStringLiteral("/usr/sbin/lsof"),
                     {QStringLiteral("-p"), QString::number(pid), QStringLiteral("-Fn")});
#else
        QString listing;
        const QDir descriptors(QStringLiteral("/proc/%1/fd").arg(pid));
        for (const auto& entry : descriptors.entryInfoList(QDir::Files | QDir::System))
            listing += QStringLiteral("n") + entry.symLinkTarget() + QLatin1Char('\n');
        note_rollout(listing);
#endif
    }
    void note_rollout(const QString& open_files) {
        if (const auto thread = codex_thread_from_open_files(open_files)) {
            note_conversation(QStringLiteral("codex"), *thread);
            rollout_timer_.setInterval(60000); // found; keep following /new slowly
        }
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
        const bool admission_deadline = output_admission_expired_;
        output_admission_expired_ = false;
        if (output_pressure_ && !admission_deadline &&
            pending_output_.size() < output_admission_limit) {
            // Sustained pressure waits for its real admission boundary, but the
            // timer remains the bound if the producer quiets below it.
            if (!output_admission_timer_.isActive())
                output_admission_timer_.start(static_cast<int>(frame_ms));
            schedule_output_admission();
            processing_output_ = false;
            return;
        }
        output_admission_timer_.stop();
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
                mark_dirty();
                timing_.parse_end_ns = monotonic_ns();
            }
            const auto replies = terminal_.take_replies();
            if (!replies.empty() && pty_.processId() != 0 &&
                !pty_.writeBytes(
                    QByteArray(replies.data(), static_cast<qsizetype>(replies.size()))))
                throw std::runtime_error("PTY reply queue overflow");
            schedule();
            if (!output_waiting_) {
                if (pending_output_.isEmpty()) {
                    if (output_pressure_) {
                        output_pressure_ = false;
                        output_retention_logged_ = false;
                        output_pressure_bytes_ = 0;
                        output_pressure_clock_.invalidate();
                        qInfo().noquote() << "PTY output retention drained; output resumed";
                    }
                    pty_.pauseOutput(false);
                } else {
                    QTimer::singleShot(0, this, [this] { process_output(); });
                }
            }
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
        }
        processing_output_ = false;
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
        if (!process_started_ || stopping_ || timer_.isActive())
            return;
        if ((client_ && (!snapshot_in_flight_ || ready_) && dirty_) || views_due()) {
            const auto since = last_publish_.isValid() ? last_publish_.elapsed() : frame_ms;
            timer_.start(since >= frame_ms ? 0 : static_cast<int>(frame_ms - since));
        }
    }
    void publish() {
        last_publish_.start();
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
        const auto decision = wire::decode_attention_decision(payload);
        if (claude_observer_) {
            allow_decision_retry(decision);
            decision_error_ = QStringLiteral("Answer Claude requests in the terminal");
            attention_dirty_ = true;
            schedule_attention();
            return;
        }
        // Unavailable or failing journals refuse the forward, exactly like a
        // decision that cannot be made durable.
        if (!attention_journal_ || attention_journal_failed_) {
            allow_decision_retry(decision);
            decision_error_ = QStringLiteral("Decision could not be recorded; try again");
            attention_dirty_ = true;
            schedule_attention();
            return;
        }
        const AttentionApproval approval{decision.source_epoch, decision.request_id,
                                         decision.revision, decision.choice.toStdString()};
        try {
            if (!record_intended_decision(*attention_journal_, *attention_state(), approval)) {
                allow_decision_retry(decision);
                decision_error_ =
                    QStringLiteral("Request changed or response is unsupported; refresh attention");
                attention_dirty_ = true;
                schedule_attention();
                return;
            }
        } catch (const std::exception& error) {
            qWarning().noquote() << "Attention decision not recorded:" << error.what();
            attention_journal_failed_ = true;
            allow_decision_retry(decision);
            decision_error_ = QStringLiteral("Decision could not be recorded; try again");
            attention_dirty_ = true;
            schedule_attention();
            return;
        }
        if (!codex_observer_->decide(decision.source_epoch, decision.request_id, decision.revision,
                                     decision.choice, decision.answers)) {
            try {
                record_refused_decision(*attention_journal_, approval);
                journaled_.erase(decision.request_id);
            } catch (const std::exception& error) {
                attention_journal_failed_ = true;
                qWarning().noquote() << "Attention rejection not recorded:" << error.what();
            }
            allow_decision_retry(decision);
            decision_error_ =
                QStringLiteral("Request changed or response is unsupported; refresh attention");
            attention_dirty_ = true;
            schedule_attention();
            return;
        }
        try {
            record_delivered_decision(*attention_journal_, approval);
        } catch (const std::exception& error) {
            attention_journal_failed_ = true;
            qWarning().noquote() << "Attention delivery outcome not recorded:" << error.what();
            decision_error_ = QStringLiteral("Delivery outcome is unknown; see the audit journal");
            attention_dirty_ = true;
            schedule_attention();
            return;
        }
        journaled_.erase(decision.request_id);
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
            write_input(frame.kind, control.payload);
            return;
        case wire::Kind::text:
        case wire::Kind::paste:
        case wire::Kind::key:
            claim_size(client_wanted_);
            write_input(frame.kind, control.payload);
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
    void write_input(wire::Kind kind, const QByteArray& payload) {
        // A wheel over the primary screen sends the program nothing.
        const auto bytes = input_bytes(kind, payload);
        if (!bytes.isEmpty() && !pty_.writeBytes(bytes))
            throw std::runtime_error("PTY input queue full");
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
        if (harvest_offset_)
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
        dirty_ = true;
        for (const auto& view : views_)
            view->dirty = true;
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
            write_input(frame.kind, control.payload);
            return;
        }
        if (frame.kind != wire::Kind::text && frame.kind != wire::Kind::paste &&
            frame.kind != wire::Kind::key)
            throw std::runtime_error(
                "A joined view may only type, scroll, resize and page history");
        claim_size(view.wanted, view.id);
        write_input(frame.kind, control.payload);
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
    QSet<QLocalSocket*> retired_;
    QByteArray fingerprint_;
    wire::SessionIdentity identity_;
    wire::Attachment attachment_;
    QByteArray buffer_;
    QTimer timer_;
    QTimer output_admission_timer_;
    QElapsedTimer last_publish_;
    static constexpr qint64 frame_ms = 16;
    QElapsedTimer last_attention_publish_;
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
    wire::SnapshotTiming timing_;
    std::unique_ptr<attention::State> claude_state_;
    std::unique_ptr<lapis::claude::Observer> claude_observer_;
    QString resume_endpoint_;
    CheckpointScanner checkpoint_scanner_;
    QString checkpoint_agent_;
    ResumeRecord resume_;
    std::unique_ptr<attention::State> codex_state_;
    std::unique_ptr<lapis::codex::Observer> codex_observer_;
    QProcess codex_backend_;
    QTimer backend_wait_;
    QLocalSocket backend_probe_;
    QTimer attention_timer_;
    QTimer rollout_timer_;
    bool rollout_probing_{};
    posix::UniqueFd backend_guard_read_;
    posix::UniqueFd backend_guard_control_;
    bool pty_requested_{};
    bool attention_dirty_{};
    std::unique_ptr<AttentionJournal> attention_journal_;
    JournaledAsks journaled_;
    bool journal_append_failed_{};
    bool attention_journal_failed_{};
    QString codex_error_;
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
            options.session_epoch = parse_session_id(value("Expected one --session-epoch HEX32"));
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
    // Before any agent or Codex backend inherits the environment.
    clear_parent_session_markers();
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
