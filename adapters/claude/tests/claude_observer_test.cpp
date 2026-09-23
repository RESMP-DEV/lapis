#include "claude_observer.hpp"
#include "hook_relay.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <array>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
namespace attention = lapis::session::attention;
void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Claude hook check failed at line " +
                                 std::to_string(where.line()));
}
void finish(QProcess& process) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    timer.start(5000);
    if (process.state() != QProcess::NotRunning)
        loop.exec();
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        static_cast<void>(process.waitForFinished(1000));
        throw std::runtime_error("Hook deadline exceeded");
    }
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
    require(process.readAllStandardOutput().isEmpty() && process.readAllStandardError().isEmpty());
}
// Compact fixture builder follows the hook schema order: event, tool, tool ID, prompt ID.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QJsonObject event(const QString& name, const QString& tool = {}, const QString& id = {},
                  const QString& prompt = QStringLiteral("turn-1")) {
    QJsonObject result{
        {"hook_event_name", name}, {"session_id", "source-1"}, {"prompt_id", prompt}};
    if (!tool.isEmpty())
        result.insert("tool_name", tool);
    if (!id.isEmpty())
        result.insert("tool_use_id", id);
    return result;
}
struct Fixture {
    attention::State state{"session", "claude-code"};
    std::unique_ptr<lapis::claude::Observer> owned_observer{
        std::make_unique<lapis::claude::Observer>(state)};
    lapis::claude::Observer& observer{*owned_observer};
    QString command;
    QString socket;
    QString nonce;
    Fixture() {
        const auto arguments = observer.launchArguments({"--", "literal --settings"},
                                                        QCoreApplication::applicationFilePath());
        require(arguments.size() == 4 && arguments[0] == QStringLiteral("--settings"));
        QFile settings(arguments[1]);
        require(settings.open(QIODevice::ReadOnly));
        const auto hooks = QJsonDocument::fromJson(settings.readAll()).object()["hooks"].toObject();
        require(hooks.size() == 9);
        require(!hooks.contains(QStringLiteral("StopFailure")));
        command = hooks["SessionStart"]
                      .toArray()[0]
                      .toObject()["hooks"]
                      .toArray()[0]
                      .toObject()["command"]
                      .toString();
        nonce = command.section(QLatin1Char('\''), -2, -2);
        socket = QFileInfo(arguments[1]).absolutePath() + QStringLiteral("/events.sock");
    }
    void raw(const QByteArray& input, bool close = true) {
        QProcess process;
        process.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
        require(process.waitForStarted(1000));
        static_cast<void>(process.write(input));
        if (close)
            process.closeWriteChannel();
        finish(process);
    }
    void send(const QJsonObject& source) {
        QLocalSocket connection;
        connection.connectToServer(socket);
        if (!connection.waitForConnected(1000)) {
            require(!state.connected());
            return;
        }
        const auto payload = QJsonDocument(QJsonObject{{"nonce", nonce}, {"event", source}})
                                 .toJson(QJsonDocument::Compact) +
                             '\n';
        require(connection.write(payload) == payload.size());
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&connection, &QLocalSocket::readyRead, &loop, &QEventLoop::quit);
        timer.start(1000);
        loop.exec();
        require(connection.readAll() == QByteArrayLiteral("ACK"));
    }
    void begin() {
        raw(QJsonDocument(event("SessionStart")).toJson(QJsonDocument::Compact));
        require(state.ready());
        send(event("UserPromptSubmit"));
        require(state.ready() && state.activity() == attention::Activity::working);
    }
};
void callback_shutdown() {
    Fixture f;
    int notifications = 0;
    QObject::connect(&f.observer, &lapis::claude::Observer::changed, &f.observer, [&] {
        ++notifications;
        f.observer.stop();
    });
    f.raw(QJsonDocument(event("SessionStart")).toJson(QJsonDocument::Compact));
    QCoreApplication::processEvents();
    require(!f.state.connected() && notifications == 2);
    f.observer.stop();
    QCoreApplication::processEvents();
    require(notifications == 2);
}
void callback_destruction() {
    Fixture f;
    QObject::connect(&f.observer, &lapis::claude::Observer::changed, QCoreApplication::instance(),
                     [&] { f.owned_observer.reset(); });
    f.raw(QJsonDocument(event("SessionStart")).toJson(QJsonDocument::Compact));
    require(!f.owned_observer);
    QCoreApplication::processEvents();
}
void idle_notice_retirement() {
    Fixture f;
    f.begin();
    auto idle = event("Notification");
    idle.insert("notification_type", "idle_prompt");
    f.send(idle);
    require(f.state.pending().size() == 1);
    f.send(event("Stop"));
    require(f.state.pending().empty());
    f.send(idle); // Same-prompt duplicates cannot resurrect a retired notice.
    require(f.state.pending().empty());
    f.send(event("UserPromptSubmit", {}, {}, "next-prompt"));
    idle.insert("prompt_id", "next-prompt");
    f.send(idle);
    require(f.state.pending().size() == 1);
}
void identities_and_boundaries() {
    Fixture f;
    f.send(event("PermissionRequest", "Bash"));
    require(!f.state.ready());
    f.begin();
    f.send(event("PermissionRequest", "Bash"));
    require(f.state.pending().size() == 1);
    const auto pending = f.state.pending().begin()->second;
    require(pending.request.choices.empty() && pending.request.item_id.empty());
    require(f.observer.details(pending.request.id)["responseLocation"] ==
            QLatin1String("terminal"));
    require(!f.state.respond(f.state.epoch(), pending.request.id, pending.revision, "allow"));
    f.send(event("PermissionRequest", "Bash"));
    auto notification = event("Notification");
    notification.insert("notification_type", "permission_prompt");
    f.send(notification);
    require(f.state.pending().size() == 1 &&
            f.state.pending().begin()->second.revision == pending.revision);
    f.send(event("PostToolUse", "Bash", "unrelated-tool"));
    require(f.state.pending().size() == 1);
    auto foreign = event("Stop");
    foreign.insert("session_id", "different-source");
    f.send(foreign);
    require(f.state.pending().size() == 1);
    f.send(event("UserPromptSubmit", {}, {}, "turn-2"));
    require(f.state.ready() && f.state.pending().empty());
    f.send(event("PermissionRequest", "Bash", {}, "turn-2"));
    f.send(event("Stop"));             // A late old-turn completion cannot clear the new request.
    f.send(event("UserPromptSubmit")); // Nor can a duplicated old boundary.
    require(f.state.pending().size() == 1);
    f.send(event("Stop", {}, {}, "turn-2"));
    require(f.state.pending().empty() && f.state.activity() == attention::Activity::turn_completed);
    f.send(event("PermissionRequest", "Bash", {}, "turn-2"));
    require(f.state.pending().empty());
    auto idle = event("Notification", {}, {}, "turn-2");
    idle.insert("notification_type", "idle_prompt");
    f.send(idle);
    f.send(event("Stop", {}, {}, "turn-2"));
    require(f.state.pending().size() ==
            1); // Duplicate completion cannot erase a newer idle notice.
}
void exact_retirement() {
    Fixture f;
    f.begin();
    f.send(event("PreToolUse", "AskUserQuestion", "first"));
    f.send(event("PreToolUse", "AskUserQuestion", "second"));
    f.send(event("PermissionRequest", "AskUserQuestion"));
    require(f.state.pending().size() == 2);
    f.send(event("PostToolUseFailure", "AskUserQuestion", "first"));
    require(f.state.pending().size() == 1 && f.state.pending().contains(std::string{"second"}));
    f.send(event("PreToolUse", "AskUserQuestion", "first"));
    require(f.state.pending().size() == 1);
    f.send(event("UserPromptSubmit", {}, {}, "next"));
    require(f.state.pending().empty());
    f.send(event("SessionEnd", {}, {}, "next"));
    require(!f.state.connected());
    f.observer.stop();
    f.send(event("SessionStart"));
    require(!f.state.connected());
}

