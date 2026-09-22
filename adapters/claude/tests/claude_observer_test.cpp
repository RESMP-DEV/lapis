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
    lapis::claude::Observer observer{state};
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
        require(hooks.size() == 10);
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
    f.send(event("SessionStart"));
    require(!f.state.connected());
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
        identities_and_boundaries();
        exact_retirement();
        privacy_bounds_and_transport();
        std::cout << "Claude live relay, identity, retirement, privacy and deadline cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
