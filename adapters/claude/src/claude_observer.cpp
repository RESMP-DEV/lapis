#include "claude_observer.hpp"
#include "hook_relay.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <map>
#include <stdexcept>
#include <vector>

namespace lapis::claude {
namespace {
namespace attention = session::attention;
constexpr qsizetype connection_limit = 8;
constexpr std::uint32_t connection_overflow_report_limit = 1024;
constexpr qsizetype prompt_limit = 1024;
constexpr qsizetype retired_source_limit = 1024;
constexpr qsizetype completed_tool_limit = qsizetype{16} * 1024;
const QStringList& hook_events() {
    static const QStringList names{
        "SessionStart", "UserPromptSubmit", "PermissionRequest",  "Notification",
        "PreToolUse",   "PostToolUse",      "PostToolUseFailure", "Stop",
        "SessionEnd"};
    return names;
}
attention::Tick now() {
    return static_cast<attention::Tick>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
}
QString quote(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
bool valid_event(const QJsonObject& event) {
    for (auto it = event.begin(); it != event.end(); ++it)
        if (std::none_of(relay_identity_fields.begin(), relay_identity_fields.end(),
                         [key = it.key()](const auto& field) { return key.compare(field) == 0; }) ||
            !it.value().isString() || it.value().toString().toUtf8().size() > 256 ||
            it.value().toString().contains(QChar::Null))
            return false;
    return hook_events().contains(event.value("hook_event_name").toString()) &&
           !event.value("session_id").toString().isEmpty();
}
} // namespace

class Observer::Impl {
  public:
    Impl(attention::State& state, Observer& owner)
        : state_(state), owner_(owner), temporary_(QStringLiteral("/tmp/lapis-hook-XXXXXX")) {
        if (!temporary_.isValid())
            throw std::runtime_error("Cannot create private Claude hook directory");
        socket_ = temporary_.filePath(QStringLiteral("events.sock"));
        nonce_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        server_.setMaxPendingConnections(static_cast<int>(connection_limit));
        if (!server_.listen(socket_))
            throw std::runtime_error("Cannot listen for Claude hooks");
        QObject::connect(&server_, &QLocalServer::newConnection, &server_, [this] { accept(); });
    }
    ~Impl() { close(); }
    void notify() {
        if (notification_pending_)
            return;
        notification_pending_ = true;
        // Publish only after the transport callback has returned. A direct signal
        // receiver may stop or destroy the observer; do not touch this afterward.
        QTimer::singleShot(0, &owner_, [this] {
            notification_pending_ = false;
            emit owner_.changed();
        });
    }
    void close() {
        stopped_ = true;
        server_.close();
        const auto clients = server_.findChildren<QLocalSocket*>();
        clients_.clear();
        for (auto* client : clients) {
            QObject::disconnect(client, nullptr, &server_, nullptr);
            delete client;
        }
    }
    void stop() {
        if (stopped_)
            return;
        close();
        state_.disconnect();
        diagnostic_ = QStringLiteral("Claude hook observation stopped");
        notify();
    }
    void connection_overflow() {
        // A refused connection is unauthenticated. Report degraded observation,
        // but never let local connection spam desynchronize the attention state.
        if (failed_ || connection_overflows_ == connection_overflow_report_limit)
            return;
        ++connection_overflows_;
        diagnostic_ =
            QStringLiteral(
                "Claude hook connection limit exceeded (at least %1); hooks may be missing")
                .arg(connection_overflows_);
        notify();
    }
    QStringList launch(const QStringList& original, const QString& executable) {
        if (stopped_ || executable.isEmpty() || executable.contains(QChar::Null))
            throw std::invalid_argument("Claude hook receiver is unavailable");
        QJsonObject hooks;
        const auto command = quote(executable) + QStringLiteral(" --claude-hook ") +
                             quote(socket_) + QLatin1Char(' ') + quote(nonce_);
        for (const auto& event : hook_events())
            hooks.insert(
                event, QJsonArray{QJsonObject{{"hooks", QJsonArray{QJsonObject{{"type", "command"},
                                                                               {"command", command},
                                                                               {"timeout", 2}}}}}});
        QFile settings(temporary_.filePath(QStringLiteral("settings.json")));
        const auto data =
            QJsonDocument(QJsonObject{{"hooks", hooks}}).toJson(QJsonDocument::Compact);
        if (!settings.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            !settings.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
            settings.write(data) != data.size() || !settings.flush())
            throw std::runtime_error("Cannot write private Claude hook settings");
        return QStringList{QStringLiteral("--settings"), settings.fileName()} + original;
    }
    void accept() {
        while (auto* client = server_.nextPendingConnection()) {
            if (stopped_ || clients_.size() >= connection_limit) {
                if (!stopped_)
                    connection_overflow();
                delete client;
                continue;
            }
            client->setParent(&server_);
            client->setReadBufferSize(relay_frame_limit + 1);
            clients_.insert(client, {});
            QObject::connect(client, &QLocalSocket::readyRead, &server_,
                             [this, client] { read(client); });
            QObject::connect(client, &QLocalSocket::disconnected, &server_, [this, client] {
                clients_.remove(client);
                client->deleteLater();
            });
            QTimer::singleShot(1000, client, [client] {
                client->abort();
                client->deleteLater();
            });
            read(client);
        }
    }
    void read(QLocalSocket* client) {
        auto found = clients_.find(client);
        if (found == clients_.end())
            return;
        found.value() += client->read(relay_frame_limit + 1 - found.value().size());
        if (found.value().size() > relay_frame_limit) {
            client->abort();
            return;
        }
        if (!found.value().contains('\n'))
            return;
        const auto data = std::move(found.value());
        clients_.erase(found);
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(data, &error);
        const auto envelope = document.object();
        if (error.error == QJsonParseError::NoError && document.isObject() &&
            envelope.value("nonce").toString() == nonce_) {
            try {
                if (envelope.size() != 2 || !envelope.value("event").isObject() ||
                    !valid_event(envelope.value("event").toObject()))
                    loss(QStringLiteral("Malformed authenticated Claude hook"));
                else
                    receive(envelope.value("event").toObject());
                static_cast<void>(client->write(QByteArrayLiteral("ACK")));
            } catch (const std::exception&) {
                loss(QStringLiteral("Claude observation could not be applied"));
            }
        }
        client->disconnectFromServer();
    }
    void loss(const QString& message) {
        state_.overflow();
        diagnostic_ = message + QStringLiteral("; restart the session to restore observation");
        failed_ = true;
        notify();
    }
    attention::Position next() { return {state_.epoch(), ++sequence_}; }
    bool applied(attention::Outcome outcome) {
        if (outcome == attention::Outcome::applied || outcome == attention::Outcome::duplicate)
            return true;
        loss(QStringLiteral("Claude observation lost synchronization or exceeded its bounds"));
        return false;
    }
    void begin() {
        state_.connect(state_.epoch() + 1, {true, false, false});
        sequence_ = 1;
        if (!applied(state_.begin_observation({state_.epoch(), sequence_}, now())))
            return;
        diagnostic_ = QStringLiteral("Claude hooks observe attention only; answer in the terminal");
    }
    void clear() {
        std::vector<attention::RequestId> ids;
        for (const auto& [id, pending] : state_.pending()) {
            static_cast<void>(pending);
            ids.push_back(id);
        }
        for (const auto& id : ids) {
            if (!applied(state_.resolve(next(), id)))
                return;
            details_.erase(id);
        }
    }
    void request(const QJsonObject& event, bool exact, bool input, bool idle = false) {
        const auto tool = event.value("tool_use_id").toString();
        if (exact && completed_tools_.contains(tool))
            return;
        const auto id =
            exact ? tool.toStdString()
                  : ((idle ? QStringLiteral("idle:") : QStringLiteral("permission:")) +
                     QString::fromLatin1(
                         QCryptographicHash::hash(prompt_.toUtf8(), QCryptographicHash::Sha256)
                             .toHex()))
                        .toStdString();
        if (state_.pending().contains(id))
            return;
        attention::Request request{.id = id,
                                   .thread_id = pinned_.toStdString(),
                                   .turn_id = prompt_.toStdString(),
                                   .item_id = exact ? tool.toStdString() : std::string{},
                                   .reason = idle    ? "idle"
                                             : input ? "input"
                                                     : "approval",
                                   .summary = idle    ? "Claude is waiting for input"
                                              : input ? "Claude has a question"
                                                      : "Claude permission request",
                                   .choices = {},
                                   .priority = static_cast<std::uint8_t>(idle ? 1 : 3)};
        const auto result = state_.request(next(), request, now());
        if (result == attention::Outcome::applied)
            details_[id] = {{"adapter", "claude-code"},
                            {"responseLocation", "terminal"},
                            {"observationOnly", true},
                            {"toolName", event.value("tool_name")},
                            {"hookEvent", event.value("hook_event_name")},
                            {"semantics", exact ? "tool" : "advisory"}};
        else
            applied(result);
    }
    void receive(const QJsonObject& event) {
        if (stopped_ || failed_)
            return;
        const auto session = event.value("session_id").toString();
        const auto name = event.value("hook_event_name").toString();
        const auto prompt = event.value("prompt_id").toString();
        if (retired_sources_.contains(session))
            return;
        if (!pinned_.isEmpty() && pinned_ != session)
            return;
        if (pinned_.isEmpty()) {
            if (awaiting_start_ && name != QLatin1String("SessionStart"))
                return;
            if (name != QLatin1String("SessionStart") && name != QLatin1String("UserPromptSubmit"))
                return;
            pinned_ = session;
            awaiting_start_ = false;
            begin();
        }
        if (!state_.ready())
            return;
        if (name == QLatin1String("SessionEnd")) {
            if (retired_sources_.size() >= retired_source_limit) {
                loss(QStringLiteral("Claude retired session identity bound exceeded"));
                return;
            }
            clear();
            if (failed_)
                return;
            retired_sources_.insert(pinned_);
            state_.disconnect();
            pinned_.clear();
            prompt_.clear();
            prompts_.clear();
            completed_ = false;
            awaiting_start_ = true;
            // /clear ends a conversation, not the service-owned Claude process.
            // Only a fresh SessionStart may bind its replacement; delayed old
            // hooks cannot resurrect the retired conversation.
            diagnostic_ = QStringLiteral("Claude session ended; waiting for a new session hook");
            notify();
            return;
        }
        if (name == QLatin1String("SessionStart")) {
            notify();
            return;
        }
        if (name == QLatin1String("UserPromptSubmit"))
            new_prompt(prompt);
        else if (!prompt_.isEmpty() && prompt == prompt_)
            turn_event(event);
        notify();
    }
    void new_prompt(const QString& prompt) {
        if (prompt.isEmpty()) {
            loss(QStringLiteral("Claude prompt boundary has no identity"));
            return;
        }
        if (prompts_.contains(prompt))
            return;
        if (prompts_.size() >= prompt_limit) {
            loss(QStringLiteral("Claude prompt identity bound exceeded"));
            return;
        }
        clear();
        if (failed_)
            return;
        begin(); // A new known turn resets bounded tombstones only after exact retirement.
        if (failed_)
            return;
        // Only a successfully applied prompt boundary may forget completed IDs.
        // Duplicate and failed boundaries retain them for delayed-event rejection.
        completed_tools_.clear();
        prompts_.insert(prompt);
        prompt_ = prompt;
        completed_ = false;
        applied(state_.activity(next(), attention::Activity::working));
    }
    void permission(const QJsonObject& event) {
        // AskUserQuestion can emit both a tool hook and a permission hook. The
        // exact input notice already directs the user to that terminal; a second
        // advisory row adds no response capability or resolution evidence.
        if (event.value("tool_name") == QLatin1String("AskUserQuestion") &&
            std::any_of(state_.pending().begin(), state_.pending().end(),
                        [](const auto& item) { return item.second.request.reason == "input"; }))
            return;
        request(event, !event.value("tool_use_id").toString().isEmpty(), false);
    }
    void complete_tool(const QString& tool) {
        const auto id = tool.toStdString();
        if (completed_tools_.contains(tool))
            return;
        if (completed_tools_.size() >= completed_tool_limit) {
            loss(QStringLiteral("Claude completed tool identity bound exceeded"));
            return;
        }
        completed_tools_.insert(tool);
        // Ordinary completions are adapter-local tombstones. Resolve only
        // an exact attention request so ordinary traffic cannot consume
        // State's shared retired-ID budget.
        if (state_.pending().contains(id)) {
            if (!applied(state_.resolve(next(), id)))
                return;
            details_.erase(id);
        }
    }
    void turn_event(const QJsonObject& event) {
        const auto name = event.value("hook_event_name").toString();
        if (name == QLatin1String("Stop")) {
            if (completed_)
                return;
            clear();
            if (failed_)
                return;
            completed_ = true;
            applied(state_.activity(next(), attention::Activity::turn_completed));
        } else if (name == QLatin1String("Notification") &&
                   event.value("notification_type") == QLatin1String("idle_prompt")) {
            request(event, false, false, true);
        } else if (!completed_) {
            const auto tool = event.value("tool_use_id").toString();
            if (name == QLatin1String("PermissionRequest"))
                permission(event);
            else if (name == QLatin1String("Notification") &&
                     event.value("notification_type") == QLatin1String("permission_prompt")) {
                const bool pending = std::any_of(
                    state_.pending().begin(), state_.pending().end(),
                    [](const auto& item) { return item.second.request.reason == "approval"; });
                if (!pending)
                    request(event, false, false);
            } else if (name == QLatin1String("PreToolUse") && !tool.isEmpty() &&
                       event.value("tool_name") == QLatin1String("AskUserQuestion"))
                request(event, true, true);
            else if ((name == QLatin1String("PostToolUse") ||
                      name == QLatin1String("PostToolUseFailure")) &&
                     !tool.isEmpty()) {
                complete_tool(tool);
            }
        }
    }
    attention::State& state_;
    Observer& owner_;
    QTemporaryDir temporary_;
    QLocalServer server_;
    QHash<QLocalSocket*, QByteArray> clients_;
    QString socket_;
    QString nonce_;
    QString pinned_;
    QString prompt_;
    QSet<QString> prompts_;
    QSet<QString> retired_sources_;
    QSet<QString> completed_tools_;
    std::map<attention::RequestId, QJsonObject> details_;
    QString diagnostic_{QStringLiteral("Waiting for a Claude session hook; hooks may be disabled")};
    std::uint32_t connection_overflows_{};
    std::uint64_t sequence_{};
    bool notification_pending_{};
    bool stopped_{};
    bool failed_{};
    bool completed_{};
    bool awaiting_start_{};
};
Observer::Observer(attention::State& state, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(state, *this)) {}
Observer::~Observer() = default;
const QString& Observer::diagnostic() const { return impl_->diagnostic_; }
QJsonObject Observer::details(const attention::RequestId& id) const {
    const auto found = impl_->details_.find(id);
    return found == impl_->details_.end() ? QJsonObject{} : found->second;
}
QStringList Observer::launchArguments(const QStringList& original, const QString& executable) {
    return impl_->launch(original, executable);
}
void Observer::stop() { impl_->stop(); }
} // namespace lapis::claude
