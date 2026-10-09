#include "claude_observer.hpp"
#include "hook_relay.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QObject>
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
    void send(const QJsonObject& source, const QString& relay_count = {}) {
        QLocalSocket connection;
        connection.connectToServer(socket);
        if (!connection.waitForConnected(1000)) {
            require(!state.connected());
            return;
        }
        auto copied = source;
        if (!relay_count.isNull())
            copied.insert("in_flight", relay_count);
        const auto payload = QJsonDocument(QJsonObject{{"nonce", nonce}, {"event", copied}})
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

void one_sided_stops_and_diagnostic_recovery() {
    const QString baseline =
        QStringLiteral("Claude hooks observe attention only; answer in the terminal");

    // Either list alone is valid. An omitted sibling is empty, not unknown,
    // so a genuinely finished turn does not incur the pause deadline.
    Fixture tasks_only;
    tasks_only.begin();
    require(tasks_only.observer.diagnostic() == baseline);
    auto tasks_stop = event("Stop");
    tasks_stop.insert("background_tasks", QJsonArray{});
    tasks_only.raw(QJsonDocument(tasks_stop).toJson(QJsonDocument::Compact));
    require(tasks_only.state.activity() == attention::Activity::turn_completed);
    require(tasks_only.observer.diagnostic() == baseline);

    Fixture crons_only;
    crons_only.begin();
    auto crons_stop = event("Stop");
    crons_stop.insert("session_crons", QJsonArray{QJsonObject{{"id", "wake-1"}}});
    crons_only.raw(QJsonDocument(crons_stop).toJson(QJsonDocument::Compact));
    require(crons_only.state.activity() == attention::Activity::working);
    require(crons_only.observer.diagnostic() == baseline);
    crons_stop.insert("session_crons", QJsonArray{});
    crons_only.raw(QJsonDocument(crons_stop).toJson(QJsonDocument::Compact));
    require(crons_only.state.activity() == attention::Activity::turn_completed);
    require(crons_only.observer.diagnostic() == baseline);

    // A present non-array remains schema drift, and a later valid count
    // retires only that background diagnostic. The standing terminal-only
    // instruction survives the clear.
    Fixture recovered;
    recovered.begin();
    QCoreApplication::processEvents();
    QString published_diagnostic;
    QObject publication_owner;
    QObject::connect(&recovered.observer, &lapis::claude::Observer::changed, &publication_owner,
                     [&] { published_diagnostic = recovered.observer.diagnostic(); });
    auto drifted = event("Stop");
    drifted.insert("background_tasks", QJsonObject{{"status", "running"}});
    recovered.raw(QJsonDocument(drifted).toJson(QJsonDocument::Compact));
    QCoreApplication::processEvents();
    require(recovered.state.activity() == attention::Activity::working);
    require(recovered.observer.diagnostic().contains(baseline) &&
            recovered.observer.diagnostic().contains(
                QLatin1String("Claude background-work schema is unavailable")));
    require(published_diagnostic == recovered.observer.diagnostic());
    auto finished = event("Stop");
    finished.insert("background_tasks", QJsonArray{});
    recovered.raw(QJsonDocument(finished).toJson(QJsonDocument::Compact));
    QCoreApplication::processEvents();
    require(recovered.state.activity() == attention::Activity::turn_completed);
    require(recovered.observer.diagnostic() == baseline);
    require(published_diagnostic == baseline);

    // An older relay's omitted lists replace the transient schema message;
    // they do not hide the standing terminal-only instruction.
    Fixture legacy;
    legacy.begin();
    legacy.send(event("Stop"), QStringLiteral("unknown"));
    legacy.raw(QJsonDocument(event("Stop")).toJson(QJsonDocument::Compact));
    require(legacy.state.activity() == attention::Activity::turn_completed);
    require(legacy.observer.diagnostic().contains(baseline) &&
            legacy.observer.diagnostic().contains(QLatin1String("observation is legacy")) &&
            !legacy.observer.diagnostic().contains(QLatin1String("schema is unavailable")));

    // The same scoped recovery applies to diagnostics from another observer
    // subsystem, rather than restoring one hard-coded baseline.
    Fixture interrupted;
    interrupted.begin();
    std::array<QLocalSocket, 8> clients;
    for (auto& client : clients) {
        client.connectToServer(interrupted.socket);
        require(client.waitForConnected(1000));
        QCoreApplication::processEvents();
    }
    QLocalSocket overflow;
    overflow.connectToServer(interrupted.socket);
    require(overflow.waitForConnected(1000));
    QCoreApplication::processEvents();
    const QString transport_diagnostic = interrupted.observer.diagnostic();
    require(transport_diagnostic.contains(
        QLatin1String("Claude hook connection limit exceeded (at least 1)")));

    overflow.disconnectFromServer();
    for (auto& client : clients)
        client.disconnectFromServer();
    QCoreApplication::processEvents();
    interrupted.raw(QJsonDocument(drifted).toJson(QJsonDocument::Compact));
    require(interrupted.state.activity() == attention::Activity::working);
    require(interrupted.observer.diagnostic().contains(transport_diagnostic) &&
            interrupted.observer.diagnostic().contains(
                QLatin1String("Claude background-work schema is unavailable")));

    interrupted.raw(QJsonDocument(finished).toJson(QJsonDocument::Compact));
    require(interrupted.state.activity() == attention::Activity::turn_completed);
    require(interrupted.observer.diagnostic() == transport_diagnostic);
}

// A Stop with background tasks or wakeups in flight is a paused turn: the
// agent's own work starts its next turn (a new prompt), and only a Stop with
// nothing in flight, or a pause no turn follows, finishes it.
void background_work_pauses_the_turn() {
    Fixture f;
    f.begin();
    // Claude's own Stop payload filter admits only running/pending work. A
    // retained completed task must not keep a completed turn waiting.
    auto stale = event("Stop");
    stale.insert("background_tasks",
                 QJsonArray{QJsonObject{{"id", "old"}, {"status", "completed"}}});
    stale.insert("session_crons", QJsonArray{});
    f.raw(QJsonDocument(stale).toJson(QJsonDocument::Compact));
    require(f.state.activity() == attention::Activity::turn_completed);

    Fixture paused;
    paused.begin();
    paused.send(event("PermissionRequest", "Bash"));
    require(paused.state.pending().size() == 1);
    auto stop = event("Stop");
    stop.insert("background_tasks", QJsonArray{QJsonObject{{"id", "b1"}, {"status", "running"}}});
    stop.insert("session_crons", QJsonArray{});
    paused.raw(QJsonDocument(stop).toJson(QJsonDocument::Compact));
    require(paused.state.pending().empty()); // A paused turn is not asking for attention.
    require(paused.state.activity() == attention::Activity::working);
    paused.send(event("UserPromptSubmit", {}, {}, "turn-2")); // the task's notification
    auto last = event("Stop", {}, {}, "turn-2");
    last.insert("background_tasks", QJsonArray{});
    last.insert("session_crons", QJsonArray{});
    paused.raw(QJsonDocument(last).toJson(QJsonDocument::Compact));
    require(paused.state.activity() == attention::Activity::turn_completed);

    // A service names its relay contract in the hook command. The command an
    // older service wrote has none, and the installed relay then sends only
    // identity fields: a derived field that service predates would read as a
    // malformed hook and end its observation for good.
    require(paused.command.endsWith(QLatin1Char(' ') + lapis::claude::relay_contract.toString()));
    Fixture older;
    older.command.chop(lapis::claude::relay_contract.size() + 1);
    older.begin();
    older.raw(QJsonDocument(stop).toJson(QJsonDocument::Compact));
    require(older.state.ready() && older.state.activity() == attention::Activity::turn_completed &&
            older.observer.diagnostic().contains(QLatin1String("legacy")));

    // A claimed count from the hook itself is ignored; the relay derives it.
    Fixture forged;
    forged.begin();
    auto claimed = event("Stop");
    claimed.insert("in_flight", "3");
    forged.raw(QJsonDocument(claimed).toJson(QJsonDocument::Compact));
    require(forged.state.activity() == attention::Activity::turn_completed);

    // The socket nonce authenticates a writer; the observer accepts the relay's
    // derived event field directly, retaining its existing wire contract.
    Fixture direct;
    direct.begin();
    direct.send(claimed);
    require(direct.state.ready() && direct.state.activity() == attention::Activity::working);
    Fixture malformed;
    malformed.begin();
    malformed.send(event("Stop"), QStringLiteral("not-a-count"));
    require(!malformed.state.ready() && malformed.observer.diagnostic().contains(QLatin1String(
                                            "Malformed Claude background-work count")));

    // One array replaced by another JSON kind is observable and conservative:
    // it cannot invent an immediate completion, and the pause remains bounded.
    Fixture drifted;
    drifted.begin();
    auto changed = event("Stop");
    changed.insert("background_tasks", QJsonObject{{"status", "running"}});
    changed.insert("session_crons", QJsonArray{});
    drifted.raw(QJsonDocument(changed).toJson(QJsonDocument::Compact));
    require(drifted.state.activity() == attention::Activity::working);
    require(drifted.observer.diagnostic().contains(
        QLatin1String("Claude background-work schema is unavailable")));

    Fixture unknown_status;
    unknown_status.begin();
    auto novel = event("Stop");
    novel.insert("background_tasks", QJsonArray{QJsonObject{{"status", "new-provider-state"}}});
    novel.insert("session_crons", QJsonArray{});
    unknown_status.raw(QJsonDocument(novel).toJson(QJsonDocument::Compact));
    require(unknown_status.state.activity() == attention::Activity::working &&
            unknown_status.observer.diagnostic().contains(QLatin1String("schema is unavailable")));

    // A wakeup counts too, and a pause nothing follows still finishes.
    Fixture waiting;
    waiting.begin();
    auto wakeup = event("Stop");
    wakeup.insert("background_tasks", QJsonArray{});
    wakeup.insert("session_crons", QJsonArray{QJsonObject{{"id", "c1"}}});
    waiting.raw(QJsonDocument(wakeup).toJson(QJsonDocument::Compact));
    require(waiting.state.activity() == attention::Activity::working);
    // Start the short deadline after the relay child has exited; sanitizer
    // startup cost is unrelated to the observer's duplicate handling.
    waiting.observer.setPausedTurnMsForTesting(150);
    QTimer duplicates;
    int working_duplicates = 0;
    QObject::connect(&duplicates, &QTimer::timeout, [&] {
        if (waiting.state.activity() == attention::Activity::working)
            ++working_duplicates;
        waiting.send(event("Stop"), QStringLiteral("1"));
    });
    duplicates.start(10);
    QEventLoop loop;
    QTimer::singleShot(400, &loop, &QEventLoop::quit);
    loop.exec();
    duplicates.stop();
    require(working_duplicates >= 2);
    require(waiting.state.activity() == attention::Activity::turn_completed);

    // If the shared attention state loses synchronization first, the deadline
    // still produces an explicit terminal observation rather than silence.
    Fixture unready;
    unready.begin();
    unready.raw(QJsonDocument(stop).toJson(QJsonDocument::Compact));
    unready.state.overflow();
    unready.observer.setPausedTurnMsForTesting(50);
    QEventLoop lost;
    QTimer::singleShot(200, &lost, &QEventLoop::quit);
    lost.exec();
    require(!unready.state.ready() && unready.observer.diagnostic().contains(QLatin1String(
                                          "Claude paused-turn deadline was unobservable")));
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
// Hooks relayed through another machine's terminal: no local listener or
// settings file, the same relay derivation (a claimed count is ignored and a
// running task pauses the turn), and the same identity rules.
void terminal_transport() {
    attention::State state{"session", "claude-code"};
    lapis::claude::Observer observer(state, lapis::claude::Observer::Transport::terminal);
    bool refused = false;
    try {
        static_cast<void>(observer.launchArguments({}, QStringLiteral("/bin/true")));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    require(refused);
    observer.receiveRelayed(event("Stop")); // nothing bound yet
    require(!state.ready());
    observer.receiveRelayed(event("SessionStart"));
    observer.receiveRelayed(event("UserPromptSubmit"));
    require(state.ready() && state.activity() == attention::Activity::working);
    auto stop = event("Stop");
    stop.insert("in_flight", "0");
    stop.insert("background_tasks", QJsonArray{QJsonObject{{"status", "running"}}});
    observer.receiveRelayed(stop);
    require(state.activity() == attention::Activity::working);
    observer.receiveRelayed(event("UserPromptSubmit", {}, {}, "turn-2"));
    auto last = event("Stop", {}, {}, "turn-2");
    last.insert("background_tasks", QJsonArray{});
    observer.receiveRelayed(last);
    require(state.activity() == attention::Activity::turn_completed);
    auto other = event("UserPromptSubmit", {}, {}, "turn-3");
    other.insert("session_id", "another-source");
    observer.receiveRelayed(other);
    require(state.activity() == attention::Activity::turn_completed);
    observer.stop();
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == QStringLiteral("--claude-hook"))
        return lapis::claude::run_hook_relay(app.arguments().value(2), app.arguments().value(3),
                                             app.arguments().value(4));
    try {
        callback_shutdown();
        callback_destruction();
        identities_and_boundaries();
        idle_notice_retirement();
        exact_retirement();
        completed_tool_bounds();
        completed_tools_reset_at_prompt_epoch();
        connection_overflow_is_observable();
        one_sided_stops_and_diagnostic_recovery();
        session_replacement();
        background_work_pauses_the_turn();
        privacy_bounds_and_transport();
        terminal_transport();
        std::cout << "Claude live relay, identity, retirement, bounds, privacy and deadline cases "
                     "passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