void completed_tool_bounds() {
    Fixture f;
    f.begin();

    // More ordinary completions than State's shared retired-ID bound must remain
    // adapter-local and leave room for later genuine attention.
    for (qsizetype index = 0; index < 1025; ++index)
        f.send(event("PostToolUse", "Bash", QStringLiteral("ordinary-%1").arg(index)));
    require(f.state.ready());

    f.send(event("PreToolUse", "AskUserQuestion", "ordinary-0"));
    f.send(event("PermissionRequest", "Bash", "ordinary-0"));
    require(f.state.ready() && f.state.pending().empty());

    f.send(event("PreToolUse", "AskUserQuestion", "genuine"));
    require(f.state.ready() && f.state.pending().contains(std::string{"genuine"}));

    f.send(event("PostToolUse", "AskUserQuestion", "genuine"));
    require(f.state.ready() && f.state.pending().empty());
    // One genuine completion plus 16,383 ordinary IDs fill the explicit bound.
    for (qsizetype index = 1025; index < 16383; ++index)
        f.send(event("PostToolUse", "Bash", QStringLiteral("ordinary-%1").arg(index)));
    require(f.state.ready());
    f.send(event("PostToolUse", "Bash", "ordinary-0")); // Duplicate at the bound is harmless.
    require(f.state.ready());
    f.send(event("PostToolUse", "Bash", "one-too-many"));
    require(!f.state.ready());
    require(f.observer.diagnostic().contains(
        QLatin1String("Claude completed tool identity bound exceeded")));
}

void completed_tools_reset_at_prompt_epoch() {
    Fixture f;
    f.begin();
    f.send(event("PostToolUse", "AskUserQuestion", "tool"));
    f.send(event("UserPromptSubmit")); // Duplicate boundary must not reset identity.
    f.send(event("PreToolUse", "AskUserQuestion", "tool"));
    require(f.state.pending().empty());
    f.send(event("UserPromptSubmit", {}, {}, "turn-2"));
    f.send(event("PreToolUse", "AskUserQuestion", "tool", "turn-2"));
    require(f.state.pending().contains(std::string{"tool"}));
}

void connection_overflow_is_observable() {
    Fixture f;
    f.begin();
    std::array<QLocalSocket, 8> clients;
    for (auto& client : clients) {
        client.connectToServer(f.socket);
        require(client.waitForConnected(1000));
        QCoreApplication::processEvents();
    }
    QLocalSocket overflow;
    overflow.connectToServer(f.socket);
    require(overflow.waitForConnected(1000));
    QCoreApplication::processEvents();
    require(f.state.ready());
    require(f.observer.diagnostic().contains(
        QLatin1String("Claude hook connection limit exceeded (at least 1)")));

    overflow.disconnectFromServer();
    for (auto& client : clients)
        client.disconnectFromServer();
    QCoreApplication::processEvents();
    f.send(event("PreToolUse", "AskUserQuestion", "after-overload"));
    require(f.state.ready() && f.state.pending().contains(std::string{"after-overload"}));
}

void session_replacement() {
    Fixture f;
    f.begin();
    f.send(event("PreToolUse", "AskUserQuestion", "pending"));
    const auto previous_epoch = f.state.epoch();
    auto end = event("SessionEnd", {}, {}, "different-final-prompt");
    end.insert("reason", "clear");
    f.send(end);
    require(!f.state.connected() && f.state.pending().empty());
    require(f.observer.details(std::string{"pending"}).isEmpty());
    f.send(event("UserPromptSubmit", {}, {}, "late-old-prompt"));
    f.send(event("SessionStart"));
    require(!f.state.connected()); // Old-source hooks cannot repin a retired session.
    auto start = event("SessionStart");
    start.insert("session_id", "source-2");
    start.insert("source", "clear");
    f.send(start);
    require(f.state.ready() && f.state.epoch() > previous_epoch);
    auto prompt = event("UserPromptSubmit"); // Prompt IDs are scoped to the new source.
    prompt.insert("session_id", "source-2");
    f.send(prompt);
    auto request = event("PermissionRequest", "Bash");
    request.insert("session_id", "source-2");
    f.send(request);
    require(f.state.pending().size() == 1 &&
            f.state.pending().begin()->second.request.thread_id == "source-2");
    f.send(end);
    f.send(event("Stop"));
    require(f.state.ready() && f.state.pending().size() == 1);
}
void privacy_bounds_and_transport() {
    Fixture f;
    f.begin();
    auto request = event("PermissionRequest", "Bash");
    request.insert("tool_input", QJsonObject{{"command", "private content"}});
    request.insert("prompt", "private prompt");
    f.raw(QJsonDocument(request).toJson(QJsonDocument::Compact));
    require(!QJsonDocument(f.observer.details(f.state.pending().begin()->first))
                 .toJson()
                 .contains("private"));
    QLocalSocket intruder;
    intruder.connectToServer(f.socket);
    require(intruder.waitForConnected(1000));
    static_cast<void>(intruder.write("{\"nonce\":\"wrong\",\"event\":{}}\n"));
    static_cast<void>(intruder.waitForBytesWritten(1000));
    f.send(event("SessionStart")); // Pump the receiver alongside a real hook.
    require(f.state.ready() && f.state.pending().size() == 1);
    f.raw("not-json");
    require(f.state.ready());
    f.raw("{", false); // Producer leaves stdin open; relay must still exit silently.
    auto oversized = event("PermissionRequest", "Bash");
    oversized.insert("tool_name", QString(257, QLatin1Char('x')));
    f.send(oversized);
    require(!f.state.ready());
    f.send(event("UserPromptSubmit", {}, {}, "after-loss"));
    require(!f.state.ready()); // A hook boundary is not authoritative recovery.
    f.observer.stop();
    f.raw("{}"); // Dead receiver cannot block or turn into an approval decision.
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == QStringLiteral("--claude-hook"))
        return lapis::claude::run_hook_relay(app.arguments().value(2), app.arguments().value(3));
    try {
        callback_shutdown();
        callback_destruction();
        identities_and_boundaries();
        idle_notice_retirement();
        exact_retirement();
        completed_tool_bounds();
        completed_tools_reset_at_prompt_epoch();
        connection_overflow_is_observable();
        session_replacement();
        privacy_bounds_and_transport();
        std::cout << "Claude live relay, identity, retirement, bounds, privacy and deadline cases "
                     "passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
