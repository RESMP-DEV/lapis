#include "agent_checkpoint.hpp"
#include "alerts.hpp"
#include "keymap.hpp"
#include "launch_spec.hpp"
#include "session_descriptor.hpp"
#include "terminals.hpp"
#include "transport/local_protocol.hpp"
#include "workspace.hpp"
#include "workspace_control.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(Q_OS_UNIX)
#include <sys/signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using lapis::desktop::Workspace;
using lapis::desktop::WorkspaceMode;
using lapis::desktop::WorkspaceOptions;
// These fixtures simulate an independently observed adapter record.
void writeObservedResume(const QString& endpoint, lapis::session::ResumeRecord record) {
    record.source = lapis::session::ResumeSource::observer;
    lapis::session::write_resume_record(endpoint, record);
}

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void explicitAgentIdentity() {
    QTemporaryDir directory;
    require(directory.isValid(), "private explicit-attach directory");
    using lapis::desktop::SessionPreview;
    using lapis::session::AgentMode;
    for (const auto mode : {AgentMode::terminal, AgentMode::codex, AgentMode::claude}) {
        WorkspaceOptions options;
        options.endpoint = QDir(QFileInfo(directory.path()).canonicalFilePath())
                               .filePath(QStringLiteral("absent.sock"));
        options.mode = lapis::session::wire::AttachMode::reconnect;
        options.launch = lapis::session::LaunchSpec{
            QStringLiteral("/usr/bin/true"), {}, directory.path(), {80, 24}, mode};
        Workspace workspace(WorkspaceMode::live, options);
        const auto* item = workspace.focusedSession();
        const QString expected = mode == AgentMode::codex    ? QStringLiteral("codex")
                                 : mode == AgentMode::claude ? QStringLiteral("claude")
                                                             : QString{};
        require(item && item->harnessId() == expected, "explicit attach retains harness identity");
        require(item->statusSource() == (mode == AgentMode::terminal
                                             ? SessionPreview::StatusSource::output
                                             : SessionPreview::StatusSource::observer),
                "explicit attach selects the correct status source");
        require(!workspace.createAgent(directory.path(), QStringLiteral("Blocked")),
                "an explicit attach cannot create an unpersisted managed agent");
        require(!workspace.restartAgent(QStringLiteral("shell")),
                "an explicit attach cannot restart an unpersisted managed agent");
        require(workspace.workspaceError() ==
                    QStringLiteral("Restart requires a persisted workspace."),
                "explicit restart rejection names the persistence requirement");
        require(workspace.sessions().size() == 1, "rejected creation leaves the attach intact");
    }
}
void projectPaths() {
    Workspace workspace(WorkspaceMode::preview);
    const auto home = QDir::homePath();
    require(workspace.homeDirectory() == home, "home comes from the platform");
    require(workspace.displayPath(home) == QStringLiteral("~"), "home label");
    require(workspace.displayPath(home + QStringLiteral("/dev/lapis/")) ==
                QStringLiteral("~/dev/lapis"),
            "home-relative project label");
    require(workspace.displayPath(home + QStringLiteral("-other/project")) ==
                home + QStringLiteral("-other/project"),
            "home prefix must respect directory boundary");
}
void categoriesAndIdentity() {
    Workspace workspace(WorkspaceMode::preview);
    auto* original = workspace.session(QStringLiteral("renderer"));
    require(!workspace.restartAgent(QStringLiteral("renderer")),
            "a preview cannot restart its sample agents");
    require(workspace.workspaceError() == QStringLiteral("Restart requires a persisted workspace."),
            "preview restart rejection names the persistence requirement");
    int list_changes = 0;
    QObject::connect(&workspace, &Workspace::sessionsChanged, &workspace, [&] { ++list_changes; });
    require(workspace.selectSession(QStringLiteral("renderer")), "select existing agent");
    require(list_changes == 0, "tab selection does not rebuild the tab model");
    require(!workspace.renameCategory(QStringLiteral("General"), QStringLiteral("general")),
            "reversed category arguments cannot resolve identity");
    require(!workspace.renameSession(QStringLiteral("Render work"), QStringLiteral("renderer")),
            "reversed session arguments cannot resolve identity");
    require(workspace.addCategory(QStringLiteral("Research")), "add category");
    const auto research = workspace.activeCategoryId();
    require(workspace.focusedSession() == nullptr && workspace.categorySessions().isEmpty(),
            "empty category must have no focused terminal");
    require(workspace.moveSession(QStringLiteral("renderer"), research), "move existing agent");
    require(workspace.focusedSession() == original, "move must preserve object identity");
    require(workspace.selectCategory(QStringLiteral("general")), "select original category");
    require(workspace.selectSession(QStringLiteral("agent")), "select other agent");
    require(workspace.selectCategory(research), "return to research");
    require(workspace.focusedSession() == original, "category restores selected agent");
    workspace.nextSession(-1);
    require(workspace.focusedSession() == original, "tab navigation cannot leave category");
    require(!workspace.removeCategory(research), "occupied category cannot be removed");
    require(workspace.renameSession(QStringLiteral("renderer"), QStringLiteral("Render work")),
            "rename agent");
    require(original->title() == QStringLiteral("Render work"),
            "title changed without replacement");
    require(workspace.moveSession(QStringLiteral("renderer"), QStringLiteral("general")),
            "return agent to general");
    require(workspace.removeCategory(research), "remove empty category");
    require(workspace.selectSession(QStringLiteral("renderer")), "select moved agent");
    require(workspace.moveSessionBy(QStringLiteral("renderer"), -100), "reorder agent");
    require(workspace.categorySessions().front().value<lapis::desktop::SessionPreview*>() ==
                original,
            "reorder within category");
    require(workspace.focusedSession() == original, "reorder preserves selected identity");
    const auto selected = workspace.focusedSession();
    require(workspace.replayAttention(QStringLiteral("arrival")), "request fixture");
    require(workspace.focusedSession() == selected, "attention cannot steal focus");
    require(
        workspace.categories().front().toMap().value(QStringLiteral("attentionCount")).toInt() == 1,
        "category aggregates requests");
    require(!workspace.addCategory(QString(81, QLatin1Char('a'))), "bounded names");
}
void categoryCountsChangeOnlyWhenNeeded() {
    Workspace workspace(WorkspaceMode::preview);
    QObject receiver;
    int notifications = 0;
    QObject::connect(&workspace, &Workspace::categoriesChanged, &receiver,
                     [&] { ++notifications; });
    auto* agent = workspace.session(QStringLiteral("agent"));
    if (!agent)
        throw std::runtime_error("missing agent fixture");
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    state.activity = lapis::session::attention::Activity::working;
    agent->applyAttention(state);
    require(notifications == 0, "activity alone does not rebuild category data");
    state.requests.emplace_back();
    agent->applyAttention(state);
    require(notifications == 1, "new pending count updates categories once");
    state.activity = lapis::session::attention::Activity::turn_completed;
    agent->applyAttention(state);
    require(notifications == 1, "same pending count preserves category navigation");
    state.requests.clear();
    agent->applyAttention(state);
    require(notifications == 2, "resolution updates category counts");
}

void persistence() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    WorkspaceOptions options;
    options.storagePath = QDir(directory.path()).filePath(QStringLiteral("workspace.json"));
    QString category;
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().isEmpty(), "new registry ready");
        require(workspace.sessions().isEmpty() && !workspace.focusedSession(),
                "normal workspace never launches shell or fixtures");
        require(
            !workspace.createAgent(directory.filePath(QStringLiteral("missing")), directory.path()),
            "invalid project directory cannot launch an agent");
        require(workspace.workspaceError().contains(QStringLiteral("project directory")),
                "invalid directory diagnostic does not depend on Codex installation");
        require(workspace.sessions().isEmpty(), "invalid launch cannot add an agent tab");
        workspace.clearError();
        require(workspace.addCategory(QStringLiteral("Research")), "persist category");
        category = workspace.activeCategoryId();
        require(workspace.renameCategory(QStringLiteral("general"), QStringLiteral("Build")),
                "persist rename");
        QFile lock(options.storagePath + QStringLiteral(".lock"));
        require(lock.open(QIODevice::ReadWrite), "open this fixture's owned lock");
        require(lock.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-3600),
                                 QFileDevice::FileModificationTime),
                "age the live fixture lock");
        lock.close();
        Workspace second(WorkspaceMode::live, options);
        require(!second.workspaceError().isEmpty(), "even an old live writer must be excluded");
        const auto original_categories = second.categories();
        require(!second.addCategory(QStringLiteral("Forbidden")), "locked writer rejects add");
        require(!second.renameCategory(QStringLiteral("general"), QStringLiteral("Forbidden")),
                "locked writer rejects rename");
        require(!second.selectCategory(QStringLiteral("general")),
                "locked writer rejects selection");
        require(second.categories() == original_categories &&
                    second.activeCategoryId() == QStringLiteral("general"),
                "locked writer cannot change in-memory categories");
    }
    {
        Workspace restored(WorkspaceMode::live, options);
        require(restored.workspaceError().isEmpty(), "registry restores");
        require(restored.categories().size() == 2, "restore category count");
        require(restored.activeCategoryId() == category, "restore active category");
        require(restored.categories().front().toMap().value(QStringLiteral("name")).toString() ==
                    QStringLiteral("Build"),
                "restore category name");
    }
    QFile file(options.storagePath);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open damaged registry");
    require(file.write("broken") == 6, "write damaged registry");
    file.close();
    {
        Workspace damaged(WorkspaceMode::live, options);
        require(!damaged.workspaceError().isEmpty(), "malformed registry diagnostic");
        require(!damaged.addCategory(QStringLiteral("Do not overwrite")),
                "refuse overwrite malformed registry");
    }
    require(file.open(QIODevice::ReadOnly), "read preserved registry");
    require(file.readAll() == QByteArrayLiteral("broken"),
            "preserve malformed registry for repair");
}
void restoreAgentIdentity() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary registry directory");
    const QString first = QStringLiteral("ef715fac-a03a-45d4-8466-b0f2740c6b7b");
    const QString second = QStringLiteral("e93750ef-ab0d-418a-b53f-25a601827e31");
    QJsonArray agents;
    for (const auto& id : {first, second})
        agents.append(QJsonObject{{"id", id},
                                  {"title", id},
                                  {"category", "general"},
                                  {"endpoint", QDir(QFileInfo(directory.path()).canonicalFilePath())
                                                   .filePath(id + QStringLiteral(".sock"))},
                                  {"program", "/usr/bin/true"},
                                  {"harness", id == second ? "claude" : "codex"},
                                  {"directory", directory.path()}});
    WorkspaceOptions options;
    options.storagePath = QDir(directory.path()).filePath(QStringLiteral("workspace.json"));
    QFile file(options.storagePath);
    require(file.open(QIODevice::WriteOnly), "create registry fixture");
    const auto data =
        QJsonDocument(QJsonObject{{"version", 1},
                                  {"activeCategory", "general"},
                                  {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                        {"name", "General"},
                                                                        {"selected", second}}}},
                                  {"agents", agents}})
            .toJson();
    require(file.write(data) == data.size(), "write registry fixture");
    file.close();
    {
        Workspace restored(WorkspaceMode::live, options);
        require(restored.workspaceError().isEmpty(), "load agent registry");
        require(restored.sessions().size() == 2, "restore both agents");
        require(restored.focusedSession()->sessionId() == second,
                "restore selected agent identity");
        require(restored.session(second)->harnessId() == QStringLiteral("claude") &&
                    restored.session(second)->agentName() == QStringLiteral("Claude"),
                "restore harness identity");
        require(restored.session(first)->live(), "restore reconnect object");
        require(restored.session(first)->harnessId() == QStringLiteral("codex"),
                "legacy registry remains Codex");
        require(!restored.removeSession(first), "disconnected agent may still be running");
        require(restored.addCategory(QStringLiteral("Research")), "create destination category");
        require(restored.moveSession(first, restored.activeCategoryId()), "persist category move");
        require(restored.focusedSession()->sessionId() == first,
                "select moved agent in empty category");
    }
    {
        Workspace restored(WorkspaceMode::live, options);
        require(restored.workspaceError().isEmpty(), "reload moved agents");
        require(restored.categorySessions().size() == 1, "persist category membership");
        require(restored.focusedSession()->sessionId() == first, "persist per-category selection");
        require(restored.selectCategory(QStringLiteral("general")), "return original category");
        require(restored.focusedSession()->sessionId() == second,
                "preserve original category selection");
        require(restored.session(second)->harnessId() == QStringLiteral("claude"),
                "persist harness after registry mutation");
    }
}
QByteArray readRegistry(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read registry bytes");
    return file.readAll();
}
void failedWritesPreserveState() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary write-failure directory");
    const auto original = directory.filePath(QStringLiteral("registry"));
    const auto moved = directory.filePath(QStringLiteral("held"));
    require(QDir().mkdir(original, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "private registry parent");
    const auto canonical = QFileInfo(original).canonicalFilePath();
    const QString first = QStringLiteral("ef715fac-a03a-45d4-8466-b0f2740c6b7b");
    const QString second = QStringLiteral("e93750ef-ab0d-418a-b53f-25a601827e31");
    QJsonArray agents;
    for (const auto& id : {first, second})
        agents.append(
            QJsonObject{{"id", id},
                        {"title", id},
                        {"category", "general"},
                        {"endpoint", QDir(canonical).filePath(id + QStringLiteral(".sock"))},
                        {"program", "/usr/bin/true"},
                        {"directory", canonical}});
    WorkspaceOptions options;
    options.storagePath = QDir(original).filePath(QStringLiteral("workspace.json"));
    QFile file(options.storagePath);
    require(file.open(QIODevice::WriteOnly), "create write-failure fixture");
    auto bytes =
        QJsonDocument(
            QJsonObject{
                {"version", 1},
                {"activeCategory", "general"},
                {"categories",
                 QJsonArray{
                     QJsonObject{{"id", "general"}, {"name", "General"}, {"selected", second}},
                     QJsonObject{{"id", "empty"}, {"name", "Empty"}, {"selected", ""}}}},
                {"agents", agents}})
            .toJson();
    require(file.write(bytes) == bytes.size(), "write complete fixture");
    file.close();
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().isEmpty(), "load mutation failure fixture");
        auto* first_object = workspace.session(first);
        auto* second_object = workspace.session(second);
        if (first_object == nullptr || second_object == nullptr)
            throw std::runtime_error("retain both session objects");
        first_object->setConnection(QStringLiteral("ended"), false);
        const auto categories = workspace.categories();
        const auto sessions = workspace.sessions();
        const auto category_sessions = workspace.categorySessions();
        int notifications = 0;
        const auto count = [&] { ++notifications; };
        QObject::connect(&workspace, &Workspace::categoriesChanged, &workspace, count);
        QObject::connect(&workspace, &Workspace::categoryChanged, &workspace, count);
        QObject::connect(&workspace, &Workspace::sessionsChanged, &workspace, count);
        QObject::connect(&workspace, &Workspace::focusChanged, &workspace, count);
        QObject::connect(first_object, &lapis::desktop::SessionPreview::identityChanged, &workspace,
                         count);
        QObject::connect(second_object, &lapis::desktop::SessionPreview::identityChanged,
                         &workspace, count);
        require(QDir().rename(original, moved),
                "move registry parent to force real QSaveFile failure");
        const auto unchanged = [&] {
            require(workspace.categories() == categories && workspace.sessions() == sessions &&
                        workspace.categorySessions() == category_sessions,
                    "failed write preserves category metadata and object order");
            require(workspace.activeCategoryId() == QStringLiteral("general") &&
                        workspace.focusedSession() == second_object,
                    "failed write preserves active category and selection");
            require(workspace.session(first) == first_object &&
                        workspace.session(second) == second_object && first_object->live() &&
                        second_object->live(),
                    "failed removal cannot destroy or disconnect retained objects");
            require(first_object->title() == first && second_object->title() == second,
                    "failed rename cannot change identity");
            require(notifications == 0,
                    "failed mutations emit no model, focus or identity notifications");
            require(readRegistry(QDir(moved).filePath(QStringLiteral("workspace.json"))) == bytes,
                    "failed write leaves previous registry bytes intact");
        };
        require(!workspace.addCategory(QStringLiteral("Uncommitted")),
                "add rolls back on failed save");
        unchanged();
        require(!workspace.renameCategory(QStringLiteral("general"), QStringLiteral("Uncommitted")),
                "category rename rolls back");
        unchanged();
        require(!workspace.removeCategory(QStringLiteral("empty")), "category removal rolls back");
        unchanged();
        require(!workspace.selectCategory(QStringLiteral("empty")),
                "category selection rolls back");
        unchanged();
        require(!workspace.selectSession(first), "tab selection rolls back");
        unchanged();
        require(!workspace.renameSession(first, QStringLiteral("Uncommitted")),
                "title rename rolls back");
        unchanged();
        require(!workspace.moveSession(first, QStringLiteral("empty")), "category move rolls back");
        unchanged();
        require(!workspace.moveSessionBy(first, 1), "tab reorder rolls back");
        unchanged();
        require(!workspace.followConversationTitle(second, second),
                "conversation-title provenance rolls back on failed save");
        unchanged();
        require(!workspace.removeSession(first), "ended tab removal rolls back");
        unchanged();
        require(QDir().rename(moved, original), "restore registry parent");
        require(workspace.renameSession(first, QStringLiteral("Committed")),
                "write recovers when directory returns");
        require(first_object->title() == QStringLiteral("Committed") && notifications == 1,
                "successful rename publishes exactly one identity change");
        const auto recovered = QJsonDocument::fromJson(readRegistry(options.storagePath))
                                   .object()
                                   .value(QStringLiteral("agents"))
                                   .toArray();
        require(std::none_of(recovered.begin(), recovered.end(),
                             [&](const QJsonValue& entry) {
                                 return entry[QStringLiteral("id")] == second &&
                                        entry[QStringLiteral("autoTitle")].toBool();
                             }),
                "a later successful mutation does not persist the failed auto-title flag");
        const QPointer<lapis::desktop::SessionPreview> removed(first_object);
        require(workspace.removeSession(first), "remove ended agent after save recovers");
        require(removed && !workspace.session(first), "removed tab survives the QML call stack");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(removed.isNull(), "removed tab is eventually destroyed");
    }
}
void malformedAgentRegistry() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary malformed-agent directory");
    WorkspaceOptions options;
    options.storagePath = directory.filePath(QStringLiteral("workspace.json"));
    const auto bytes =
        QJsonDocument(
            QJsonObject{{"version", 1},
                        {"activeCategory", "foreign"},
                        {"categories",
                         QJsonArray{QJsonObject{{"id", "foreign"}, {"name", "Must not survive"}}}},
                        {"agents", QJsonArray{QJsonObject{{"id", "invalid"}}}}})
            .toJson();
    QFile file(options.storagePath);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "write malformed agent fixture");
    file.close();
    Workspace workspace(WorkspaceMode::live, options);
    require(!workspace.workspaceError().isEmpty(), "reject malformed agent");
    require(workspace.categories().size() == 1 &&
                workspace.categories().front().toMap().value(QStringLiteral("id")).toString() ==
                    QStringLiteral("general") &&
                workspace.activeCategoryId() == QStringLiteral("general"),
            "partial parsing cannot pollute clean General category");
    require(workspace.sessions().isEmpty() && !workspace.focusedSession(),
            "malformed restore creates no session");
    require(!workspace.addCategory(QStringLiteral("Blocked")),
            "malformed registry blocks mutations");
    require(workspace.categories().size() == 1 && readRegistry(options.storagePath) == bytes,
            "malformed registry leaves memory and bytes unchanged");
}
void placeholderMatchesTerminalDefaults() {
    const auto engine = lapis::session::Terminal({2, 2}).snapshot();
    lapis::desktop::SessionPreview live(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    require(live.snapshot().background_rgb == engine.background_rgb &&
                live.snapshot().foreground_rgb == engine.foreground_rgb,
            "a live agent's placeholder uses the terminal defaults, not the sample palette");
    lapis::desktop::SessionPreview fixture(QStringLiteral("Sample"), {}, {}, QColor{}, "sample");
    require(fixture.snapshot().background_rgb == 0x0d131dU, "fixtures keep the sample palette");
}
void discardOutsideActiveCategorySelectsNeighbor() {
    Workspace workspace(WorkspaceMode::preview);
    require(workspace.addCategory(QStringLiteral("Research")), "destination category");
    const auto research = workspace.activeCategoryId();
    require(workspace.moveSession(QStringLiteral("renderer"), research), "move first neighbor");
    require(workspace.moveSession(QStringLiteral("agent"), research), "move second neighbor");
    require(workspace.moveSession(QStringLiteral("service"), research), "move right neighbor");
    require(workspace.selectSession(QStringLiteral("agent")), "select the future closed tab");
    require(workspace.selectCategory(QStringLiteral("general")), "show a different category");
    auto* agent = workspace.session(QStringLiteral("agent"));
    require(agent != nullptr, "retain the closed preview fixture");
    agent->setConnection(QStringLiteral("ended"), false);
    require(workspace.removeSession(QStringLiteral("agent")), "remove an inactive-category tab");
    require(workspace.selectCategory(research) &&
                workspace.focusedSession() == workspace.session(QStringLiteral("service")),
            "closing an unseen category remembers its own neighbor");
}
void truthfulStatus() {
    lapis::desktop::SessionPreview item(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    lapis::session::wire::AttentionSnapshot attention;
    attention.available = true;
    attention.connected = true;
    attention.ready = true;
    attention.activity = lapis::session::attention::Activity::working;
    item.applyAttention(attention);
    require(item.statusKind() == QStringLiteral("working"), "working from source");
    attention.activity = lapis::session::attention::Activity::turn_completed;
    item.applyAttention(attention);
    require(item.statusLabel() == QStringLiteral("Turn finished"),
            "turn completion is not task or process completion");
    lapis::session::wire::AttentionItem request;
    request.pending.request.id = std::string("request");
    request.pending.request.reason = "Choose a value";
    attention.requests.push_back(request);
    item.applyAttention(attention);
    require(item.statusKind() == QStringLiteral("waiting"), "pending request has its own state");
    item.invalidateAttention();
    require(item.attentionCount() == 1, "stale requests remain visible for reconciliation");
    lapis::session::wire::AttentionSnapshot fresh;
    fresh.available = fresh.connected = true;
    // The producer owns the lifecycle phase; this diagnostic is display-only.
    fresh.diagnostic = QStringLiteral("Waiting for Codex thread history");
    fresh.observation_phase = lapis::session::attention::ObservationPhase::awaiting_first_prompt;
    lapis::desktop::SessionPreview first(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    first.applyAttention(fresh);
    require(first.statusLabel() == QStringLiteral("No prompt yet"),
            "a new Codex session without a thread is not a pending status");
    lapis::session::wire::AttentionSnapshot idle;
    idle.available = idle.connected = idle.ready = true;
    idle.activity = lapis::session::attention::Activity::turn_completed;
    lapis::session::wire::AttentionItem notice;
    notice.pending.request.id = std::string("idle:prompt");
    notice.pending.request.reason = "idle";
    idle.requests.push_back(notice);
    lapis::desktop::SessionPreview claude(QStringLiteral("Claude"), {}, {}, QColor{}, "");
    claude.applyAttention(idle);
    require(claude.attentionCount() == 0 && claude.statusKind() == QStringLiteral("finished"),
            "an idle notice after a finished turn is not a pending request");
    fresh.diagnostic = QStringLiteral("Vendor text changed during retry");
    fresh.observation_phase = lapis::session::attention::ObservationPhase::reconciling;
    first.applyAttention(fresh);
    require(first.statusLabel() == QStringLiteral("No prompt yet"),
            "a typed retry does not end a producer-established wait");
    fresh.observation_phase = lapis::session::attention::ObservationPhase::unknown;
    first.applyAttention(fresh);
    require(first.statusLabel() == QStringLiteral("Status pending"),
            "an unknown phase is not relabeled from vendor text");
    lapis::desktop::SessionPreview legacy(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    legacy.applyAttention(fresh);
    require(legacy.statusLabel() == QStringLiteral("Status pending"),
            "legacy snapshots retain conservative unknown semantics");
    lapis::desktop::SessionPreview reconnecting(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    fresh.observation_phase = lapis::session::attention::ObservationPhase::reconciling;
    reconnecting.applyAttention(fresh);
    require(reconnecting.statusLabel() == QStringLiteral("Status pending"),
            "reconciliation alone still reads as pending");
    require(item.statusKind() == QStringLiteral("unknown"), "stale activity must not look current");
}

bool waitFor(const std::function<bool()>& condition, int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return condition();
}

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

class ScopedCodexHome {
  public:
    explicit ScopedCodexHome(const QByteArray& value)
        : had_previous_(qEnvironmentVariableIsSet("CODEX_HOME")), previous_(qgetenv("CODEX_HOME")) {
        qputenv("CODEX_HOME", value);
    }
    ~ScopedCodexHome() {
        if (had_previous_)
            qputenv("CODEX_HOME", previous_);
        else
            qunsetenv("CODEX_HOME");
    }
    ScopedCodexHome(const ScopedCodexHome&) = delete;
    ScopedCodexHome& operator=(const ScopedCodexHome&) = delete;

  private:
    bool had_previous_;
    QByteArray previous_;
};

QJsonObject agentRecord(const QString& directory, const QString& id, const char* category) {
    return QJsonObject{{"id", id},
                       {"title", id},
                       {"category", category},
                       {"endpoint", QDir(directory).filePath(id + QStringLiteral(".sock"))},
                       {"program", "/usr/bin/true"},
                       {"directory", directory}};
}

class ScopedClaudeHome {
  public:
    explicit ScopedClaudeHome(const QByteArray& value)
        : had_previous_(qEnvironmentVariableIsSet("CLAUDE_CONFIG_DIR")),
          previous_(qgetenv("CLAUDE_CONFIG_DIR")) {
        qputenv("CLAUDE_CONFIG_DIR", value);
    }
    ~ScopedClaudeHome() {
        if (had_previous_)
            qputenv("CLAUDE_CONFIG_DIR", previous_);
        else
            qunsetenv("CLAUDE_CONFIG_DIR");
    }
    ScopedClaudeHome(const ScopedClaudeHome&) = delete;
    ScopedClaudeHome& operator=(const ScopedClaudeHome&) = delete;

  private:
    bool had_previous_;
    QByteArray previous_;
};

void writeRegistry(const QString& path, const QJsonObject& root) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open registry fixture");
    const auto bytes = QJsonDocument(root).toJson();
    require(file.write(bytes) == bytes.size(), "write registry fixture");
}

void writeExecutable(const QString& path, const QByteArray& body) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write a fake executable");
    require(file.write(body) == body.size(), "write the fake executable body");
    file.close();
    require(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make the fake executable executable");
}

// An agent that finishes a turn, or starts needing a response, while another
// is selected is marked until it is selected; the selected agent never is.
void unseenFollowsTurnsAndSelection() {
    Workspace workspace(WorkspaceMode::preview);
    require(workspace.selectSession(QStringLiteral("renderer")), "select renderer");
    int pings = 0;
    QObject::connect(&workspace, &Workspace::turnFinished, [&pings] { ++pings; });
    const int waiting_before = workspace.attentionAgents();
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    const auto turn = [&](const char* id) {
        auto* item = workspace.session(QString::fromLatin1(id));
        state.activity = lapis::session::attention::Activity::working;
        item->applyAttention(state);
        state.activity = lapis::session::attention::Activity::turn_completed;
        item->applyAttention(state);
        return item;
    };
    auto* agent = turn("agent");
    require(agent->unseen(), "an unselected agent's finished turn is marked");
    require(!turn("renderer")->unseen(), "the selected agent is already in view");
    const auto count = [&](const char* id) {
        for (const auto& value : workspace.categories())
            if (value.toMap().value(QStringLiteral("id")).toString() == QLatin1String(id))
                return value.toMap().value(QStringLiteral("unseenCount")).toInt();
        return -1;
    };
    require(count("general") == 1, "categories count unseen agents");
    require(workspace.attentionAgents() > waiting_before, "the Dock badge counts unseen agents");
    require(workspace.selectSession(QStringLiteral("agent")) && !agent->unseen(),
            "selecting an agent clears its mark");
    require(workspace.selectSession(QStringLiteral("renderer")), "select renderer again");
    auto* checks = workspace.session(QStringLiteral("checks"));
    state.activity = lapis::session::attention::Activity::idle;
    checks->applyAttention(state);
    require(!checks->unseen(), "an agent that was not working has nothing new");
    const int pings_before = pings;
    state.requests.emplace_back();
    checks->applyAttention(state);
    require(checks->unseen(), "a new request marks its agent");
    require(pings == pings_before + 1, "a new request pings once, as a finished turn does");
    require(count("general") == 1, "one unseen agent after the earlier one was selected");
    // Jumping to the waiting agent clears it as seen.
    require(workspace.nextAttention() && workspace.focusedSession() == checks,
            "the next waiting agent is selected");
    require(!checks->unseen(), "jumping to an agent is looking at it");
    require(workspace.attentionAgents() == waiting_before,
            "a request already looked at no longer counts, though it is still pending");
    require(!workspace.nextAttention(), "nor does it draw the jump again");
}

// An agent asked for without a mode starts in the forms' default, or the
// nearest mode its CLI offers, unless its configured arguments choose one.
void modelessAgentsGetTheDefaultMode() {
    using lapis::desktop::AgentRequest;
    using lapis::desktop::launch_mode;
    const auto asked = [](const char* harness, const char* mode = "") {
        AgentRequest request;
        request.harness = QString::fromLatin1(harness);
        request.mode = QString::fromLatin1(mode);
        return request;
    };
    require(launch_mode(asked("claude", "edits"), {}, QStringLiteral("full")) ==
                QLatin1String("edits"),
            "a mode asked for is kept");
    require(launch_mode(asked("claude"), {}, {}) == QLatin1String("full"),
            "no mode and no default: Full access");
    require(launch_mode(asked("claude"), {}, QStringLiteral("auto")) == QLatin1String("auto"),
            "no mode: newAgent.mode");
    require(launch_mode(asked("omp"), {}, QStringLiteral("auto")) == QLatin1String("edits"),
            "a CLI without the default takes the nearest, less access first");
    require(launch_mode(asked("opencode"), {}, QStringLiteral("edits")).isEmpty(),
            "an unavailable conservative mode must not grant full access");
    require(launch_mode(asked("kimi"), {}, QStringLiteral("edits")).isEmpty(),
            "modeless requests cannot escalate to a higher supported mode");
    require(launch_mode(asked("claude"), {}, QStringLiteral("unknown")).isEmpty(),
            "unknown preferences do not become full access");
    require(launch_mode(asked("claude"),
                        {QStringLiteral("--"), QStringLiteral("--permission-mode")},
                        QStringLiteral("edits")) == QLatin1String("edits"),
            "a literal prompt token must not suppress the configured permission mode");
    require(launch_mode(asked("codex"), {QStringLiteral("-s"), QStringLiteral("read-only")},
                        QStringLiteral("edits"))
                .isEmpty(),
            "a configured read-only sandbox suppresses generated mode/sandbox flags");
    require(
        launch_mode(asked("codex"), {QStringLiteral("--sandbox=read-only")}, QStringLiteral("full"))
            .isEmpty(),
        "the long sandbox option cannot be overwritten by a default full-access mode");
    require(launch_mode(asked("codex"),
                        {QStringLiteral("--label"), QStringLiteral("workspace-write")},
                        QStringLiteral("edits")) == QLatin1String("edits"),
            "an ordinary value is not mistaken for an emitted option");
    require(launch_mode(asked("codex"),
                        {QStringLiteral("--model"), QStringLiteral("-a"), QStringLiteral("-s"),
                         QStringLiteral("read-only")},
                        QStringLiteral("edits"))
                .isEmpty(),
            "a value that spells a mode option still cannot suppress mode flags");
    require(launch_mode(asked("codex"), {QStringLiteral("--label"), QStringLiteral("--sandbox")},
                        QStringLiteral("edits")) == QLatin1String("edits"),
            "an option spelled as another option's value is consumed, not scanned");
    for (const auto* configured :
         {"--permission-mode=acceptEdits", "--permission-mode", "--dangerously-skip-permissions"})
        require(launch_mode(asked("claude"), {QString::fromLatin1(configured)}, {}).isEmpty(),
                "configured arguments that choose a mode win, however spelled");
    require(launch_mode(asked("codex"), {QStringLiteral("--ask-for-approval=never")}, {}).isEmpty(),
            "Codex's own approval option wins too");
    require(launch_mode(asked("claude"), {QStringLiteral("--verbose")}, {}) ==
                QLatin1String("full"),
            "other configured arguments do not");
    require(launch_mode(asked("shell"), {}, {}).isEmpty(), "a CLI without modes gets none");
}

// Command-L goes to the agent that most recently began to need you, then the
// one before it.
void latestAttentionGoesToTheNewest() {
    Workspace workspace(WorkspaceMode::preview);
    require(workspace.selectSession(QStringLiteral("renderer")), "select renderer");
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    const auto finish = [&](const char* id) {
        auto* item = workspace.session(QString::fromLatin1(id));
        state.activity = lapis::session::attention::Activity::working;
        item->applyAttention(state);
        state.activity = lapis::session::attention::Activity::turn_completed;
        item->applyAttention(state);
        require(item->unseen(), "a finished turn out of view is marked");
        QThread::msleep(5); // a later need has a later time
        return item;
    };
    auto* first = finish("agent");
    auto* second = finish("checks");
    require(workspace.latestAttention() && workspace.focusedSession() == second,
            "the newest need is selected first");
    require(workspace.latestAttention() && workspace.focusedSession() == first,
            "then the one before it");
    require(!workspace.latestAttention(), "and nothing once all were looked at");
}

// Tab's next agent: a guess not yet seen first, then a turn finished unseen,
// then a guess already seen, so Tab cannot bounce between guesses while
// another agent waits; an agent at work is not waiting even with a guess.
void tabGoesToTheReadyThenTheOldest() {
    Workspace workspace(WorkspaceMode::preview);
    require(workspace.selectSession(QStringLiteral("renderer")), "select renderer");
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    const auto finish = [&](const char* id) {
        auto* item = workspace.session(QString::fromLatin1(id));
        state.activity = lapis::session::attention::Activity::working;
        item->applyAttention(state);
        state.activity = lapis::session::attention::Activity::turn_completed;
        item->applyAttention(state);
        QThread::msleep(5);
        return item;
    };
    auto* older = finish("agent");
    auto* newer = finish("checks");
    require(workspace.nextPriorityAttention({{newer->sessionId(), false}}) &&
                workspace.focusedSession() == newer,
            "the agent with a guess not yet seen goes first");
    const QVariantMap seen{{newer->sessionId(), true}};
    require(workspace.nextPriorityAttention(seen) && workspace.focusedSession() == older,
            "then the one whose turn finished unseen");
    require(workspace.nextPriorityAttention(seen) && workspace.focusedSession() == newer,
            "a guess already seen and not used keeps its agent in the rotation, last");
    auto* third = finish("renderer");
    require(workspace.nextPriorityAttention(seen) && workspace.focusedSession() == third,
            "an unseen turn goes before a guess already seen");
    require(workspace.selectSession(older->sessionId()), "back to the older one");
    state.activity = lapis::session::attention::Activity::working;
    newer->applyAttention(state);
    require(!workspace.nextPriorityAttention(seen),
            "nothing is waiting once the one with a guess is at work");
}

// Claude agents run under the service's Claude Code adapter and read their
// status from its observer. Records saved before the adapter keep terminal
// mode, the launch their running service was created with.
void claudeAgentsUseServiceAdapter() {
    QTemporaryDir directory;
    require(directory.isValid(), "adapter directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString managed = uuid();
    const QString legacy = uuid();
    const QString codex = uuid();
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    auto managed_record = agentRecord(canonical, managed, "general");
    managed_record.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    managed_record.insert(QStringLiteral("mode"), QStringLiteral("claude"));
    auto legacy_record = agentRecord(canonical, legacy, "general");
    legacy_record.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    writeRegistry(
        options.storagePath,
        QJsonObject{
            {"version", 2},
            {"activeCategory", "general"},
            {"categories",
             QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}, {"selected", codex}}}},
            {"agents",
             QJsonArray{managed_record, legacy_record, agentRecord(canonical, codex, "general")}}});
    const auto source = [](Workspace& workspace, const QString& id) {
        return workspace.session(id)->statusSource();
    };
    using Source = lapis::desktop::SessionPreview::StatusSource;
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().isEmpty(), "load adapter registry");
        require(source(workspace, managed) == Source::observer,
                "Claude agents read status from the service adapter");
        require(source(workspace, legacy) == Source::output,
                "pre-adapter Claude agents keep terminal mode and the output estimate");
        require(source(workspace, codex) == Source::observer, "Codex keeps its observer");
        require(workspace.addCategory(QStringLiteral("Research")), "save the registry");
    }
    const auto saved = QJsonDocument::fromJson(readRegistry(options.storagePath))
                           .object()
                           .value(QStringLiteral("agents"))
                           .toArray();
    const auto mode = [&](const QString& id) {
        for (const auto& value : saved)
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject().value(QStringLiteral("mode")).toString();
        return QStringLiteral("missing");
    };
    require(mode(managed) == QStringLiteral("claude") && mode(legacy).isEmpty() &&
                mode(codex).isEmpty(),
            "the registry records which Claude agents use the adapter");
    Workspace reopened(WorkspaceMode::live, options);
    require(source(reopened, managed) == Source::observer &&
                source(reopened, legacy) == Source::output,
            "adapter mode survives a save and reopen");
}

// An agent's literal arguments are part of its launch fingerprint, so the
// registry keeps them; a malformed list is rejected rather than guessed.
void agentArgumentsPersist() {
    QTemporaryDir directory;
    require(directory.isValid(), "arguments directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString id = uuid();
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    auto record = agentRecord(canonical, id, "general");
    record.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    record.insert(QStringLiteral("mode"), QStringLiteral("claude"));
    record.insert(QStringLiteral("arguments"),
                  QJsonArray{QStringLiteral("--dangerously-skip-permissions")});
    const QJsonObject root{
        {"version", 2},
        {"activeCategory", "general"},
        {"categories",
         QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}, {"selected", id}}}},
        {"agents", QJsonArray{record}}};
    writeRegistry(options.storagePath, root);
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().isEmpty(), "load an agent with arguments");
        require(workspace.addCategory(QStringLiteral("Research")), "save the registry");
    }
    const auto saved = QJsonDocument::fromJson(readRegistry(options.storagePath))
                           .object()
                           .value(QStringLiteral("agents"))
                           .toArray()
                           .first()
                           .toObject()
                           .value(QStringLiteral("arguments"))
                           .toArray();
    require(saved == QJsonArray{QStringLiteral("--dangerously-skip-permissions")},
            "saving keeps the agent's arguments");
    record.insert(QStringLiteral("arguments"), QJsonArray{3});
    auto broken = root;
    broken.insert(QStringLiteral("agents"), QJsonArray{record});
    writeRegistry(options.storagePath, broken);
    Workspace rejected(WorkspaceMode::live, options);
    require(!rejected.workspaceError().isEmpty(), "non-string arguments are rejected");
}

// A direct selection can enter an inactive category with stale selection state.
// Even then, an untiled selection must take a real tile instead of floating
// above the unchanged tile delegates.
void directTileSelectionNormalizesAStaleTarget() {
    QTemporaryDir directory;
    require(directory.isValid(), "tile-selection directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString left = uuid();
    const QString right = uuid();
    const QString untiled = uuid();
    auto agent = [&](const QString& id) { return agentRecord(canonical, id, "work"); };
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{
            {"version", 2},
            {"activeCategory", "general"},
            {"categories",
             QJsonArray{
                 QJsonObject{{"id", "general"}, {"name", "General"}},
                 QJsonObject{{"id", "work"},
                             {"name", "Work"},
                             {"selected", uuid()},
                             {"tiles", QJsonObject{{"stacked", false},
                                                   {"ratio", 0.5},
                                                   {"children",
                                                    QJsonArray{QJsonObject{{"agent", left}},
                                                               QJsonObject{{"agent", right}}}}}}}}},
            {"agents", QJsonArray{agent(left), agent(right), agent(untiled)}}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "load the stale tile-selection registry");
    require(workspace.selectSession(untiled), "select directly into the inactive tiled category");
    const auto tiles = workspace.stageTiles();
    require(tiles.size() == 2 &&
                tiles[0].toMap().value(QStringLiteral("sessionId")).toString() == untiled &&
                tiles[1].toMap().value(QStringLiteral("sessionId")).toString() == right &&
                workspace.focusedSession() == workspace.session(untiled),
            "an untiled direct selection replaces the first valid tile");
}

// The next and previous keys walk the tiles of the layout the walk started
// from, but an agent can leave the category while the walk shows another agent
// in its tile. Closing or moving it then finds no tile to remove, so the walk
// must end instead of stepping onto the departed agent and putting it back.
void tileWalkDropsDisplacedAgentsThatLeave() {
    const auto make = [](const QString& directory, QString& left, QString& right,
                         QString& untiled) {
        const auto canonical = QFileInfo(directory).canonicalFilePath();
        left = uuid();
        right = uuid();
        untiled = uuid();
        auto agent = [&](const QString& id) { return agentRecord(canonical, id, "work"); };
        WorkspaceOptions options;
        options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
        writeRegistry(
            options.storagePath,
            QJsonObject{
                {"version", 2},
                {"activeCategory", "work"},
                {"categories",
                 QJsonArray{QJsonObject{{"id", "work"},
                                        {"name", "Work"},
                                        {"selected", right},
                                        {"tiles",
                                         QJsonObject{{"stacked", false},
                                                     {"ratio", 0.5},
                                                     {"children",
                                                      QJsonArray{QJsonObject{{"agent", left}},
                                                                 QJsonObject{{"agent", right}}}}}}},
                            QJsonObject{{"id", "other"}, {"name", "Other"}}}},
                {"agents", QJsonArray{agent(left), agent(right), agent(untiled)}}});
        return std::make_unique<Workspace>(WorkspaceMode::live, options);
    };
    const auto stage = [](const Workspace& workspace) {
        QStringList result;
        for (const auto& tile : workspace.stageTiles())
            result.append(tile.toMap().value(QStringLiteral("sessionId")).toString());
        return result;
    };
    const auto tile_ids = [](const QJsonObject& tiles) {
        QStringList result;
        const std::function<void(const QJsonObject&)> walk = [&](const QJsonObject& node) {
            const auto children = node.value(QStringLiteral("children")).toArray();
            if (children.isEmpty()) {
                result.append(node.value(QStringLiteral("agent")).toString());
                return;
            }
            for (const auto& child : children)
                walk(child.toObject());
        };
        walk(tiles);
        return result;
    };
    const auto saved = [](const QString& path, const QString& category) {
        const auto groups = QJsonDocument::fromJson(readRegistry(path))
                                .object()
                                .value(QStringLiteral("categories"))
                                .toArray();
        for (const auto& value : groups) {
            const auto group = value.toObject();
            if (group.value(QStringLiteral("id")).toString() == category)
                return group.value(QStringLiteral("tiles")).toObject();
        }
        throw std::runtime_error("missing category in saved registry");
    };

    QTemporaryDir closed_directory;
    require(closed_directory.isValid(), "closed-walk directory");
    QString left;
    QString right;
    QString untiled;
    auto closed = make(closed_directory.path(), left, right, untiled);
    require(closed->workspaceError().isEmpty(), "load the closed-walk registry");
    require(closed->focusedSession() == closed->session(right), "the walk starts from a tile");
    closed->nextSession(1);
    require(stage(*closed) == QStringList{left, untiled} &&
                closed->focusedSession() == closed->session(untiled),
            "the walk shows the untiled agent in the tile it displaced");
    closed->session(right)->setConnection(QStringLiteral("ended"), false);
    require(closed->removeSession(right), "close the displaced agent while the walk shows another");
    require(stage(*closed) == QStringList{left, untiled},
            "closing a displaced agent leaves the shown tiles alone");
    closed->nextSession(-1);
    require(closed->focusedSession() == closed->session(left) &&
                stage(*closed) == QStringList{left, untiled},
            "the walk ends instead of stepping onto the closed agent");
    require(tile_ids(saved(closed->storagePath(), QStringLiteral("work"))) ==
                QStringList{left, untiled},
            "closing a displaced agent never saves its stale tile");

    QTemporaryDir moved_directory;
    require(moved_directory.isValid(), "moved-walk directory");
    auto moved = make(moved_directory.path(), left, right, untiled);
    require(moved->workspaceError().isEmpty(), "load the moved-walk registry");
    moved->nextSession(1);
    require(stage(*moved) == QStringList{left, untiled} &&
                moved->focusedSession() == moved->session(untiled),
            "the moved walk displaces its starting tile");
    require(moved->moveSession(right, QStringLiteral("other")), "move the displaced agent away");
    require(stage(*moved) == QStringList{left, untiled},
            "moving a displaced agent leaves the shown tiles alone");
    moved->nextSession(-1);
    require(moved->focusedSession() == moved->session(left) &&
                stage(*moved) == QStringList{left, untiled},
            "the walk ends instead of showing the moved agent back");
    require(!moved->categorySessions().contains(QVariant::fromValue(moved->session(right))),
            "the moved agent stays out of its old category");
    require(tile_ids(saved(moved->storagePath(), QStringLiteral("work"))) ==
                QStringList{left, untiled},
            "moving a displaced agent never saves its stale tile");
}

// A walk belongs to its category. A no-op or real step elsewhere must not
// replace the saved home layout that lets the original walk restore its tiles.
void tileWalkSurvivesAnotherCategory() {
    QTemporaryDir directory;
    require(directory.isValid(), "cross-category walk directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const QString left = uuid();
    const QString right = uuid();
    const QString untiled = uuid();
    const QString alone = uuid();
    const QString first_away = uuid();
    const QString second_away = uuid();
    const auto agent = [&](const QString& id, const char* category) {
        return agentRecord(root.path(), id, category);
    };
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{
            {"version", 2},
            {"activeCategory", "work"},
            {"categories",
             QJsonArray{QJsonObject{
                            {"id", "work"},
                            {"name", "Work"},
                            {"selected", right},
                            {"tiles",
                             QJsonObject{{"stacked", false},
                                         {"ratio", 0.5},
                                         {"children", QJsonArray{QJsonObject{{"agent", left}},
                                                                 QJsonObject{{"agent", right}}}}}}},
                        QJsonObject{{"id", "solo"}, {"name", "Solo"}, {"selected", alone}},
                        QJsonObject{{"id", "away"}, {"name", "Away"}, {"selected", first_away}}}},
            {"agents", QJsonArray{agent(left, "work"), agent(right, "work"), agent(untiled, "work"),
                                  agent(alone, "solo"), agent(first_away, "away"),
                                  agent(second_away, "away")}}});
    Workspace workspace(WorkspaceMode::live, options);
    const auto stage = [](const Workspace& item) {
        QStringList result;
        for (const auto& tile : item.stageTiles())
            result.append(tile.toMap().value(QStringLiteral("sessionId")).toString());
        return result;
    };
    require(workspace.workspaceError().isEmpty(), "load the cross-category walk registry");
    workspace.nextSession(1);
    require(stage(workspace) == QStringList{left, untiled} &&
                workspace.focusedSession() == workspace.session(untiled),
            "the original walk displaces a home tile");

    require(workspace.selectCategory(QStringLiteral("solo")), "visit a one-agent category");
    workspace.nextSession(1);
    require(workspace.focusedSession() == workspace.session(alone),
            "a one-agent category's next key is a no-op");
    require(workspace.selectCategory(QStringLiteral("away")), "visit a two-agent category");
    workspace.nextSession(1);
    require(workspace.focusedSession() == workspace.session(second_away),
            "another category can take a real step");

    require(workspace.selectCategory(QStringLiteral("work")), "return to the original walk");
    require(workspace.focusedSession() == workspace.session(untiled) &&
                stage(workspace) == QStringList{left, untiled},
            "returning preserves the original walk's shown stage");
    workspace.nextSession(1);
    require(workspace.focusedSession() == workspace.session(left),
            "the original walk restores its selected home tile");
    require(stage(workspace) == QStringList{left, right},
            "the original walk restores its saved home tiles");
}

void unknownRegistryVersionsAreRejected() {
    QTemporaryDir directory;
    require(directory.isValid(), "version directory");
    WorkspaceOptions options;
    options.storagePath = QDir(directory.path()).filePath(QStringLiteral("workspace.json"));
    const auto bytes =
        QJsonDocument(QJsonObject{{"version", 3},
                                  {"activeCategory", "general"},
                                  {"categories",
                                   QJsonArray{QJsonObject{
                                       {"id", "general"}, {"name", "General"}, {"selected", ""}}}},
                                  {"agents", QJsonArray{}}})
            .toJson();
    writeRegistry(options.storagePath, QJsonDocument::fromJson(bytes).object());
    Workspace workspace(WorkspaceMode::live, options);
    require(!workspace.workspaceError().isEmpty(), "unknown registry versions are rejected");
    require(workspace.sessions().isEmpty() && !workspace.focusedSession(),
            "unknown registry versions create no sessions");
    require(readRegistry(options.storagePath) == bytes, "unknown versions remain for migration");
}

// Harnesses without an observer get an output-timing estimate that
// says so; reattaching does not count as activity.
void outputEstimate() {
    lapis::desktop::SessionPreview item(QStringLiteral("Grok"), {}, {}, QColor{}, "");
    item.setHarnessId(QStringLiteral("grok"));
    item.setStatusSource(lapis::desktop::SessionPreview::StatusSource::output);
    item.setOutputTimingForTesting({.settle_ms = 200, .burst_ms = 1500, .quiet_ms = 300});
    QTemporaryDir directory;
    require(directory.isValid(), "estimate directory");
    item.startLive(directory.filePath(QStringLiteral("absent.sock")),
                   {QStringLiteral("/usr/bin/true"), {}, directory.path()},
                   lapis::session::wire::AttachMode::reconnect);
    waitFor([] { return false; }, 300);
    require(item.connectionState() == QStringLiteral("disconnected"), "no service in fixture");
    item.setConnection(QStringLiteral("ready"), true);
    const auto snapshot = item.snapshot();
    for (int frame = 0; frame < 3; ++frame)
        item.applySnapshot(snapshot);
    require(item.statusKind() == QStringLiteral("unknown"), "the reattach replay is ignored");
    QThread::msleep(250);
    for (int frame = 0; frame < 3; ++frame)
        item.applySnapshot(snapshot);
    require(item.statusKind() == QStringLiteral("working") &&
                item.statusLabel() == QStringLiteral("Output active"),
            "a burst of frames reads as output activity, labelled as an estimate");
    require(waitFor([&] { return item.statusKind() == QStringLiteral("idle"); }, 2000) &&
                item.statusLabel() == QStringLiteral("Quiet"),
            "silence after activity reads as quiet, not finished");
    item.setConnection(QStringLiteral("disconnected"), false);
    for (int frame = 0; frame < 3; ++frame)
        item.applySnapshot(snapshot);
    item.setConnection(QStringLiteral("ready"), true);
    for (int frame = 0; frame < 3; ++frame)
        item.applySnapshot(snapshot);
    require(item.statusKind() != QStringLiteral("working"),
            "a later reconnect replay must not count as fresh output");
}

// An agent is a top-level session even when lapis itself was opened from
// inside another agent's terminal: parent-session markers never reach it.
void agentsStartWithoutParentSessionMarkers() {
    QTemporaryDir directory;
    require(directory.isValid(), "marker directory");
    qputenv("CLAUDECODE", "1");
    qputenv("CLAUDE_CODE_CHILD_SESSION", "1");
    qputenv("CLAUDE_CODE_SESSION_ID", "parent-session");
    qputenv("GROK_AGENT", "1");
    qputenv("CLAUDE_CODE_EFFORT_LEVEL", "max");
    const auto restore = [] {
        for (const char* name : {"CLAUDECODE", "CLAUDE_CODE_CHILD_SESSION", "GROK_AGENT",
                                 "CLAUDE_CODE_SESSION_ID", "CLAUDE_CODE_EFFORT_LEVEL"})
            qunsetenv(name);
    };
    const auto record = QDir(directory.path()).filePath(QStringLiteral("environment"));
    WorkspaceOptions options;
    options.endpoint = QDir(QFileInfo(directory.path()).canonicalFilePath())
                           .filePath(QStringLiteral("agent.sock"));
    options.launch = lapis::session::LaunchSpec{
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"), QStringLiteral("env > environment.tmp && mv environment.tmp "
                                              "environment && exec sleep 600")},
        directory.path(),
        {80, 24},
        lapis::session::AgentMode::terminal};
    options.mode = lapis::session::wire::AttachMode::create;
    {
        Workspace workspace(WorkspaceMode::live, options);
        // The service starts from the event loop, so keep the markers until then.
        const bool recorded = waitFor([&] { return QFileInfo::exists(record); }, 10000);
        restore();
        require(recorded, "agent recorded its environment");
        QFile file(record);
        require(file.open(QIODevice::ReadOnly), "read agent environment");
        const auto lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        for (const auto& marker : {"CLAUDECODE=", "CLAUDE_CODE_CHILD_SESSION=",
                                   "CLAUDE_CODE_SESSION_ID=", "AI_AGENT=", "GROK_AGENT="})
            require(std::none_of(lines.begin(), lines.end(),
                                 [&](const QString& line) {
                                     return line.startsWith(QLatin1String(marker));
                                 }),
                    "parent-session markers are removed");
        require(lines.contains(QStringLiteral("CLAUDE_CODE_EFFORT_LEVEL=max")),
                "user configuration is kept");
        auto* agent = workspace.focusedSession();
        require(waitFor([agent] { return agent->inputReady(); }, 10000), "agent ready");
        require(workspace.closeSession(agent->sessionId()) &&
                    waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "marker fixture closes");
    }
}

QString screenText(const lapis::session::TerminalSnapshot& snapshot) {
    QString text;
    for (std::size_t index = 0; index < snapshot.cells.size(); ++index) {
        const auto cell = snapshot.text(index);
        text += cell.empty() ? QStringLiteral(" ")
                             : QString::fromUcs4(cell.data(), static_cast<qsizetype>(cell.size()));
        if ((index + 1) % snapshot.size.columns == 0)
            text += QLatin1Char('\n');
    }
    return text;
}

// A view joined beside the desktop, as the phone gateway joins: the same agent
// on both, typing from either reaches both, and the desktop stays attached.
class JoinedView {
  public:
    JoinedView(const QString& endpoint, const lapis::session::LaunchSpec& launch) {
        namespace wire = lapis::session::wire;
        socket_.connectToServer(endpoint);
        require(socket_.waitForConnected(3000), "the view connects");
        socket_.write(wire::frame(
            wire::Kind::attach,
            wire::encode_attach({.mode = wire::AttachMode::join,
                                 .fingerprint = lapis::session::launch_fingerprint(launch),
                                 .expected = {}})));
    }
    // Reads frames until the screen shows the text; acknowledges the first.
    bool waitForText(const QString& text, int timeout_ms) {
        namespace wire = lapis::session::wire;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeout_ms) {
            wire::Frame frame;
            while (wire::take_frame(buffer_, frame)) {
                if (frame.kind == wire::Kind::status)
                    throw std::runtime_error(
                        "the joined view was closed: " +
                        wire::decode_status(frame.payload).message.toStdString());
                if (frame.kind == wire::Kind::hello)
                    attachment_ = wire::decode_hello(frame.payload).attachment;
                if (frame.kind != wire::Kind::snapshot)
                    continue;
                const auto message = wire::decode_snapshot_message(frame.payload);
                screen_ = screenText(message.snapshot);
                if (!ready_) {
                    socket_.write(wire::frame(wire::Kind::ready,
                                              wire::encode_ready({attachment_, message.sequence})));
                    ready_ = true;
                }
            }
            if (ready_ && screen_.contains(text))
                return true;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            socket_.waitForReadyRead(20);
            buffer_ += socket_.readAll();
        }
        return false;
    }
    void type(const QByteArray& bytes) {
        namespace wire = lapis::session::wire;
        socket_.write(wire::frame(wire::Kind::text, wire::encode_control({attachment_, bytes})));
        socket_.flush();
    }
    void resize(quint16 columns, quint16 rows) {
        namespace wire = lapis::session::wire;
        QByteArray bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out << columns << rows;
        socket_.write(wire::frame(wire::Kind::resize, wire::encode_control({attachment_, bytes})));
        socket_.flush();
    }
    void leave() { socket_.disconnectFromServer(); }

  private:
    QLocalSocket socket_;
    QByteArray buffer_;
    lapis::session::wire::Attachment attachment_;
    QString screen_;
    bool ready_{};
};

void joinedViewStaysInSync() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-join-XXXXXX"));
    require(directory.isValid(), "service directory");
    WorkspaceOptions options;
    options.endpoint = QDir(QFileInfo(directory.path()).canonicalFilePath())
                           .filePath(QStringLiteral("agent.sock"));
    options.launch = lapis::session::LaunchSpec{
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"), QStringLiteral("while read line; do echo \"got:$line\"; done")},
        directory.path(),
        {80, 24},
        lapis::session::AgentMode::terminal};
    options.mode = lapis::session::wire::AttachMode::create;
    Workspace workspace(WorkspaceMode::live, options);
    auto* agent = workspace.focusedSession();
    require(agent != nullptr && waitFor([agent] { return agent->inputReady(); }, 10000),
            "the desktop is attached");
    // The service fingerprints the validated launch (canonical folder).
    JoinedView phone(options.endpoint, lapis::session::validate_launch(*options.launch));
    require(phone.waitForText(QString(), 5000), "the joined view receives the screen");
    phone.type("from phone\r");
    require(waitFor(
                [agent] {
                    return screenText(agent->snapshot()).contains(QStringLiteral("got:from phone"));
                },
                5000),
            "the desktop shows what the phone typed");
    agent->sendText("from mac\r");
    require(phone.waitForText(QStringLiteral("got:from mac"), 5000),
            "the phone shows what the desktop typed");
    require(agent->inputReady() && agent->connectionState() == QStringLiteral("ready"),
            "the desktop was never replaced");
    {
        // A ready connection ignores reconnect(). Replace it first so this
        // fixture exercises service detach/attach while the phone stays joined.
        namespace wire = lapis::session::wire;
        QLocalSocket disposable;
        disposable.connectToServer(options.endpoint);
        require(disposable.waitForConnected(3000), "the disposable client connects");
        const auto takeover =
            wire::frame(wire::Kind::attach,
                        wire::encode_attach({.mode = wire::AttachMode::discover,
                                             .fingerprint = lapis::session::launch_fingerprint(
                                                 lapis::session::validate_launch(*options.launch)),
                                             .expected = {}}));
        require(disposable.write(takeover) == takeover.size() &&
                    disposable.waitForBytesWritten(3000),
                "the disposable takeover was sent");
        require(waitFor(
                    [agent] {
                        return !agent->inputReady() &&
                               agent->connectionState() == QStringLiteral("replaced");
                    },
                    5000),
                "the takeover really detached the desktop");
        disposable.abort();
    }
    agent->reconnect();
    require(waitFor([agent] { return agent->inputReady(); }, 10000), "the desktop reattaches");
    phone.type("after reconnect\r");
    require(phone.waitForText(QStringLiteral("got:after reconnect"), 5000),
            "the joined view survives the desktop reattaching");
    require(waitFor(
                [agent] {
                    return screenText(agent->snapshot())
                        .contains(QStringLiteral("got:after reconnect"));
                },
                5000),
            "the reattached desktop receives the phone's new output");
    agent->sendText("reattached mac\r");
    require(phone.waitForText(QStringLiteral("got:reattached mac"), 5000),
            "the phone receives input from the new desktop attachment");
    require(workspace.closeSession(agent->sessionId()) &&
                waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "the joined fixture closes");
}

struct UpdaterFixture {
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-updater-XXXXXX")};
    QDir root{QFileInfo(directory.path()).canonicalFilePath()};
    QByteArray original_path{qgetenv("PATH")};
    QByteArray original_home{qgetenv("HOME")};
    WorkspaceOptions options;
    QString harness{"grok"};

    UpdaterFixture(const QByteArray& script, const QString& harness_name = QStringLiteral("grok"))
        : harness(harness_name) {
        require(directory.isValid() && root.mkpath(QStringLiteral("bin")) &&
                    root.mkpath(QStringLiteral("project")),
                "create updater fixture folders");
        writeExecutable(root.filePath(QStringLiteral("bin/") + harness), script);
        options.storagePath = root.filePath(QStringLiteral("workspace.json"));
        options.updateHarnesses = true;
        qputenv("PATH", QFile::encodeName(root.filePath(QStringLiteral("bin"))) + ":/usr/bin:/bin");
        qputenv("HOME", QFile::encodeName(root.path()));
    }
    ~UpdaterFixture() {
        qputenv("PATH", original_path);
        qputenv("HOME", original_home);
    }
    UpdaterFixture(const UpdaterFixture&) = delete;
    UpdaterFixture& operator=(const UpdaterFixture&) = delete;
    [[nodiscard]] QByteArray read(const QString& name) const {
        QFile file(root.filePath(name));
        require(file.open(QIODevice::ReadOnly), "open updater fixture output");
        return file.readAll();
    }
    void create(Workspace& workspace) const {
        require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                      QStringLiteral("updater fixture"), harness),
                "create updater fixture agent");
    }
};

void updaterLifecycle() {
    enum class Cleanup : std::uint8_t { timeout, destruction, normal_exit };
    for (const auto mode : {Cleanup::timeout, Cleanup::destruction, Cleanup::normal_exit}) {
        const bool destroy = mode == Cleanup::destruction;
        const bool normal_exit = mode == Cleanup::normal_exit;
        UpdaterFixture fixture(
            QByteArray("#!/bin/sh\nnormal_exit=") + (normal_exit ? "1\n" : "0\n") +
                "root=\"${0%/*}\"\n"
                "if [ \"$1\" = update ]; then\n"
                "  trap 'exit 0' TERM\n"
                "  (trap '' TERM; while :; do /bin/sleep 1; done) &\n"
                "  printf '%s\\n' \"$!\" > \"$root/descendant-pid\"\n"
                "  printf '%s\\n' \"$$\" > \"$root/updater-pid\"\n"
                "  if [ \"$normal_exit\" = 1 ]; then exit 0; fi\n"
                "  while :; do /bin/sleep 1; done\n"
                "fi\n"
                "for file in updater-pid descendant-pid; do\n"
                "  read -r pid < \"$root/$file\"\n"
                "  if kill -0 \"$pid\" 2>/dev/null; then echo overlap > \"$root/overlap\"; fi\n"
                "done\n"
                "echo started >> \"$root/starts\"\n"
                "echo started\n"
                "while read -r line; do [ \"$line\" = done ] && exit 0; done\n",
            QStringLiteral("grok"));
        // Instrumented fork/exec startup can outlast the short updater budget.
        // Keep the normal run fast while allowing the fixture to start in TSan.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
        constexpr int startup_timeout_ms = 10000;
        constexpr int updater_timeout_ms = 5000;
#else
        constexpr int startup_timeout_ms = 2000;
        constexpr int updater_timeout_ms = 300;
#endif
#else
        constexpr int startup_timeout_ms = 2000;
        constexpr int updater_timeout_ms = 300;
#endif
        fixture.options.updateTimeoutMs = destroy ? 30000 : updater_timeout_ms;
        auto workspace = std::make_unique<Workspace>(WorkspaceMode::live, fixture.options);
        fixture.create(*workspace);
        auto* agent = workspace->focusedSession();
        const auto id = agent->sessionId();
        for (int attempt = 0; attempt < 3; ++attempt)
            require(!workspace->restartAgent(id), "repeated restart while queued is rejected");
        require(!agent->live(), "queued restarts never create an early service");
        require(waitFor([&] { return QFileInfo::exists(fixture.root.filePath("bin/updater-pid")); },
                        startup_timeout_ms),
                "updater and descendant acknowledge startup");
        const auto leader = fixture.read("bin/updater-pid").trimmed().toLongLong();
        const auto child = fixture.read("bin/descendant-pid").trimmed().toLongLong();
        require(leader > 0 && child > 0, "fixture has two explicit owned PIDs");
        if (destroy) {
            workspace.reset();
        } else {
            require(waitFor([agent] { return agent->inputReady(); }, 10000),
                    "timeout releases exactly one launch after updater exit");
            require(waitFor([&] { return QFileInfo::exists(fixture.root.filePath("bin/starts")); },
                            2000),
                    "new agent acknowledges execution");
            require(fixture.read("bin/starts") == "started\n", "queued agent starts once");
            require(!QFileInfo::exists(fixture.root.filePath("bin/overlap")),
                    "updater and descendant have exited before agent execution");
            require(fixture.read("harness-updates.log")
                        .contains(normal_exit ? "exit 0" : "stopped after timeout"),
                    "timeout outcome is recorded");
            require(workspace->closeSession(id) &&
                        waitFor([&] { return workspace->sessions().isEmpty(); }, 10000),
                    "close updater fixture agent");
        }
#if defined(Q_OS_UNIX)
        require(waitFor(
                    [&] {
                        return ::kill(static_cast<pid_t>(leader), 0) != 0 &&
                               ::kill(static_cast<pid_t>(child), 0) != 0;
                    },
                    2000),
                "timeout and destruction stop both owned updater processes");
#endif
    }
}

void updaterOutputIsDrainedWithABoundedTail() {
    UpdaterFixture fixture("#!/bin/sh\n"
                           "if [ \"$1\" = update ]; then\n"
                           "  /usr/bin/yes x | /usr/bin/head -c 1048576\n"
                           "  echo tail-marker\n"
                           "  exit 0\n"
                           "fi\n"
                           "echo started\n"
                           "while read -r line; do [ \"$line\" = done ] && exit 0; done\n");
    Workspace workspace(WorkspaceMode::live, fixture.options);
    fixture.create(workspace);
    auto* agent = workspace.focusedSession();
    require(waitFor([agent] { return agent->inputReady(); }, 10000),
            "high-volume updater completes before agent launch");
    const auto logged = fixture.read("harness-updates.log");
    require(logged.contains("tail-marker") && logged.size() < 2048,
            "only the bounded updater tail reaches the log");
    require(workspace.closeSession(agent->sessionId()) &&
                waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "close output fixture agent");
}

void failedUpdaterStartClearsTheQueue() {
    UpdaterFixture fixture("#!/nonexistent/lapis-fake-updater\n");
    Workspace workspace(WorkspaceMode::live, fixture.options);
    fixture.create(workspace);
    auto* agent = workspace.focusedSession();
    require(
        waitFor([agent] { return agent->statusLabel() != QStringLiteral("Updating Grok…"); }, 2000),
        "FailedToStart releases the queue");
    require(fixture.read("harness-updates.log").contains("grok update: could not start"),
            "failed start is logged without waiting for finished");
}

void explicitLaunchesUseUpdaterPolicy() {
    UpdaterFixture fixture("#!/bin/sh\n"
                           "if [ \"$1\" = update ]; then\n"
                           "  echo update >> \"$HOME/updates\"\n"
                           "  while [ ! -f \"$HOME/release-update\" ]; do /bin/sleep .02; done\n"
                           "  exit 0\n"
                           "fi\n"
                           "echo $$ >> \"$HOME/starts\"\n"
                           "exec /bin/sleep 60\n",
                           QStringLiteral("claude"));
    const auto endpoint = fixture.root.filePath(QStringLiteral("agent.sock"));
    const auto launch =
        lapis::session::LaunchSpec{fixture.root.filePath(QStringLiteral("bin/claude")),
                                   {},
                                   fixture.root.filePath(QStringLiteral("project")),
                                   {80, 24},
                                   lapis::session::AgentMode::claude};
    auto options = fixture.options;
    options.endpoint = endpoint;
    options.launch = launch;
    options.mode = lapis::session::wire::AttachMode::create;
    QByteArray original_pid;
    {
        Workspace workspace(WorkspaceMode::live, options);
        auto* agent = workspace.focusedSession();
        require(agent && agent->statusLabel() == QStringLiteral("Updating Claude…") &&
                    !agent->live(),
                "explicit creation waits before starting");
        require(waitFor([&] { return QFileInfo::exists(fixture.root.filePath("updates")); }, 10000),
                "the explicit updater starts first");
        require(!agent->live() && !QFileInfo::exists(fixture.root.filePath("starts")),
                "the agent cannot start before the update finishes");
        QFile release(fixture.root.filePath(QStringLiteral("release-update")));
        require(release.open(QIODevice::WriteOnly), "release the acknowledged updater");
        release.close();
        require(waitFor(
                    [&] {
                        return agent->inputReady() &&
                               QFileInfo::exists(fixture.root.filePath(QStringLiteral("starts")));
                    },
                    20000),
                "the deferred explicit agent starts");
        original_pid = fixture.read(QStringLiteral("starts"));
        require(
            lapis::session::read_descriptor(endpoint, lapis::session::launch_fingerprint(launch))
                .has_value(),
            "the deferred start records its normalized endpoint and launch identity");
        require(fixture.read("harness-updates.log").contains("claude update: exit 0"),
                "the explicit update is logged beside the endpoint");
    }
    require(QFile::remove(fixture.root.filePath("updates")) &&
                QFile::remove(fixture.root.filePath("harness-updates.log")),
            "clear update observations before reattachment");
    for (const auto mode : {lapis::session::wire::AttachMode::reconnect,
                            lapis::session::wire::AttachMode::discover}) {
        options.mode = mode;
        Workspace workspace(WorkspaceMode::live, options);
        auto* agent = workspace.focusedSession();
        require(agent && waitFor([agent] { return agent->inputReady(); }, 20000),
                "explicit reattachment retains its mode");
        require(fixture.read("starts") == original_pid &&
                    !QFileInfo::exists(fixture.root.filePath("updates")) &&
                    !QFileInfo::exists(fixture.root.filePath("harness-updates.log")),
                "reattachment keeps the same process without updating");
        if (mode == lapis::session::wire::AttachMode::discover)
            require(workspace.closeSession(agent->sessionId()) &&
                        waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                    "explicit fixture closes after both reattachments");
    }
    options.mode = lapis::session::wire::AttachMode::create;
    options.updateHarnesses = false;
    options.endpoint = fixture.root.filePath(QStringLiteral("disabled.sock"));
    Workspace workspace(WorkspaceMode::live, options);
    auto* agent = workspace.focusedSession();
    require(agent && waitFor([agent] { return agent->inputReady(); }, 20000),
            "update-disabled explicit creation starts");
    require(!QFileInfo::exists(fixture.root.filePath("updates")) &&
                !QFileInfo::exists(fixture.root.filePath("harness-updates.log")),
            "the explicit update opt-out is honored");
    require(workspace.closeSession(agent->sessionId()) &&
                waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "update-disabled fixture closes");
}

// The login helper (lapis_desktop --restore-agents) holds the workspace only
// while it restarts agents: a window opened during its lock-to-marker gap
// waits briefly for publication; an established helper or windowless host
// waits for handover; a duplicate headless helper fails at once.
void windowWaitsForTheRestoreHelper() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-lock-XXXXXX"));
    require(directory.isValid(), "lock directory");
    WorkspaceOptions options;
    options.storagePath = QDir(QFileInfo(directory.path()).canonicalFilePath())
                              .filePath(QStringLiteral("workspace.json"));
    // The helper's lock and marker, as lapis_desktop --restore-agents keeps
    // them. It owns the lock briefly before it can replace a previous marker,
    // so start with that stale value and publish the helper's PID in the gap.
    QLockFile helper(options.storagePath + QStringLiteral(".lock"));
    require(helper.tryLock(0), "the helper holds the workspace");
    QFile marker(options.storagePath + QStringLiteral(".restoring"));
    require(marker.open(QIODevice::WriteOnly) && marker.write(QByteArrayLiteral("0")) > 0,
            "the helper holds a stale marker before naming itself");
    marker.close();
    // A second helper must not wait even if the first helper already published.
    require(marker.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                marker.write(QByteArray::number(QCoreApplication::applicationPid())) > 0,
            "publish the first helper identity");
    marker.close();
    auto restoring = options;
    restoring.restoreAgents = true;
    restoring.headless = true;
    QElapsedTimer helper_clock;
    helper_clock.start();
    Workspace duplicate_helper(WorkspaceMode::live, restoring);
    require(!duplicate_helper.workspaceError().isEmpty() && helper_clock.elapsed() < 1000,
            "a helper never waits for another helper");
    require(marker.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                marker.write(QByteArrayLiteral("0")) > 0,
            "restore the unpublished marker");
    marker.close();
    // The helper exits: its marker goes with its lock.
    std::thread release([&helper, &options] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        QFile marker(options.storagePath + QStringLiteral(".restoring"));
        require(marker.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "open the marker in the helper race");
        require(marker.write(QByteArray::number(QCoreApplication::applicationPid())) > 0,
                "the helper names itself after taking the lock");
        marker.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        QFile::remove(options.storagePath + QStringLiteral(".restoring"));
        helper.unlock();
    });
    QElapsedTimer clock;
    clock.start();
    Workspace window(WorkspaceMode::live, options);
    release.join();
    require(window.workspaceError().isEmpty() && clock.elapsed() >= 700,
            "a window waits for the login helper");
    clock.restart();
    Workspace second(WorkspaceMode::live, options);
    require(!second.workspaceError().isEmpty() && clock.elapsed() < 3000,
            "a second window waits only for marker publication");
    Workspace late(WorkspaceMode::live, restoring);
    require(!late.workspaceError().isEmpty(), "the helper leaves an open window's workspace alone");

    WorkspaceOptions short_options;
    short_options.storagePath = QDir(QFileInfo(directory.path()).canonicalFilePath())
                                    .filePath(QStringLiteral("short.json"));
    QLockFile short_helper(short_options.storagePath + QStringLiteral(".lock"));
    require(short_helper.tryLock(0), "a short helper holds another workspace");
    std::thread short_release([&short_helper] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        short_helper.unlock();
    });
    Workspace after_short_helper(WorkspaceMode::live, short_options);
    short_release.join();
    require(after_short_helper.workspaceError().isEmpty(),
            "a window acquires a helper lock released without a marker");
}

// A request to the process that owns the workspace, answered while this
// thread's event loop runs (a window's control server lives on it).
QJsonObject askWorkspace(const QString& registry, const QJsonObject& request) {
    QLocalSocket socket;
    socket.connectToServer(lapis::desktop::WorkspaceControl::path(registry));
    require(waitFor([&socket] { return socket.state() == QLocalSocket::ConnectedState; }, 3000),
            "the workspace takes requests");
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    QByteArray answer;
    require(waitFor(
                [&] {
                    answer += socket.readAll();
                    return answer.contains('\n');
                },
                5000),
            "the workspace answers");
    return QJsonDocument::fromJson(answer.trimmed()).object();
}

QJsonObject createRequest(const QString& category, const QString& harness,
                          const QString& directory) {
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("request"), QStringLiteral("createAgent")},
            {QStringLiteral("category"), category},
            {QStringLiteral("harness"), harness},
            {QStringLiteral("directory"), directory}};
}

// A stand-in Grok on PATH that says where it started.
QByteArray installStandInGrok(const QDir& root) {
    require(root.mkpath(QStringLiteral("bin")) && root.mkpath(QStringLiteral("project")),
            "fixture folders");
    QFile script(root.filePath(QStringLiteral("bin/grok")));
    require(script.open(QIODevice::WriteOnly), "write the stand-in CLI");
    script.write("#!/bin/sh\necho \"grok ready in $(pwd)\"\nexec sleep 600\n");
    script.close();
    require(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make it executable");
    const auto path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(root.filePath(QStringLiteral("bin"))) + ':' + path);
    return path;
}

// Restore planning must install the complete SSH policy or leave the saved
// command intact. No event loop is pumped, so no executable is launched.
void remoteOptionsRespectTheArgumentLimit() {
    struct Case {
        int count{};
        QStringList options;
        QStringList added;
        bool rejected{};
        bool live{};
    };
    const QStringList policy{"-o", "ControlPath=none",     "-o", "ServerAliveInterval=15",
                             "-o", "ServerAliveCountMax=4"};
    const std::vector<Case> cases{{58, {}, policy},
                                  {60, {}, {}, true},
                                  {62, {}, {}, true},
                                  {64, policy, {}},
                                  {62, policy.mid(2), policy.mid(0, 2)},
                                  {58, {"-o", "ControlPath=shared"}, policy},
                                  {58, {"ControlPath=none"}, policy},
                                  {58, {"-v", "-o", "ControlPath=none"}, policy.mid(2)},
                                  {62, {}, {}, false, true}};
    for (const auto& variant : cases) {
        QTemporaryDir directory(QStringLiteral("/tmp/lapis-remote-options-XXXXXX"));
        require(directory.isValid(), "remote options directory");
        const QDir root(QFileInfo(directory.path()).canonicalFilePath());
        const auto id = uuid();
        auto record = agentRecord(root.path(), id, "general");
        const auto program = root.filePath(QStringLiteral("ssh"));
        writeExecutable(program, "#!/usr/bin/env bash\nexit 0\n");
        auto arguments = variant.options;
        while (arguments.size() < variant.count - 3)
            arguments << QStringLiteral("-v");
        arguments << QStringLiteral("-t") << QStringLiteral("fixture")
                  << QStringLiteral("exec grok");
        record.insert(QStringLiteral("program"), program);
        record.insert(QStringLiteral("harness"), QStringLiteral("grok"));
        record.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(arguments));
        WorkspaceOptions options;
        options.storagePath = root.filePath(QStringLiteral("workspace.json"));
        options.restoreAgents = true;
        writeRegistry(
            options.storagePath,
            {{"version", 2},
             {"activeCategory", "general"},
             {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
             {"agents", QJsonArray{record}}});
        QLocalServer listener;
        if (variant.live)
            require(listener.listen(record.value(QStringLiteral("endpoint")).toString()),
                    "the existing service stays listening");
        const auto expected = variant.added + arguments;
        for (int pass = 0; pass < 2; ++pass) {
            Workspace workspace(WorkspaceMode::live, options);
            require(workspace.sessions().size() == 1, "a rejected migration retains the tab");
            require(variant.rejected
                        ? workspace.workspaceError().contains(QStringLiteral("no room"))
                        : workspace.workspaceError().isEmpty(),
                    "an unrepresentable SSH migration has an explicit diagnostic");
            const auto saved = QJsonDocument::fromJson(readRegistry(options.storagePath))
                                   .object()[QStringLiteral("agents")]
                                   .toArray()
                                   .first()
                                   .toObject();
            require(saved[QStringLiteral("arguments")].toArray() ==
                        QJsonArray::fromStringList(expected),
                    "restore is complete and idempotent, or leaves the original argv untouched");
        }
    }
}

// A Claude Code agent on another machine keeps one conversation: its first
// launch names it, and a reconnect after the connection dropped, a restart
// and nothing else resume it. A stand-in ssh records what it was given, per
// host: devbox drops the first connection (ssh exits 255), then the second
// ends by itself; typo exits immediately and never holds a connection. An
// agent saved before lapis gave each ssh its own connection (older) gains that
// and the keepalives on restore.
void remoteClaudeReconnectsToItsConversation() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-reconnect-XXXXXX"));
    require(directory.isValid(), "reconnect directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    QFile ssh(root.filePath(QStringLiteral("bin/ssh")));
    require(ssh.open(QIODevice::WriteOnly), "write the stand-in ssh");
    ssh.write(R"(#!/bin/sh
d=$(dirname "$0"); host=$(printf '%s\n' "$@" | sed -n '/^-t$/{n;p;q;}')
n=$(($(cat "$d/$host.count" 2>/dev/null || echo 0) + 1))
printf '%s\n' "$@" > "$d/$host.call$n"
echo $n > "$d/$host.count"
case "$host:$n" in typo:*) exit 255;; devbox:1) sleep 1; exit 255;; devbox:2) sleep 1; exit 1;; esac
echo connected
exec sleep 600
)");
    ssh.close();
    require(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make it executable");
    QFile config(root.filePath(QStringLiteral("ssh_config")));
    require(config.open(QIODevice::WriteOnly), "write an ssh config");
    config.write("Host devbox typo\n");
    config.close();
    const auto calls = [&root](const QString& host) {
        QFile count(root.filePath(QStringLiteral("bin/%1.count").arg(host)));
        return count.open(QIODevice::ReadOnly) ? count.readAll().trimmed().toInt() : 0;
    };
    const auto call = [&root](const QString& host, int number) {
        QFile file(root.filePath(QStringLiteral("bin/%1.call%2").arg(host).arg(number)));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    const auto conversation = [](const QString& arguments) {
        static const QRegularExpression id(QStringLiteral(R"( && s=([0-9a-f-]{36}) && )"));
        return id.match(arguments).captured(1);
    };
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.restoreAgents = true;
    const auto older_id = QStringLiteral("5a1d0000-0000-4000-8000-000000000001");
    auto older = agentRecord(root.path(), older_id, "general");
    older.insert(QStringLiteral("title"), QStringLiteral("older"));
    older.insert(QStringLiteral("program"), root.filePath(QStringLiteral("bin/ssh")));
    older.insert(QStringLiteral("directory"), QDir::homePath());
    older.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    older.insert(QStringLiteral("arguments"),
                 QJsonArray{"-t", "older", R"(cd ~ && exec "${SHELL:-/bin/sh}" -lic claude)"});
    writeRegistry(options.storagePath,
                  QJsonObject{{"version", 2},
                              {"activeCategory", "general"},
                              {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                    {"name", "General"},
                                                                    {"selected", older_id}}}},
                              {"agents", QJsonArray{older}}});
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(waitFor([&] { return calls(QStringLiteral("older")) >= 1; }, 10000) &&
                    call(QStringLiteral("older"), 1)
                        .startsWith(QStringLiteral("-o\nControlPath=none\n-o\n"
                                                   "ServerAliveInterval=15\n-o\n"
                                                   "ServerAliveCountMax=4\n-t\nolder\n")),
                "an agent saved before gets its own connection and keepalives");
        workspace.setSshConfigForTesting(config.fileName());
        workspace.setReconnectTimingForTesting({.first_hold = std::chrono::milliseconds(300),
                                                .wait = std::chrono::milliseconds(2000)});
        require(workspace.createAgent(QStringLiteral("~/dev/far"), QStringLiteral("far"),
                                      QStringLiteral("claude"), {}, {}, QStringLiteral("devbox")),
                "a Claude Code agent on another machine");
        auto* far = workspace.focusedSession();
        require(far != nullptr, "the new agent is shown");
        const auto id = far->sessionId();
        require(waitFor([&] { return calls(QStringLiteral("devbox")) >= 1; }, 10000), "ssh starts");
        const auto first = call(QStringLiteral("devbox"), 1);
        require(
            first.startsWith(QStringLiteral("-o\nControlPath=none\n-o\nServerAliveInterval=15\n"
                                            "-o\nServerAliveCountMax=4\n-t\ndevbox\n")) &&
                first.contains(QStringLiteral("cd ~/dev/far && s=")) &&
                !conversation(first).isEmpty() &&
                first.contains(QStringLiteral(
                    R"(-lic 'export CLAUDE_CODE_NO_FLICKER="${CLAUDE_CODE_NO_FLICKER:-1}"; claude --permission-mode bypassPermissions '"$o $s")")),
            "ssh has its own connection, kept alive, and names the conversation; with no mode "
            "asked for, Claude Code starts in Full access, as the forms default, not auto mode");
        require(workspace.agentPlace(id).value(QStringLiteral("place")) ==
                    QStringLiteral("devbox:~/dev/far"),
                "the agent's place is still its machine and folder");
        require(waitFor(
                    [&] {
                        return calls(QStringLiteral("devbox")) >= 1 &&
                               far->connectionState() == QStringLiteral("ended") &&
                               far->activity().contains(QStringLiteral("Reconnecting in"));
                    },
                    10000),
                "the first connection drops");
        // Hold the pending retry while a real QSaveFile failure rolls the
        // discard back. The retry state must survive that rollback with the card.
        const auto held_registry = QDir(root).filePath(QStringLiteral("workspace-held.json"));
        require(QFile::rename(options.storagePath, held_registry),
                "move complete reconnect registry bytes");
        const auto restore_registry = qScopeGuard([&] {
            if (QFileInfo::exists(held_registry)) {
                QDir(options.storagePath).removeRecursively();
                QFile::rename(held_registry, options.storagePath);
            }
        });
        require(QDir().mkpath(options.storagePath), "replace the registry path with a directory");
        workspace.clearError();
        require(!workspace.closeSession(id, true),
                "a failed discard during a pending retry reports failure");
        require(workspace.workspaceError().contains(QStringLiteral("Cannot save workspace:")) &&
                    workspace.session(id) == far,
                "a failed discard retains the reconnecting card");
        require(QDir(options.storagePath).removeRecursively() &&
                    QFile::rename(held_registry, options.storagePath),
                "restore complete reconnect registry bytes");
        require(waitFor(
                    [&] {
                        return calls(QStringLiteral("devbox")) >= 2 &&
                               call(QStringLiteral("devbox"), 2) == first;
                    },
                    10000),
                "a dropped connection reconnects after a failed discard");
        workspace.setReconnectTimingForTesting(
            {.first_hold = std::chrono::milliseconds(300), .wait = std::chrono::milliseconds(100)});
        require(waitFor(
                    [&] {
                        return far->connectionState() == QStringLiteral("ended") &&
                               far->activity() == QStringLiteral("Process exited (1)");
                    },
                    10000),
                "the reconnected agent ends by itself");
        QElapsedTimer settle;
        settle.start();
        waitFor([&] { return settle.elapsed() > 800; }, 2000);
        require(calls(QStringLiteral("devbox")) == 2, "an agent that ended itself stays ended");
        require(workspace.restartAgent(id) &&
                    waitFor([&] { return calls(QStringLiteral("devbox")) >= 3; }, 10000) &&
                    call(QStringLiteral("devbox"), 3) == first,
                "a restart resumes the same conversation");
        require(
            waitFor(
                [far] { return screenText(far->snapshot()).contains(QStringLiteral("connected")); },
                10000),
            "the restarted agent is connected");
        workspace.selectSession(id);
        const auto split = workspace.splitAgent(QStringLiteral("right"));
        require(!split.isEmpty() &&
                    waitFor([&] { return calls(QStringLiteral("devbox")) >= 4; }, 10000),
                "a split starts the same CLI beside it");
        const auto fourth = call(QStringLiteral("devbox"), 4);
        require(!conversation(fourth).isEmpty() && conversation(fourth) != conversation(first),
                "as a new conversation of its own");
        // Instrumented event delivery can stretch a 300 ms hold, so prove the
        // never-held policy with a test-only wide threshold and fast retries.
        workspace.setReconnectTimingForTesting(
            {.first_hold = std::chrono::seconds(5), .wait = std::chrono::milliseconds(20)});
        require(workspace.createAgent(QStringLiteral("~"), QStringLiteral("typo"),
                                      QStringLiteral("claude"), {}, {}, QStringLiteral("typo")),
                "an agent on a machine that never answers");
        lapis::desktop::SessionPreview* typo = nullptr;
        for (const auto& listed : workspace.sessions())
            if (auto* item = listed.value<lapis::desktop::SessionPreview*>();
                item != nullptr && item->title() == QStringLiteral("typo"))
                typo = item;
        require(typo != nullptr, "the agent is listed");
        // It ends before or just after the window attaches, depending on timing.
        require(waitFor(
                    [&] {
                        return calls(QStringLiteral("typo")) >= 1 &&
                               (typo->connectionState() == QStringLiteral("ended") ||
                                typo->connectionState() == QStringLiteral("disconnected"));
                    },
                    10000),
                "its connection fails");
        settle.restart();
        waitFor([&] { return settle.elapsed() >= 300; }, 1000);
        require(calls(QStringLiteral("typo")) == 1, "a connection that never worked stays ended");
        // The unreachable one is abandoned; the others end.
        for (const auto& closing : {older_id, id, split, typo->sessionId()})
            require(workspace.closeSession(closing, true), "close the stand-in agents");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in agents close");
    }
    qputenv("PATH", path);
}

// Reload ends an agent's CLI and starts it again in its tab, for one tab, the
// category or every agent; an agent on another machine whose conversation
// lapis cannot name is left running. The stand-in CLI counts its starts.
void reloadStartsAgentsAgain() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-reload-XXXXXX"));
    require(directory.isValid(), "reload directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    writeExecutable(root.filePath(QStringLiteral("bin/grok")),
                    "#!/bin/sh\necho start >> \"$(dirname \"$0\")/starts\"\n"
                    "echo \"grok ready\"\nexec sleep 600\n");
    writeExecutable(root.filePath(QStringLiteral("bin/ssh")), "#!/bin/sh\nexec sleep 600\n");
    QFile config(root.filePath(QStringLiteral("ssh_config")));
    require(config.open(QIODevice::WriteOnly), "write an ssh config");
    config.write("Host devbox\n");
    config.close();
    const auto starts = [&root] {
        QFile file(root.filePath(QStringLiteral("bin/starts")));
        return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
    };
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        workspace.setSshConfigForTesting(config.fileName());
        const auto ready = [&workspace](const QString& id) {
            const auto* item = workspace.session(id);
            return item != nullptr && item->inputReady();
        };
        const auto project = root.filePath(QStringLiteral("project"));
        require(workspace.createAgent(project, QStringLiteral("one"), QStringLiteral("grok")),
                "a first agent");
        const auto one = workspace.focusedSession()->sessionId();
        require(workspace.createAgent(project, QStringLiteral("two"), QStringLiteral("grok")),
                "a second agent");
        const auto two = workspace.focusedSession()->sessionId();
        require(waitFor([&] { return starts() == 2 && ready(one) && ready(two); }, 10000),
                "both run");
        require(workspace.reloadAgent(one) == 1 &&
                    waitFor([&] { return starts() == 3 && ready(one); }, 10000),
                "reloading a tab starts its CLI again");
        require(ready(two), "the other agent keeps running");
        require(workspace.reloadCategory() == 2 &&
                    waitFor([&] { return starts() == 5 && ready(one) && ready(two); }, 10000),
                "reloading the category starts both again");
        require(workspace.createAgent(QStringLiteral("~/far"), QStringLiteral("far"),
                                      QStringLiteral("grok"), {}, {}, QStringLiteral("devbox")),
                "an agent on another machine");
        const auto far = workspace.focusedSession()->sessionId();
        require(waitFor([&] { return ready(far); }, 10000), "it runs");
        require(workspace.reloadAgent(far) == 0 &&
                    workspace.workspaceError().contains(QStringLiteral("/resume")) && ready(far),
                "an agent whose conversation lapis cannot name is left running");
        workspace.clearError();
        require(workspace.reloadAll() == 2 &&
                    waitFor([&] { return starts() == 7 && ready(one) && ready(two); }, 10000),
                "reloading the window starts the rest again");
        for (const auto& closing : {one, two, far})
            require(workspace.closeSession(closing, true), "close the stand-in agents");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in agents close");
    }
    qputenv("PATH", path);
}

// A CLI starts at the stage's grid rather than being resized just after it
// drew, which the classic renderers cannot redraw from.
void agentsStartAtTheStageSize() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-size-XXXXXX"));
    require(directory.isValid(), "size directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    writeExecutable(root.filePath(QStringLiteral("bin/grok")),
                    "#!/bin/sh\necho \"size $(stty size)\"\nexec sleep 600\n");
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        workspace.setLaunchSize(QSize(91, 27));
        require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                      QStringLiteral("sized"), QStringLiteral("grok")),
                "an agent");
        auto* item = workspace.focusedSession();
        require(
            item != nullptr &&
                waitFor(
                    [item] {
                        return screenText(item->snapshot()).contains(QStringLiteral("size 27 91"));
                    },
                    10000),
            "its CLI starts at the stage's grid");
        require(workspace.closeSession(item->sessionId(), true), "close it");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "it closes");
    }
    qputenv("PATH", path);
}

// Claude Code plans: a session takes its machine's own sign-in, a new one
// takes a plan with room when that is full, a remote session moves once its
// plan fills and its output is quiet, reading the kept token on that machine,
// and a switch can be asked for. Stand-ins say which plan they run on.
void plansFollowTheirLoad() {
    using lapis::desktop::account_home_name;
    using lapis::desktop::account_load_key;
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-plans-XXXXXX"));
    require(directory.isValid(), "plans directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    writeExecutable(root.filePath(QStringLiteral("bin/claude")),
                    "#!/bin/sh\necho \"plan ${CLAUDE_CODE_OAUTH_TOKEN:-own}\"\nexec sleep 600\n");
    writeExecutable(root.filePath(QStringLiteral("bin/ssh")), R"(#!/bin/sh
d=$(dirname "$0"); host=$(printf '%s\n' "$@" | sed -n '/^-t$/{n;p;q;}')
n=$(($(cat "$d/$host.count" 2>/dev/null || echo 0) + 1))
printf '%s\n' "$@" > "$d/$host.call$n"
echo $n > "$d/$host.count"
for i in 1 2 3 4 5 6 7 8; do echo "connected $i"; sleep 0.05; done
exec sleep 600
)");
    QFile config(root.filePath(QStringLiteral("ssh_config")));
    require(config.open(QIODevice::WriteOnly), "write an ssh config");
    config.write("Host devbox\n");
    config.close();
    require(root.mkpath(QStringLiteral("accounts/claude")), "a credentials folder");
    QFile token(root.filePath(QStringLiteral("accounts/claude/spare.token")));
    require(token.open(QIODevice::WriteOnly), "write a token");
    token.write("token-for-spare\n");
    token.close();
    const auto calls = [&root](const QString& host) {
        QFile count(root.filePath(QStringLiteral("bin/%1.count").arg(host)));
        return count.open(QIODevice::ReadOnly) ? count.readAll().trimmed().toInt() : 0;
    };
    const auto call = [&root](const QString& host, int number) {
        QFile file(root.filePath(QStringLiteral("bin/%1.call%2").arg(host).arg(number)));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    const auto shows = [](lapis::desktop::SessionPreview* item, const char* text) {
        return waitFor(
            [item, text] {
                return screenText(item->snapshot()).contains(QString::fromLatin1(text));
            },
            10000);
    };
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.accounts = lapis::desktop::parse_accounts(QJsonDocument::fromJson(R"({"claude": [
            {"name": "mine", "email": "me@example.com", "home": "local"},
            {"name": "dev", "email": "dev@example.com", "home": "devbox"},
            {"name": "spare", "email": "spare@example.com", "machines": ["local", "devbox"]}]})")
                                                          .object());
    const auto claude = QStringLiteral("claude");
    const auto mine_full = std::pair{account_load_key(claude, QStringLiteral("me@example.com")),
                                     lapis::desktop::AccountLoad{97, 40}};
    {
        Workspace workspace(WorkspaceMode::live, options);
        workspace.setSshConfigForTesting(config.fileName());
        workspace.setAccountsRootForTesting(root.filePath(QStringLiteral("accounts")));
        const auto project = root.filePath(QStringLiteral("project"));
        require(workspace.createAgent(project, QStringLiteral("here"), claude), "a Claude agent");
        auto* here = workspace.focusedSession();
        require(here != nullptr && shows(here, "plan own") &&
                    workspace.agentAccount(here->sessionId()) == QStringLiteral("mine"),
                "it runs on this Mac's own sign-in");
        require(workspace.agentPlanCredential(here->sessionId()).isEmpty(),
                "an own sign-in has no visiting credential override");
        require(workspace.createAgent(QStringLiteral("~/far"), QStringLiteral("far"), claude, {},
                                      {}, QStringLiteral("devbox")),
                "a Claude agent on another machine");
        auto* far = workspace.focusedSession();
        require(far != nullptr, "remote agent exists");
        far->setOutputTimingForTesting({.settle_ms = 0, .burst_ms = 1500, .quiet_ms = 100});
        const auto far_id = far->sessionId();
        require(waitFor([&] { return calls(QStringLiteral("devbox")) >= 1; }, 10000) &&
                    !call(QStringLiteral("devbox"), 1).contains(QStringLiteral("{ a=")) &&
                    workspace.agentAccount(far_id) == QStringLiteral("dev"),
                "it runs on that machine's own sign-in");

        require(QFile::rename(token.fileName(), token.fileName() + ".saved"), "hide kept token");
        require(!workspace.switchAccount(here->sessionId()) && !here->closing() &&
                    here->inputReady() &&
                    workspace.agentAccount(here->sessionId()) == QStringLiteral("mine") &&
                    workspace.workspaceError().contains(QStringLiteral("no usable token")),
                "failed account preparation leaves the running agent on its original plan");
        require(QFile::rename(token.fileName() + ".saved", token.fileName()), "restore kept token");
        workspace.setAccountLoads({mine_full});
        require(workspace.createAgent(project, QStringLiteral("next"), claude),
                "another Claude agent");
        auto* next = workspace.focusedSession();
        require(next != nullptr && shows(next, "plan token-for-spare") &&
                    workspace.agentAccount(next->sessionId()) == QStringLiteral("spare"),
                "a new session takes the plan with room, its token in the environment");
        require(workspace.agentPlanCredential(next->sessionId()) == token.fileName(),
                "reset and launch use the same configured credential root");

        require(waitFor([&] { return far->inputReady(); }, 10000), "remote input is ready");
        // Drive the output estimate explicitly: instrumented transports may
        // coalesce the stand-in's startup frames into fewer than three updates.
        const auto output = far->snapshot();
        for (int frame = 0; frame < 3; ++frame)
            far->applySnapshot(output);
        require(waitFor([&] { return far->statusLabel() == QStringLiteral("Quiet"); }, 10000),
                "the remote stand-in becomes output-quiet without an observer");
        const QHash<QString, lapis::desktop::AccountLoad> full = {
            mine_full,
            {account_load_key(claude, account_home_name(QStringLiteral("devbox"))), {99, 99}}};
        workspace.setAccountLoads(full);
        require(!far->closing() && workspace.agentAccount(far_id) == QStringLiteral("dev"),
                "output quiet is not permission to interrupt a remote turn");
        // Synthetic observer snapshots exercise the account policy; they do not
        // claim that the SSH transport supplies an observer.
        lapis::session::wire::AttentionSnapshot observed;
        observed.available = true;
        observed.connected = true;
        observed.ready = true;
        for (const auto activity : {lapis::session::attention::Activity::unknown,
                                    lapis::session::attention::Activity::working}) {
            observed.activity = activity;
            far->applyAttention(observed);
            workspace.setAccountLoads(full);
            require(!far->closing() && workspace.agentAccount(far_id) == QStringLiteral("dev"),
                    "unknown and working observers retain the running plan");
        }
        observed.activity = lapis::session::attention::Activity::idle;
        observed.ready = false;
        far->applyAttention(observed);
        workspace.setAccountLoads(full);
        require(!far->closing(), "an unreconciled idle observation cannot switch plans");
        observed.ready = true;
        far->applyAttention(observed);
        workspace.setAccountLoads(full);
        require(waitFor([&] { return calls(QStringLiteral("devbox")) >= 2; }, 10000),
                "a reconciled idle observation permits switching a full plan");
        const auto first = call(QStringLiteral("devbox"), 1);
        const auto second = call(QStringLiteral("devbox"), 2);
        static const QRegularExpression conversation(
            QStringLiteral(R"( && s=([0-9a-f-]{36}) && )"));
        require(second.contains(
                    QStringLiteral(R"({ a=spare; t="$HOME/.lapis/accounts/claude/$a.token"; )")) &&
                    !second.contains(QStringLiteral("token-for-spare")) &&
                    conversation.match(second).captured(1) ==
                        conversation.match(first).captured(1) &&
                    workspace.agentAccount(far_id) == QStringLiteral("spare"),
                "it reads the kept token there and resumes the same conversation");
        require(workspace.agentPlanCredential(far_id) ==
                    QStringLiteral("~/.lapis/accounts/claude/spare.token"),
                "remote credential lookup retains the remote user's home");

        workspace.setAccountLoads({});
        require(
            workspace.canSwitchAccount(next->sessionId()) &&
                workspace.switchAccount(next->sessionId()) &&
                waitFor(
                    [&] {
                        return workspace.agentAccount(next->sessionId()) ==
                                   QStringLiteral("mine") &&
                               screenText(next->snapshot()).contains(QStringLiteral("plan own"));
                    },
                    10000),
            "a switch asked for moves the agent to the next plan with room");
        for (const auto& closing : {here->sessionId(), far_id, next->sessionId()})
            require(workspace.closeSession(closing, true), "close the stand-in agents");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in agents close");
    }
    qputenv("PATH", path);
}

// A selected visiting plan is fail-closed on a remote route before lapis
// launches or replaces anything. Saved-agent registries and explicit restart
// exercise the apply seam, and a conservative running-service endpoint
// exercises reload; a real healthy transport kept alive through termination
// remains parent-owned.
void remoteAccountsDropPreambleWithoutConfiguration() {
    // Reuses the saved-record restart seam from remote-account refusals while
    // avoiding the later live reload listener.
    QTemporaryDir directory(QCoreApplication::applicationDirPath() +
                            QStringLiteral("/../../ra-XXXXXX"));
    require(directory.isValid(), "remote account registry directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    require(root.mkpath(QStringLiteral("project")) && root.mkpath(QStringLiteral("bin")),
            "create the stale-plan fixture");
    const auto ssh = root.filePath(QStringLiteral("bin/ssh"));
    writeExecutable(ssh, "#!/bin/sh\nexit 0\n");
    const auto project = root.filePath(QStringLiteral("project"));
    const auto preamble = QStringLiteral("{ a=spare; t=\"$HOME/.lapis/accounts/claude/$a.token\"; "
                                         "export CLAUDE_CODE_OAUTH_TOKEN; true; } && ");
    const auto conversation = uuid();
    const auto base_command =
        QStringLiteral("cd %1 && s=%2 && exec claude").arg(project, conversation);
    const auto saved_command =
        QStringLiteral("cd %1 && %2s=%3 && exec claude").arg(project, preamble, conversation);
    const auto id = uuid();
    auto record = agentRecord(root.path(), id, "general");
    record.insert(QStringLiteral("endpoint"), root.filePath(id + QStringLiteral(".sock")));
    record.insert(QStringLiteral("program"), ssh);
    record.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    record.insert(QStringLiteral("account"), QStringLiteral("spare"));
    record.insert(QStringLiteral("arguments"),
                  QJsonArray{QStringLiteral("-t"), QStringLiteral("box"), saved_command});
    writeRegistry(root.filePath(QStringLiteral("workspace.json")),
                  {{"version", 2},
                   {"activeCategory", "general"},
                   {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                   {"agents", QJsonArray{record}}});
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.accounts = {};
    Workspace workspace(WorkspaceMode::live, options);
    auto* item = workspace.focusedSession();
    require(item != nullptr && !item->closing(), "the unconfigured workspace loads its agent");
    require(workspace.restartAgent(id), "restart proceeds with no plan configured");
    const auto saved_record = QJsonDocument::fromJson(readRegistry(options.storagePath))
                                  .object()
                                  .value(QStringLiteral("agents"))
                                  .toArray()
                                  .first()
                                  .toObject();
    require(workspace.agentAccount(id).isEmpty() &&
                saved_record.value(QStringLiteral("account")).toString().isEmpty() &&
                saved_record.value(QStringLiteral("arguments")).toArray().last().toString() ==
                    base_command &&
                !QJsonDocument(saved_record).toJson(QJsonDocument::Compact).contains("{ a=spare"),
            "a restart without configured plans drops the saved remote preamble and plan");
}

void remoteAccountsRefuseBeforeReplacement() {
    QJsonArray claude_names{
        QJsonObject{{"name", "safe.name-1"},
                    {"email", "safe@example.test"},
                    {"machines", QJsonArray{"box-1"}}},
        QJsonObject{{"name", "; rm -rf ~"}, {"email", "semicolon@example.test"}},
        QJsonObject{{"name", "$(danger)"}, {"email", "substitution@example.test"}},
        QJsonObject{{"name", "`danger`"}, {"email", "backtick@example.test"}},
        QJsonObject{{"name", "danger\nexit"}, {"email", "newline@example.test"}},
        QJsonObject{{"name", "*"}, {"email", "glob@example.test"}},
        QJsonObject{{"name", ".."}, {"email", "parent@example.test"}}};
    QJsonArray codex_names{QJsonObject{{"name", "quote"}, {"email", "quoted@example.test"}},
                           QJsonObject{{"name", "\"quote"}, {"email", "broken@example.test"}}};
    const auto configured = lapis::desktop::parse_accounts(
        QJsonObject{{"claude", claude_names}, {"codex", codex_names}});
    static const QRegularExpression safe_name(QStringLiteral(R"(^[A-Za-z0-9._-]{1,64}$)"));
    require(configured.accounts.size() == 2 &&
                configured.accounts.front().name == QStringLiteral("safe.name-1") &&
                configured.accounts.back().name == QStringLiteral("quote") &&
                std::all_of(configured.accounts.cbegin(), configured.accounts.cend(),
                            [](const lapis::desktop::Account& account) {
                                return safe_name.match(account.name).hasMatch();
                            }),
            "configured plan names reject shell metacharacters, traversal, and length abuse");

    QTemporaryDir directory;
    require(directory.isValid(), "remote account registry directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    require(root.mkpath(QStringLiteral("project")) && root.mkpath(QStringLiteral("bin")) &&
                root.mkpath(QStringLiteral("accounts/claude")) &&
                root.mkpath(QStringLiteral("accounts/codex/spare")),
            "create the remote-account fixture");
    const auto ssh = root.filePath(QStringLiteral("bin/ssh"));
    writeExecutable(ssh, "#!/bin/sh\nexit 0\n");
    const auto token = root.filePath(QStringLiteral("accounts/claude/spare.token"));
    const auto auth = root.filePath(QStringLiteral("accounts/codex/spare/auth.json"));
    const auto writeToken = [&](const QByteArray& value) {
        QFile file(token);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write Claude token");
        require(file.write(value) == value.size(), "write the Claude token body");
    };
    const auto writeAuth = [&](const QByteArray& value) {
        QFile file(auth);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write Codex login");
        require(file.write(value) == value.size(), "write the Codex login body");
    };
    writeToken("token-for-spare\n");
    writeAuth("{}\n");
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.accounts = lapis::desktop::parse_accounts(QJsonDocument::fromJson(R"({"claude": [
        {"name": "dev", "email": "claude-dev@example.test", "home": "box"},
        {"name": "spare", "email": "claude-spare@example.test", "machines": ["box"]}],
        "codex": [{"name": "dev", "email": "codex-dev@example.test", "home": "box"},
        {"name": "spare", "email": "codex-spare@example.test", "machines": ["box"]}]})")
                                                          .object());
    const auto reload_id = uuid();
    // Keep this filename short: QLocalServer rejects Unix paths over the
    // platform sockaddr limit, while a saved agent ID remains a UUID.
    const auto reload_endpoint = root.filePath(QStringLiteral("reload.sock"));
    // Fixture-local builder with one call site; argument order is the record's
    // own field order, and distinct wrapper types add no safety here.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const auto remoteAgent = [&](const QString& harness, const QString& command,
                                 const QString& id) {
        auto record = agentRecord(root.path(), id, "general");
        record.insert(QStringLiteral("endpoint"), root.filePath(id + QStringLiteral(".sock")));
        record.insert(QStringLiteral("program"), ssh);
        record.insert(QStringLiteral("harness"), harness);
        record.insert(QStringLiteral("account"), QStringLiteral("spare"));
        record.insert(QStringLiteral("arguments"),
                      QJsonArray{QStringLiteral("-t"), QStringLiteral("box"), command});
        return record;
    };
    const auto registry = [&](const QString& name, const QJsonObject& agent) {
        writeRegistry(
            root.filePath(name),
            {{"version", 2},
             {"activeCategory", "general"},
             {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
             {"agents", QJsonArray{agent}}});
    };
    const auto claude_command = QStringLiteral("cd %1 && s=%2 && exec claude")
                                    .arg(root.filePath(QStringLiteral("project")), uuid());
    const auto codex_command =
        QStringLiteral("cd %1 && exec codex").arg(root.filePath(QStringLiteral("project")));
    require(QFile::remove(token), "hide the Claude credential");
    auto claude_registry = options;
    claude_registry.storagePath = root.filePath(QStringLiteral("claude-workspace.json"));
    registry(QStringLiteral("claude-workspace.json"),
             remoteAgent(QStringLiteral("claude"), claude_command, uuid()));
    {
        Workspace workspace(WorkspaceMode::live, claude_registry);
        workspace.setAccountsRootForTesting(root.filePath(QStringLiteral("accounts")));
        auto* item = workspace.focusedSession();
        const bool restarted = item == nullptr ? false : workspace.restartAgent(item->sessionId());
        require(item != nullptr && !restarted && !item->closing() &&
                    workspace.agentAccount(item->sessionId()) == QStringLiteral("spare") &&
                    workspace.workspaceError() ==
                        QStringLiteral("Claude Code plan spare has no usable token."),
                "restart refuses a missing selected Claude credential before launch");
    }

    writeToken("token-for-spare\n");
    require(QFile::remove(auth), "hide the Codex credential");
    auto codex_registry = options;
    codex_registry.storagePath = root.filePath(QStringLiteral("codex-workspace.json"));
    registry(QStringLiteral("codex-workspace.json"),
             remoteAgent(QStringLiteral("codex"), codex_command, uuid()));
    {
        Workspace workspace(WorkspaceMode::live, codex_registry);
        workspace.setAccountsRootForTesting(root.filePath(QStringLiteral("accounts")));
        auto* item = workspace.focusedSession();
        require(item != nullptr && !workspace.restartAgent(item->sessionId()) && !item->closing() &&
                    workspace.agentAccount(item->sessionId()) == QStringLiteral("spare") &&
                    workspace.workspaceError() ==
                        QStringLiteral("Codex plan spare has no usable login."),
                "restart refuses a missing selected Codex credential before launch");
    }

    writeAuth("{}\n");
    auto reload_registry = claude_registry;
    reload_registry.storagePath = root.filePath(QStringLiteral("reload-workspace.json"));
    registry(QStringLiteral("reload-workspace.json"),
             remoteAgent(QStringLiteral("claude"), claude_command, reload_id));
    QLocalServer::removeServer(reload_endpoint);
    QLocalServer reload_peer;
    reload_peer.setSocketOptions(QLocalServer::UserAccessOption);
    require(reload_peer.listen(reload_endpoint), "reload endpoint listens as a service");
    {
        Workspace workspace(WorkspaceMode::live, reload_registry);
        workspace.setAccountsRootForTesting(root.filePath(QStringLiteral("accounts")));
        auto* item = workspace.focusedSession();
        require(item != nullptr && !item->closing() &&
                    workspace.agentAccount(item->sessionId()) == QStringLiteral("spare"),
                "the remote reload fixture loads");
        for (const auto& token_body : {
                 QByteArrayLiteral("\ntoken-for-spare\n"),
                 QByteArrayLiteral(" token-for-spare\n"),
                 QByteArrayLiteral("token-for-spare\nsecond\n"),
                 QByteArrayLiteral("token-for-spare\nsecond\nthird"),
                 QByteArray(8192, 'a') + '\n',
                 QByteArray(8193, 'a'),
             }) {
            writeToken(token_body);
            require(workspace.reloadAgent(item->sessionId()) == 0 && !item->closing() &&
                        workspace.workspaceError() ==
                            QStringLiteral("Claude Code plan spare has no usable token."),
                    "reload rejects a token the remote shell would reject");
            require(!reload_peer.hasPendingConnections(),
                    "credential preflight contacted the healthy session");
        }
    }
}
void incompleteCodexHomeNeverStartsAnAgent() {
    UpdaterFixture fixture("#!/bin/sh\necho started > \"${0%/*}/started\"\nexec sleep 600\n",
                           QStringLiteral("codex"));
    fixture.options.updateHarnesses = false;
    fixture.options.accounts = lapis::desktop::parse_accounts(QJsonDocument::fromJson(R"({
        "codex": [{"name":"mine", "email":"mine@example.com", "home":"local"},
                  {"name":"spare", "machines":["local"]}]
    })")
                                                                  .object());
    require(fixture.root.mkpath("accounts/codex/spare"), "create kept account home");
    QFile auth(fixture.root.filePath("accounts/codex/spare/auth.json"));
    require(auth.open(QIODevice::WriteOnly), "write fixture credential");
    auth.write("fixture credential");
    auth.close();
    const ScopedCodexHome shared(QFile::encodeName(fixture.root.filePath("missing-shared-home")));
    Workspace workspace(WorkspaceMode::live, fixture.options);
    workspace.setAccountsRootForTesting(fixture.root.filePath("accounts"));
    workspace.setAccountLoads({{lapis::desktop::account_load_key(
                                    QStringLiteral("codex"), QStringLiteral("mine@example.com")),
                                {99, 99}}});
    require(!workspace.createAgent(fixture.root.filePath("project"), QStringLiteral("blocked"),
                                   QStringLiteral("codex")) &&
                workspace.sessions().isEmpty() &&
                workspace.workspaceError().contains(QStringLiteral("Cannot prepare Codex plan")),
            "a failed shared-home preparation refuses activation with its cause");
    require(!QFileInfo::exists(fixture.root.filePath("bin/started")) &&
                auth.open(QIODevice::ReadOnly) && auth.readAll() == "fixture credential",
            "failed activation starts no child and preserves the kept credential");
}

// An attached peer can lose the terminate handshake, report replacement, or
// report ended while its endpoint still answers. None may wedge future reloads.
void reloadFailureRemainsRetryable(lapis::session::wire::StatusCode outcome) {
    namespace wire = lapis::session::wire;
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-reload-failure-XXXXXX"));
    require(directory.isValid(), "reload failure directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto id = uuid();
    auto record = agentRecord(root.path(), id, "general");
    record.insert(QStringLiteral("harness"), QStringLiteral("grok"));
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    writeRegistry(options.storagePath,
                  {{"version", 2},
                   {"activeCategory", "general"},
                   {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                   {"agents", QJsonArray{record}}});
    QLocalServer listener;
    listener.setSocketOptions(QLocalServer::UserAccessOption);
    require(listener.listen(record.value(QStringLiteral("endpoint")).toString()),
            "reload peer listens");
    const wire::SessionIdentity identity{wire::new_id(), wire::new_id()};
    const auto launch = lapis::session::validate_launch(
        {.program = QStringLiteral("/usr/bin/true"), .arguments = {}, .directory = root.path()});
    lapis::session::write_descriptor(record.value(QStringLiteral("endpoint")).toString(),
                                     lapis::session::launch_fingerprint(launch), identity);
    lapis::session::Terminal terminal({10, 5});
    int terminations = 0;
    QObject::connect(&listener, &QLocalServer::newConnection, &listener, [&] {
        while (listener.hasPendingConnections()) {
            auto* socket = listener.nextPendingConnection();
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(
                socket, &QLocalSocket::readyRead, socket,
                [&, socket, buffer = QByteArray{}]() mutable {
                    buffer += socket->readAll();
                    wire::Frame frame;
                    while (wire::take_frame(buffer, frame)) {
                        if (frame.kind == wire::Kind::attach) {
                            socket->write(wire::frame(wire::Kind::hello,
                                                      wire::encode_hello({{identity, 1}, 123})));
                            socket->write(
                                wire::frame(wire::Kind::snapshot,
                                            wire::encode_snapshot_message(
                                                {{identity, 1}, 1, terminal.snapshot()})));
                        } else if (frame.kind == wire::Kind::terminate) {
                            ++terminations;
                            if (outcome == wire::StatusCode::rejected) {
                                socket->abort();
                            } else {
                                socket->write(
                                    wire::frame(wire::Kind::status,
                                                wire::encode_status({outcome, "fixture end"})));
                                socket->flush();
                            }
                        }
                    }
                });
        }
    });
    Workspace workspace(WorkspaceMode::live, options);
    auto* item = workspace.session(id);
    require(item != nullptr, "reload fixture loads");
    require(!workspace.restartAgent(QStringLiteral("missing")), "seed an unrelated error");
    require(workspace.reloadAgent(id) == 0 &&
                workspace.workspaceError().contains(QStringLiteral("Could not request a reload")),
            "unsynchronized reload replaces stale feedback with its own failure");
    require(waitFor([&] { return item->inputReady(); }, 3000), "reload peer attaches");
    require(workspace.reloadAgent(id) == 1, "terminate is accepted");
    const auto diagnostic = outcome == wire::StatusCode::ended
                                ? QStringLiteral("did not stop in time")
                                : QStringLiteral("lost its session connection");
    require(waitFor([&] { return workspace.workspaceError().contains(diagnostic); }, 6000),
            "failed reload reports the actual recovery boundary");
    require(terminations == 1, "one terminate was sent");
    item->reconnect();
    require(waitFor([&] { return item->inputReady(); }, 3000), "the same peer reconnects");
    require(workspace.reloadAgent(id) == 1,
            "a failed reload no longer prevents a later explicit retry");
    require(waitFor([&] { return terminations == 2; }, 3000), "retry reaches the peer");
}

void reloadFailuresRemainRetryable() {
    namespace wire = lapis::session::wire;
    for (const auto outcome :
         {wire::StatusCode::rejected, wire::StatusCode::replaced, wire::StatusCode::ended})
        reloadFailureRemainsRetryable(outcome);
}

void batchReloadRetainsEarlierFailures() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-reload-batch-XXXXXX"));
    require(directory.isValid(), "batch reload directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto broken_id = uuid();
    const auto working_id = uuid();
    auto broken = agentRecord(root.path(), broken_id, "general");
    broken.insert(QStringLiteral("harness"), QStringLiteral("grok"));
    broken.insert(QStringLiteral("directory"), root.filePath(QStringLiteral("missing")));
    auto working = agentRecord(root.path(), working_id, "general");
    working.insert(QStringLiteral("harness"), QStringLiteral("grok"));
    working.insert(QStringLiteral("program"), QStringLiteral("/bin/cat"));
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    writeRegistry(options.storagePath,
                  {{"version", 2},
                   {"activeCategory", "general"},
                   {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                   {"agents", QJsonArray{broken, working}}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.reloadAll() == 1 &&
                workspace.workspaceError().contains(QStringLiteral("program or folder")),
            "a later successful restart cannot erase the earlier failure");
    auto* running = workspace.session(working_id);
    require(waitFor([&] { return running->inputReady(); }, 10000), "the valid peer starts");
    require(workspace.closeSession(working_id) &&
                waitFor([&] { return workspace.session(working_id) == nullptr; }, 10000),
            "batch fixture closes its child");
}

// Updating a tab's CLI runs the CLI's update where the agent runs, once for
// every agent that asks meanwhile, then reloads them; a failed update leaves
// them running. On another machine it goes through ssh without a terminal.
void updateReloadsAgentsAfterTheirCli() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-update-reload-XXXXXX"));
    require(directory.isValid(), "update directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    writeExecutable(root.filePath(QStringLiteral("bin/grok")),
                    "#!/bin/sh\nbin=\"$(dirname \"$0\")\"\n"
                    "if [ \"$1\" = update ]; then\n"
                    "  echo update >> \"$bin/updates\"\n"
                    "  while [ -f \"$bin/hold-update\" ]; do sleep 0.05; done\n"
                    "  if [ -f \"$bin/fail-update\" ]; then echo 'no network'; exit 3; fi\n"
                    "  exit 0\n"
                    "fi\n"
                    "echo start >> \"$bin/starts\"\necho \"grok ready\"\nexec sleep 600\n");
    writeExecutable(root.filePath(QStringLiteral("bin/ssh")),
                    "#!/bin/sh\nfor a; do if [ \"$a\" = -T ]; then\n"
                    "  printf '%s\\n' \"$*\" >> \"$(dirname \"$0\")/remote-updates\"; exit 0\n"
                    "fi; done\nexec sleep 600\n");
    QFile config(root.filePath(QStringLiteral("ssh_config")));
    require(config.open(QIODevice::WriteOnly), "write an ssh config");
    config.write("Host devbox\n");
    config.close();
    const auto lines = [&root](const QString& name) {
        QFile file(root.filePath(QStringLiteral("bin/") + name));
        return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
    };
    const auto flag = [&root](const QString& name, bool on) {
        QFile file(root.filePath(QStringLiteral("bin/") + name));
        require(on ? file.open(QIODevice::WriteOnly) : !file.exists() || file.remove(),
                "set a stand-in flag");
    };
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        workspace.setSshConfigForTesting(config.fileName());
        const auto ready = [&workspace](const QString& id) {
            const auto* item = workspace.session(id);
            return item != nullptr && item->inputReady();
        };
        const auto label = [&workspace](const QString& id) {
            const auto* item = workspace.session(id);
            require(item != nullptr, "updated session retains its identity");
            return item->statusLabel();
        };
        const auto project = root.filePath(QStringLiteral("project"));
        require(workspace.createAgent(project, QStringLiteral("one"), QStringLiteral("grok")),
                "a first agent");
        const auto one = workspace.focusedSession()->sessionId();
        require(workspace.createAgent(project, QStringLiteral("two"), QStringLiteral("grok")),
                "a second agent");
        const auto two = workspace.focusedSession()->sessionId();
        require(waitFor([&] { return lines("starts") == 2 && ready(one) && ready(two); }, 10000),
                "both run");
        require(workspace.canUpdateAgent(one), "Grok has an update command");

        flag(QStringLiteral("hold-update"), true);
        require(workspace.updateAndReloadAgent(one) == 1 &&
                    workspace.updateAndReloadAgent(two) == 1 &&
                    waitFor([&] { return lines("updates") == 1; }, 10000),
                "both tabs wait on one update");
        require(workspace.updateAndReloadAgent(one) == 1 && workspace.workspaceError().isEmpty(),
                "repeated update enrollment is accepted without a false error");
        require(label(one) == QStringLiteral("Updating Grok…") &&
                    label(two) == QStringLiteral("Updating Grok…") && ready(one),
                "they say so and keep running meanwhile");
        flag(QStringLiteral("hold-update"), false);
        require(waitFor([&] { return lines("starts") == 4 && ready(one) && ready(two); }, 10000),
                "both reload once it finishes");
        require(lines("updates") == 1 && label(one) != QStringLiteral("Updating Grok…"),
                "after a single update");

        flag(QStringLiteral("fail-update"), true);
        require(workspace.updateAndReloadAgent(one) == 1 &&
                    waitFor(
                        [&] {
                            return workspace.workspaceError().contains(
                                       QStringLiteral("did not update")) &&
                                   workspace.workspaceError().contains(
                                       QStringLiteral("no network"));
                        },
                        10000),
                "a failed update says why");
        require(lines("starts") == 4 && ready(one) &&
                    label(one) != QStringLiteral("Updating Grok…"),
                "and leaves the agent running");
        flag(QStringLiteral("fail-update"), false);
        workspace.clearError();

        require(workspace.createAgent(QStringLiteral("~/far"), QStringLiteral("far"),
                                      QStringLiteral("grok"), {}, {}, QStringLiteral("devbox")),
                "an agent on another machine");
        const auto far = workspace.focusedSession()->sessionId();
        require(waitFor([&] { return ready(far); }, 10000), "it runs");
        require(workspace.updateAndReloadAgent(far) == 1 &&
                    waitFor([&] { return lines("remote-updates") == 1; }, 10000),
                "its update goes to its machine");
        QFile remote(root.filePath(QStringLiteral("bin/remote-updates")));
        require(remote.open(QIODevice::ReadOnly), "read the remote update");
        const auto sent = QString::fromUtf8(remote.readAll());
        require(sent.contains(QStringLiteral("BatchMode=yes")) &&
                    sent.contains(QStringLiteral("ControlPath=none")) &&
                    sent.contains(QStringLiteral("-- devbox")) &&
                    sent.contains(QStringLiteral("-lic 'grok update'")),
                "through its own ssh connection, in a login shell, without prompts");
        require(
            waitFor([&] { return workspace.workspaceError().contains(QStringLiteral("/resume")); },
                    10000) &&
                ready(far),
            "an agent whose conversation lapis cannot name is updated but left running");
        workspace.clearError();
        require(workspace.updateClaudeAndReload() == 0 &&
                    workspace.workspaceError().contains(QStringLiteral("No Claude Code agent")),
                "updating Claude Code needs a Claude Code agent");
        for (const auto& closing : {one, two, far})
            require(workspace.closeSession(closing, true), "close the stand-in agents");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in agents close");
    }
    qputenv("PATH", path);
}

// Manual and startup consumers share one installer, whichever arrives first.
void startupAndManualUpdatesShareOneInstaller() {
    for (const bool manual_first : {false, true}) {
        for (const bool failed : {false, true}) {
            UpdaterFixture fixture(R"(#!/usr/bin/env bash
bin="${0%/*}"
if [ "$1" = update ]; then
    echo update >> "$bin/updates"
    while [ -f "$bin/hold" ]; do sleep 0.05; done
    [ -f "$bin/fail" ] && exit 3
    exit 0
fi
echo start >> "$bin/starts"
echo ready
exec sleep 600
)");
            fixture.options.updateHarnesses = !manual_first;
            const auto flag = [&](const char* name) {
                QFile file(fixture.root.filePath(QStringLiteral("bin/") + QLatin1String(name)));
                require(file.open(QIODevice::WriteOnly), "create updater flag");
            };
            flag("hold");
            if (failed)
                flag("fail");
            Workspace workspace(WorkspaceMode::live, fixture.options);
            fixture.create(workspace);
            auto* one = workspace.focusedSession();
            const auto first_id = one->sessionId();
            if (manual_first)
                require(waitFor([&] { return one->inputReady(); }, 10000),
                        "first agent is running");
            require(workspace.updateAndReloadAgent(first_id) == 1 &&
                        workspace.updateAndReloadAgent(first_id) == 1,
                    "manual request joins either kind of update idempotently");
            fixture.create(workspace);
            auto* two = workspace.focusedSession();
            const auto second_id = two->sessionId();
            require(waitFor([&] { return QFileInfo::exists(fixture.root.filePath("bin/updates")); },
                            10000),
                    "the one installer started");
            require(fixture.read("bin/updates") == "update\n" && !two->live() &&
                        (!manual_first || one->inputReady()),
                    "startup waits while an existing agent keeps running");
            require(QFile::remove(fixture.root.filePath("bin/hold")), "release installer");
            const int expected_starts = manual_first && !failed ? 3 : 2;
            require(waitFor(
                        [&] {
                            return one->inputReady() && two->inputReady() &&
                                   QFileInfo::exists(fixture.root.filePath("bin/starts")) &&
                                   fixture.read("bin/starts").count('\n') == expected_starts &&
                                   one->statusLabel() != QStringLiteral("Updating Grok…");
                        },
                        10000),
                    "each consumer starts or reloads once according to the result");
            require(fixture.read("bin/updates") == "update\n", "both paths used one installer");
            require(failed ? workspace.workspaceError().contains(QStringLiteral("did not update"))
                           : workspace.workspaceError().isEmpty(),
                    "manual callers see the updater outcome");
            for (const auto& id : {first_id, second_id})
                require(workspace.closeSession(id, true), "close shared updater consumer");
            require(waitFor([&] { return workspace.sessions().isEmpty(); }, 10000),
                    "consumers close");
        }
    }
}

void skippedClaudeUpdateReportsTheCurrentOperation() {
    UpdaterFixture fixture("#!/usr/bin/env bash\necho ready\nexec sleep 600\n", "claude");
    fixture.options.updateHarnesses = false;
    Workspace workspace(WorkspaceMode::live, fixture.options);
    fixture.create(workspace);
    auto* item = workspace.focusedSession();
    require(waitFor([&] { return item->inputReady(); }, 10000), "Claude stand-in is ready");
    require(workspace.closeSession(item->sessionId()), "Claude begins closing");
    require(!workspace.restartAgent(QStringLiteral("missing")), "set an earlier unrelated error");
    require(workspace.updateClaudeAndReload() == 0 &&
                workspace.workspaceError().contains(QStringLiteral("cannot update now")),
            "all-skipped Claude update reports its own reason");
    require(waitFor([&] { return workspace.sessions().isEmpty(); }, 10000),
            "Claude stand-in closes");
}

// A full-screen program that reports the mouse, as Claude Code's full-screen
// mode does, gets the wheel as mouse wheel events at the cell under it. The
// stand-in takes the alternate screen, reads what the wheel sends, and prints
// it once it leaves.
void wheelReachesAFullScreenProgram() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-wheel-XXXXXX"));
    require(directory.isValid(), "wheel directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    {
        QFile script(root.filePath(QStringLiteral("bin/grok")));
        require(script.open(QIODevice::WriteOnly | QIODevice::Truncate), "write the stand-in");
        script.write("#!/bin/sh\n"
                     "printf '\\033[?1049h\\033[?1000h\\033[?1006hwheel ready'\n"
                     "stty raw -echo\n"
                     "got=$(dd bs=1 count=20 2>/dev/null | od -An -c | tr -d ' \\n')\n"
                     "stty sane\n"
                     "printf '\\033[?1006l\\033[?1000l\\033[?1049l'\n"
                     "echo \"got $got\"\n"
                     "exec sleep 600\n");
    }
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                      QStringLiteral("wheel"), QStringLiteral("grok")),
                "a full-screen stand-in");
        auto* agent = workspace.focusedSession();
        require(agent != nullptr && waitFor(
                                        [agent] {
                                            return agent->inputReady() &&
                                                   agent->snapshot().accepts_wheel &&
                                                   screenText(agent->snapshot())
                                                       .contains(QStringLiteral("wheel ready"));
                                        },
                                        10000),
                "the program takes the alternate screen, and the service the wheel");
        agent->sendWheel(2, 4, 2);
        require(waitFor(
                    [agent] {
                        return screenText(agent->snapshot())
                            .contains(QStringLiteral("got 033[<64;5;3M033[<64;5;3M"));
                    },
                    10000),
                "two wheel-up events at the cell reached the program");
        require(workspace.closeSession(agent->sessionId()), "close the stand-in");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in closes");
    }
    qputenv("PATH", path);
}

// History reaches back to the first row, and the scrubber jumps anywhere in
// it: the page at the start, the middle, then live again.
void historyJumpsToTheStart() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-scrub-XXXXXX"));
    require(directory.isValid(), "scrub directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    {
        QFile script(root.filePath(QStringLiteral("bin/grok")));
        require(script.open(QIODevice::WriteOnly | QIODevice::Truncate), "write the stand-in");
        script.write("#!/bin/sh\n"
                     "i=0\n"
                     "while [ $i -lt 2000 ]; do printf 'line %04d\\n' $i; i=$((i + 1)); done\n"
                     "echo all printed\n"
                     "exec sleep 600\n");
    }
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                      QStringLiteral("scrub"), QStringLiteral("grok")),
                "a talkative stand-in");
        auto* agent = workspace.focusedSession();
        const auto shows = [agent](const QString& text) {
            return screenText(agent->snapshot()).contains(text);
        };
        require(
            agent != nullptr &&
                waitFor([&] { return agent->inputReady() && shows(QStringLiteral("all printed")); },
                        15000),
            "it prints its lines");
        const auto settled = [agent] {
            return agent->historyActive() && !agent->historyRequestPending();
        };
        // History scrolls by rows as one strip: three rows back, the three
        // kept lines above the screen, then the screen moved down, whole.
        const auto live = agent->snapshot();
        const auto live_rows = screenText(live).split(QLatin1Char('\n'));
        agent->scrollHistory(3);
        require(waitFor(settled, 10000), "three rows back");
        const auto view = agent->snapshot();
        const auto rows = screenText(view).split(QLatin1Char('\n'));
        const auto number = [](const QString& row) {
            return row.trimmed().startsWith(QStringLiteral("line "))
                       ? row.trimmed().mid(5, 4).toInt()
                       : -1;
        };
        require(view.size == live.size &&
                    rows.mid(3, live.size.rows - 3) == live_rows.mid(0, live.size.rows - 3),
                "the screen moves down three rows, and the view is a whole screen");
        require(number(rows[0]) >= 0 && number(rows[1]) == number(rows[0]) + 1 &&
                    number(rows[2]) == number(rows[1]) + 1 &&
                    (number(live_rows[0]) < 0 || number(live_rows[0]) == number(rows[2]) + 1),
                "above it, the three kept lines just before the screen");
        agent->scrollHistory(-3);
        require(!agent->historyActive() && shows(QStringLiteral("all printed")),
                "three rows forward is live again");
        agent->olderHistory();
        require(waitFor(settled, 10000) && agent->historyScrubbable(),
                "the newest page says where it sits");
        agent->historyAt(0);
        require(waitFor([&] { return settled() && shows(QStringLiteral("line 0000")); }, 10000) &&
                    agent->historyPosition() < 0.001,
                "the scrubber reaches the first line");
        agent->historyAt(0.5);
        require(waitFor([&] { return settled() && agent->historyPosition() > 0.4; }, 10000) &&
                    agent->historyPosition() < 0.6 &&
                    (shows(QStringLiteral("line 09")) || shows(QStringLiteral("line 10"))),
                "and the middle");
        agent->returnToLive();
        require(waitFor([&] { return shows(QStringLiteral("all printed")); }, 5000),
                "and live again");
        require(workspace.closeSession(agent->sessionId()), "close the stand-in");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in closes");
    }
    qputenv("PATH", path);
}

// Screens are decoded for the views showing them: an agent nobody is looking
// at keeps only its newest screen, encoded, until someone reads it; the stage
// decodes each one, a preview at most every 250 ms.
void unseenAgentsDecodeNothing() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-unseen-XXXXXX"));
    require(directory.isValid(), "unseen directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    {
        QFile script(root.filePath(QStringLiteral("bin/grok")));
        require(script.open(QIODevice::WriteOnly | QIODevice::Truncate), "write the stand-in");
        script.write(
            "#!/bin/sh\n"
            "echo ready\n"
            "while read round; do\n"
            "  i=0; while [ $i -lt 30 ]; do echo \"$round $i\"; i=$((i + 1)); sleep 0.03; done\n"
            "  echo \"done $round\"\n"
            "done\n");
    }
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                      QStringLiteral("unseen"), QStringLiteral("grok")),
                "a stand-in that prints on request");
        auto* agent = workspace.focusedSession();
        const auto shows = [agent](const QString& text) {
            return screenText(agent->snapshot()).contains(text);
        };
        require(agent != nullptr &&
                    waitFor([&] { return agent->inputReady() && shows(QStringLiteral("ready")); },
                            10000),
                "it starts");
        const auto idle = [](int milliseconds) {
            static_cast<void>(waitFor([] { return false; }, milliseconds));
        };
        const auto before = agent->decodedScreens();
        agent->sendText("one\n");
        idle(2500);
        require(agent->decodedScreens() == before, "nobody is looking: nothing is decoded");
        require(shows(QStringLiteral("done one")) && agent->decodedScreens() == before + 1,
                "reading the screen decodes the newest once");
        agent->addViewer(0);
        const auto staged = agent->decodedScreens();
        agent->sendText("two\n");
        idle(2500);
        require(agent->decodedScreens() > staged + 5, "the stage decodes each screen");
        agent->removeViewer(0);
        agent->addViewer(250);
        const auto previewed = agent->decodedScreens();
        agent->sendText("three\n");
        idle(2500);
        const auto decoded = agent->decodedScreens() - previewed;
        require(decoded >= 1 && decoded <= 12, "a preview decodes at most every 250 ms");
        agent->removeViewer(250);
        require(workspace.closeSession(agent->sessionId()), "close the stand-in");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in closes");
    }
    qputenv("PATH", path);
}

// Quick-command terminals: one shell per machine under its own service, apart
// from agents. It is reused, reattached by the next lapis, started from the
// phone through the control socket, and leaves when its shell exits.
void terminalsRunPlainShells() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-terminals-XXXXXX"));
    require(directory.isValid(), "terminals directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto config = root.filePath(QStringLiteral("ssh_config"));
    {
        QFile ssh(config);
        require(ssh.open(QIODevice::WriteOnly), "write the ssh config");
        ssh.write(QStringLiteral("Host devbox build-*\n  HostName 10.0.0.2\nHost *\n"
                                 "  ServerAliveInterval 30\nInclude %1\n")
                      .arg(QDir(root).filePath(QStringLiteral("extra.conf")))
                      .toUtf8());
        QFile extra(root.filePath(QStringLiteral("extra.conf")));
        require(extra.open(QIODevice::WriteOnly), "write the included config");
        extra.write("Host = gpu devbox\n");
    }
    const auto hosts = lapis::desktop::ssh_config_hosts(config);
    // Include resolves an absolute fixture path, without a developer's ~/.ssh.
    require(hosts.contains(QStringLiteral("devbox")) && !hosts.contains(QStringLiteral("*")) &&
                !hosts.contains(QStringLiteral("build-*")) && hosts.contains(QStringLiteral("gpu")),
            "hosts come from the ssh config, without patterns");
    QFile shell(root.filePath(QStringLiteral("shell")));
    require(shell.open(QIODevice::WriteOnly), "write the stand-in shell");
    shell.write("#!/bin/sh\necho \"shell ready $*\"\nwhile read line; do\n"
                "  [ \"$line\" = exit ] && exit 0\n  echo \"ran $line\"\ndone\n");
    shell.close();
    require(shell.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make it executable");
    require(root.mkpath(QStringLiteral("project")), "create the terminal's original directory");
    QString id;
    {
        const auto original_home = qgetenv("HOME");
        const auto restore_home = qScopeGuard([&original_home] { qputenv("HOME", original_home); });
        qputenv("HOME", QFile::encodeName(root.filePath(QStringLiteral("project"))));
        lapis::desktop::Terminals terminals(root.path(), config);
        terminals.setShellForTesting(shell.fileName());
        require(!terminals.show(QStringLiteral("nowhere")) && !terminals.error().isEmpty(),
                "only this Mac and ssh config hosts");
        require(terminals.show(QString()) && terminals.current() != nullptr,
                "a terminal on this Mac");
        auto* current = terminals.current();
        id = current->sessionId();
        require(waitFor([current] { return current->inputReady(); }, 10000) &&
                    waitFor(
                        [current] {
                            return screenText(current->snapshot())
                                .contains(QStringLiteral("shell ready -l -i"));
                        },
                        5000),
                "a login shell starts at home");
        require(terminals.show(QString()) && terminals.current()->sessionId() == id,
                "the machine's terminal is reused");
        const auto machines = terminals.machines();
        require(machines.front().toMap().value(QStringLiteral("name")) ==
                        QStringLiteral("This Mac") &&
                    machines.front().toMap().value(QStringLiteral("open")).toBool(),
                "this Mac is first and has a terminal");
        QFile saved(terminals.registryPath());
        require(saved.open(QIODevice::ReadOnly) && saved.readAll().contains(id.toUtf8()),
                "terminals.json records it for the next lapis and the phone");
    }
    // The service keeps running after its original executable and working
    // directory disappear; their absence must not invalidate reattachment.
    require(QFile::rename(shell.fileName(), shell.fileName() + QStringLiteral(".moved")),
            "move the terminal's original executable");
    require(QFile::rename(root.filePath(QStringLiteral("project")),
                          root.filePath(QStringLiteral("project.moved"))),
            "move the terminal's original working directory");
    Workspace workspace(WorkspaceMode::live, [&root] {
        WorkspaceOptions options;
        options.storagePath = root.filePath(QStringLiteral("workspace.json"));
        return options;
    }());
    {
        // The service outlived that lapis; this one reattaches.
        lapis::desktop::Terminals terminals(root.path(), config);
        terminals.setShellForTesting(shell.fileName());
        terminals.restore();
        auto* again = terminals.terminal(id);
        require(again != nullptr && waitFor([again] { return again->inputReady(); }, 10000),
                "the next lapis reattaches the running shell");
        lapis::desktop::WorkspaceControl control(workspace, false);
        control.setTerminals(&terminals);
        const auto opened = askWorkspace(
            workspace.storagePath(), {{QStringLiteral("version"), 1},
                                      {QStringLiteral("request"), QStringLiteral("openTerminal")},
                                      {QStringLiteral("machine"), QStringLiteral("")}});
        require(opened.value(QStringLiteral("ok")).toBool() &&
                    opened.value(QStringLiteral("id")).toString() == id,
                "the phone gets this Mac's terminal");
        const auto closed = askWorkspace(
            workspace.storagePath(), {{QStringLiteral("version"), 1},
                                      {QStringLiteral("request"), QStringLiteral("closeTerminal")},
                                      {QStringLiteral("id"), id}});
        require(closed.value(QStringLiteral("ok")).toBool() &&
                    waitFor([&terminals, &id] { return terminals.terminal(id) == nullptr; }, 10000),
                "closing ends the shell and the terminal leaves");
        require(QFile::rename(shell.fileName() + QStringLiteral(".moved"), shell.fileName()),
                "restore the fixture executable before starting a new shell");
        require(terminals.show(QString()) && terminals.current()->sessionId() != id,
                "the next one is a fresh shell");
        auto* fresh = terminals.current();
        require(fresh != nullptr, "a fresh terminal");
        const auto fresh_id = fresh->sessionId();
        require(terminals.close(fresh_id) && terminals.terminal(fresh_id) != nullptr,
                "the first close is accepted and owns the unsynchronized terminal");
        require(waitFor([&terminals, &fresh_id] { return terminals.terminal(fresh_id) == nullptr; },
                        10000) &&
                    !terminals.machines().front().toMap().value(QStringLiteral("open")).toBool(),
                "a close requested before synchronization ends the terminal");
    }
    {
        // A saved endpoint can answer while the attach itself fails. That
        // settled failure must not leave a permanently "open" broken entry.
        const auto failed_id =
            QStringLiteral("terminal-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto endpoint = root.filePath(failed_id + QStringLiteral(".sock"));
        QLocalServer listener;
        require(QLocalServer::removeServer(endpoint) && listener.listen(endpoint),
                "listen on a failed-attach fixture endpoint");
        QFile registry(root.filePath(QStringLiteral("terminals.json")));
        require(registry.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "rewrite the failed-attach registry");
        const auto saved = QJsonObject{
            {"version", 1},
            {"terminals",
             QJsonArray{QJsonObject{{"id", failed_id},
                                    {"machine", ""},
                                    {"endpoint", endpoint},
                                    {"program", "/bin/sh"},
                                    {"arguments", QJsonArray{}},
                                    {"directory", root.filePath(QStringLiteral("absent"))}}}}};
        const auto records = QJsonDocument(saved).toJson(QJsonDocument::Compact);
        require(registry.write(records) == records.size(), "write the failed-attach registry");
        registry.close();
        lapis::desktop::Terminals terminals(root.path(), config);
        terminals.restore();
        require(terminals.terminal(failed_id) != nullptr,
                "an answering saved endpoint attaches before failure");
        require(
            waitFor([&terminals, &failed_id] { return terminals.terminal(failed_id) == nullptr; },
                    10000),
            "a failed attach removes its broken terminal");
        require(!terminals.machines().front().toMap().value(QStringLiteral("open")).toBool(),
                "a failed attach does not leave the machine falsely open");
        // A close owns a failed attachment until it can terminate. Exhausting
        // bounded recovery must still permit an explicit close retry.
        int connections = 0;
        QObject::connect(&listener, &QLocalServer::newConnection, &listener, [&] {
            while (auto* socket = listener.nextPendingConnection()) {
                ++connections;
                socket->abort();
                socket->deleteLater();
            }
        });
        require(registry.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    registry.write(records) == records.size(),
                "restore the failed-close fixture");
        registry.close();
        terminals.restore();
        require(terminals.close(failed_id), "a close during failed attachment is accepted");
        require(waitFor(
                    [&terminals] {
                        return terminals.error().contains(QStringLiteral("Could not reconnect"));
                    },
                    10000),
                "recovery reaches its bounded failure");
        const int before_retry = connections;
        require(terminals.terminal(failed_id) != nullptr && terminals.close(failed_id) &&
                    waitFor([&] { return connections > before_retry; }, 10000),
                "an explicit close retries after exhausted recovery without losing ownership");
    }
}

// Resuming a past conversation starts its CLI with the resume option, from the
// window or the phone, and the pair is lapis's to follow on a later restart.
void resumingAConversationStartsItsCli() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-resume-XXXXXX"));
    require(directory.isValid(), "resume directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    QFile script(root.filePath(QStringLiteral("bin/grok")));
    require(script.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite the stand-in CLI");
    script.write("#!/bin/sh\necho \"grok args: $*\"\nexec sleep 600\n");
    script.close();
    const auto project = root.filePath(QStringLiteral("project"));
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        const auto close_owned = qScopeGuard([&] {
            for (const auto& value : workspace.sessions())
                if (auto* item = value.value<lapis::desktop::SessionPreview*>())
                    static_cast<void>(workspace.closeSession(item->sessionId()));
            static_cast<void>(waitFor([&] { return workspace.sessions().isEmpty(); }, 10000));
        });
        require(!workspace.resumeAgent(project, QStringLiteral("x"), QStringLiteral("grok"),
                                       QStringLiteral("-rf")),
                "an option is never taken for a conversation");
        require(workspace.resumeAgent(project, QStringLiteral("project"), QStringLiteral("grok"),
                                      QStringLiteral("conv-123")),
                "a past conversation resumes, named after its folder");
        auto* agent = workspace.focusedSession();
        require(agent != nullptr && waitFor(
                                        [agent] {
                                            return screenText(agent->snapshot())
                                                .remove(QLatin1Char('\n'))
                                                .contains(QStringLiteral(
                                                    "grok args: --fullscreen --permission-mode "
                                                    "bypassPermissions -r conv-123"));
                                        },
                                        10000),
                "the CLI starts with its resume option, in the default mode");
        lapis::desktop::WorkspaceControl control(workspace, false);
        auto request = createRequest(workspace.activeCategoryId(), QStringLiteral("grok"), project);
        request.insert(QStringLiteral("resume"), QStringLiteral("conv-456"));
        const auto started = askWorkspace(workspace.storagePath(), request);
        auto* phone = workspace.session(started.value(QStringLiteral("id")).toString());
        require(phone != nullptr && waitFor(
                                        [phone] {
                                            return screenText(phone->snapshot())
                                                .remove(QLatin1Char('\n'))
                                                .contains(QStringLiteral(
                                                    "grok args: --fullscreen --permission-mode "
                                                    "bypassPermissions -r conv-456"));
                                        },
                                        10000),
                "the phone resumes a conversation too");
        QFile saved(workspace.storagePath());
        require(saved.open(QIODevice::ReadOnly), "read the registry");
        const auto agents = QJsonDocument::fromJson(saved.readAll())
                                .object()
                                .value(QStringLiteral("agents"))
                                .toArray();
        require(std::any_of(agents.begin(), agents.end(),
                            [&](const QJsonValue& entry) {
                                const auto managed =
                                    entry[QStringLiteral("managedResume")].toObject();
                                return entry[QStringLiteral("id")] == agent->sessionId() &&
                                       managed.value(QStringLiteral("identity")) ==
                                           QStringLiteral("conv-123");
                            }),
                "the resume pair is recorded as lapis's");
        // An agent named after its folder takes its conversation's title; a
        // name chosen on the Mac or the phone stays.
        const auto conversations = workspace.agentConversations();
        require(conversations.value(agent->sessionId()) == QStringLiteral("conv-123") &&
                    conversations.value(phone->sessionId()) == QStringLiteral("conv-456"),
                "each agent's conversation is known");
        require(workspace.followConversationTitle(agent->sessionId(),
                                                  QStringLiteral("  Fix   the resize bug ")) &&
                    agent->title() == QStringLiteral("Fix the resize bug"),
                "the conversation's title names the agent");
        require(
            workspace.followConversationTitle(agent->sessionId(), QStringLiteral("After /clear")) &&
                agent->title() == QStringLiteral("After /clear"),
            "it keeps following its conversation");
        const auto rocket = QString::fromUcs4(U"\U0001f680");
        const auto long_title = rocket.repeated(41);
        const auto elided_title = rocket.repeated(39) + QChar(0x2026);
        require(workspace.followConversationTitle(agent->sessionId(), long_title) &&
                    agent->title() == elided_title,
                "auto titles keep complete supplementary Unicode scalars");
        // A name from before chosen names were recorded stays.
        require(
            workspace.createAgent(project, QFileInfo(project).fileName(), QStringLiteral("grok")),
            "an agent with its own name");
        auto* own = workspace.focusedSession();
        require(own != nullptr, "the named agent is shown");
        require(
            !workspace.followConversationTitle(own->sessionId(), QFileInfo(project).fileName()) &&
                own->title() == QFileInfo(project).fileName(),
            "an explicit folder default is user-owned");
        const auto renamed = askWorkspace(
            workspace.storagePath(), {{QStringLiteral("version"), 1},
                                      {QStringLiteral("request"), QStringLiteral("renameAgent")},
                                      {QStringLiteral("id"), phone->sessionId()},
                                      {QStringLiteral("title"), QStringLiteral("Named here")}});
        require(renamed.value(QStringLiteral("ok")).toBool() &&
                    phone->title() == QStringLiteral("Named here") &&
                    !workspace.followConversationTitle(phone->sessionId(),
                                                       QStringLiteral("Their title")) &&
                    phone->title() == QStringLiteral("Named here"),
                "a name chosen on the phone stays over the conversation's title");
        {
            QFile named(workspace.storagePath());
            require(named.open(QIODevice::ReadOnly), "read the registry again");
            const auto saved_agents = QJsonDocument::fromJson(named.readAll())
                                          .object()
                                          .value(QStringLiteral("agents"))
                                          .toArray();
            require(std::any_of(saved_agents.begin(), saved_agents.end(),
                                [&](const QJsonValue& entry) {
                                    return entry[QStringLiteral("id")] == phone->sessionId() &&
                                           entry[QStringLiteral("named")].toBool() &&
                                           entry[QStringLiteral("title")] ==
                                               QStringLiteral("Named here");
                                }),
                    "the chosen name is saved as chosen");
            require(std::any_of(saved_agents.begin(), saved_agents.end(),
                                [&](const QJsonValue& entry) {
                                    return entry[QStringLiteral("id")] == own->sessionId() &&
                                           entry[QStringLiteral("named")].toBool() &&
                                           entry[QStringLiteral("title")] ==
                                               QFileInfo(project).fileName();
                                }),
                    "an explicit folder default is saved as chosen");
        }
        require(waitFor(
                    [agent, phone, own] {
                        return agent->inputReady() && phone->inputReady() && own->inputReady();
                    },
                    10000),
                "the agents take input");
        for (const auto& closing : {agent->sessionId(), phone->sessionId(), own->sessionId()})
            require(workspace.closeSession(closing), "close the stand-in agents");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in agents close");
        workspace.setHarnessArguments(
            {{QStringLiteral("grok"),
              {QStringLiteral("--"), QStringLiteral("-r"), QStringLiteral("conv-literal"),
               QStringLiteral("--permission-mode")}}});
        lapis::desktop::AgentDefaults defaults;
        defaults.mode = QStringLiteral("edits");
        workspace.setAgentDefaults(defaults);
        require(workspace.resumeAgent(project, QStringLiteral("literal"), QStringLiteral("grok"),
                                      QStringLiteral("conv-literal")),
                "a modeless resume with literal configured arguments starts");
        auto* literal = workspace.focusedSession();
        require(literal != nullptr &&
                    waitFor(
                        [literal] {
                            return literal->inputReady() &&
                                   screenText(literal->snapshot())
                                       .remove(QLatin1Char('\n'))
                                       .contains(QStringLiteral(
                                           "--permission-mode acceptEdits -r "
                                           "conv-literal -- -r conv-literal --permission-mode"));
                        },
                        10000),
                "generated mode and resume flags must precede literal prompt arguments");
        QFile registry(workspace.storagePath());
        require(registry.open(QIODevice::ReadOnly), "read managed resume provenance");
        const auto saved_launches = QJsonDocument::fromJson(registry.readAll())
                                        .object()
                                        .value(QStringLiteral("agents"))
                                        .toArray();
        require(saved_launches.size() == 1, "one resumed agent is recorded");
        const auto saved_agent = saved_launches.first().toObject();
        const auto saved_arguments = saved_agent.value(QStringLiteral("arguments")).toArray();
        const auto index = saved_agent.value(QStringLiteral("managedResume"))
                               .toObject()
                               .value(QStringLiteral("index"))
                               .toInt(-1);
        require(index >= 0 && index + 2 < saved_arguments.size() &&
                    saved_arguments.at(index) == QLatin1String("-r") &&
                    saved_arguments.at(index + 1) == QLatin1String("conv-literal") &&
                    saved_arguments.at(index + 2) == QLatin1String("--"),
                "managed resume index refers to the generated pair before the literal tail");
        require(workspace.closeSession(literal->sessionId()), "close the literal-argument fixture");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the literal-argument fixture closes");
    }
    qputenv("PATH", path);
}

// A workspace request as the phone gateway sends it.
QJsonObject askVersioned(const Workspace& workspace, QJsonObject request) {
    request.insert(QStringLiteral("version"), 1);
    return askWorkspace(workspace.storagePath(), request);
}

bool answeredOk(const QJsonObject& answer) { return answer.value(QStringLiteral("ok")).toBool(); }

struct PhoneArrangement {
    QString agent; // running, in `later`
    QString later;
    QString ideas; // empty
};

// The phone renames, orders and removes categories under the Mac's rules,
// and moves an agent to a category or a place in one, while the window keeps
// what it shows.
void phoneArrangesTheWorkspace(Workspace& workspace, const PhoneArrangement& place) {
    // A copy: the requests below change the workspace through its socket.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const auto shown = workspace.activeCategoryId();
    const auto* focused = workspace.focusedSession();
    const auto& ideas = place.ideas;
    require(answeredOk(askVersioned(workspace,
                                    {{QStringLiteral("request"), QStringLiteral("renameCategory")},
                                     {QStringLiteral("id"), ideas},
                                     {QStringLiteral("name"), QStringLiteral("Someday")}})) &&
                workspace.categories().constLast().toMap().value(QStringLiteral("name")) ==
                    QStringLiteral("Someday"),
            "the phone renames a category");
    require(answeredOk(askVersioned(workspace,
                                    {{QStringLiteral("request"), QStringLiteral("placeCategory")},
                                     {QStringLiteral("id"), ideas},
                                     {QStringLiteral("index"), 0}})) &&
                workspace.categories().constFirst().toMap().value(QStringLiteral("id")) == ideas,
            "the phone moves a category to the top");
    require(answeredOk(
                askVersioned(workspace, {{QStringLiteral("request"), QStringLiteral("placeAgent")},
                                         {QStringLiteral("id"), place.agent},
                                         {QStringLiteral("category"), ideas},
                                         {QStringLiteral("index"), 0}})) &&
                workspace.agentPlace(place.agent).value(QStringLiteral("category")) ==
                    QStringLiteral("Someday"),
            "the phone moves an agent to another category");
    const auto kept =
        askVersioned(workspace, {{QStringLiteral("request"), QStringLiteral("removeCategory")},
                                 {QStringLiteral("id"), ideas}});
    require(!answeredOk(kept) &&
                kept.value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("Move the agents out")) &&
                workspace.workspaceError().isEmpty(),
            "a category with agents stays, and the reason goes to the phone only");
    require(answeredOk(
                askVersioned(workspace, {{QStringLiteral("request"), QStringLiteral("placeAgent")},
                                         {QStringLiteral("id"), place.agent},
                                         {QStringLiteral("category"), place.later},
                                         {QStringLiteral("index"), 1024 * 1024}})) &&
                answeredOk(askVersioned(
                    workspace, {{QStringLiteral("request"), QStringLiteral("removeCategory")},
                                {QStringLiteral("id"), ideas}})),
            "an emptied category is removed");
    const auto categories = workspace.categories();
    require(std::none_of(categories.begin(), categories.end(),
                         [&ideas](const QVariant& category) {
                             return category.toMap().value(QStringLiteral("id")) == ideas;
                         }),
            "and is gone");
    const auto running =
        askVersioned(workspace, {{QStringLiteral("request"), QStringLiteral("restartAgent")},
                                 {QStringLiteral("id"), place.agent}});
    require(!answeredOk(running) && running.value(QStringLiteral("error"))
                                        .toString()
                                        .contains(QStringLiteral("still running")),
            "a running agent is not restarted");
    require(workspace.activeCategoryId() == shown && workspace.focusedSession() == focused,
            "the window keeps its category and agent");
}

// The Mac's settings that matter away from it, read and changed from the
// phone and saved to lapis.json; nothing else can be changed that way.
void phoneChangesTheMacsSettings(const Workspace& workspace,
                                 lapis::desktop::WorkspaceControl& control, const QDir& root) {
    require(!answeredOk(
                askVersioned(workspace, {{QStringLiteral("request"), QStringLiteral("settings")}})),
            "no settings without a config");
    QFile file(root.filePath(QStringLiteral("lapis.json")));
    require(file.open(QIODevice::WriteOnly) && file.write("{}\n") == 3, "write a config");
    file.close();
    lapis::desktop::KeyMap keymap;
    keymap.setSourcePathForTesting(file.fileName());
    require(keymap.load(), "the config loads");
    control.setKeyMap(&keymap);
    const auto shown =
        askVersioned(workspace, {{QStringLiteral("request"), QStringLiteral("settings")}})
            .value(QStringLiteral("settings"))
            .toObject();
    require(shown.value(QStringLiteral("keepAwake")).toBool() &&
                shown.value(QStringLiteral("alertRepeat")).toInt() == 3 &&
                shown.contains(QStringLiteral("showUsage")),
            "the phone reads the Mac's settings");
    const auto changed = askVersioned(
        workspace, {{QStringLiteral("request"), QStringLiteral("changeSettings")},
                    {QStringLiteral("settings"), QJsonObject{{QStringLiteral("keepAwake"), false},
                                                             {QStringLiteral("alertRepeat"), 5}}}});
    require(answeredOk(changed) && !keymap.keepAwake() && keymap.alertRepeat() == 5 &&
                !changed.value(QStringLiteral("settings"))
                     .toObject()
                     .value(QStringLiteral("keepAwake"))
                     .toBool(),
            "the phone changes them");
    require(file.open(QIODevice::ReadOnly) && !QJsonDocument::fromJson(file.readAll())
                                                   .object()
                                                   .value(QStringLiteral("keepAwake"))
                                                   .toBool(true),
            "and they are saved");
    file.close();
    for (const auto& bad : {QJsonObject{{QStringLiteral("keepAwake"), true},
                                        {QStringLiteral("theme"), QStringLiteral("amber")}},
                            QJsonObject{{QStringLiteral("keepAwake"), QStringLiteral("yes")}}})
        require(!answeredOk(askVersioned(
                    workspace, {{QStringLiteral("request"), QStringLiteral("changeSettings")},
                                {QStringLiteral("settings"), bad}})) &&
                    !keymap.keepAwake(),
                "an unknown or mistyped setting changes nothing");
    for (const auto& request :
         {QJsonObject{{QStringLiteral("request"), QStringLiteral("changeSettings")}},
          QJsonObject{{QStringLiteral("request"), QStringLiteral("changeSettings")},
                      {QStringLiteral("settings"), QStringLiteral("no object")}}}) {
        const auto refused = askVersioned(workspace, request);
        require(!answeredOk(refused) &&
                    refused.value(QStringLiteral("error"))
                        .toString()
                        .contains(QStringLiteral("Missing settings")) &&
                    !keymap.keepAwake(),
                "a missing or non-object settings value is refused");
    }
    for (const auto repeat : {0.0, -1.0, 0.5, 2.5, 10.5, 1e20}) {
        const auto refused = askVersioned(
            workspace,
            {{QStringLiteral("request"), QStringLiteral("changeSettings")},
             {QStringLiteral("settings"), QJsonObject{{QStringLiteral("alertRepeat"), repeat}}}});
        require(!answeredOk(refused) &&
                    refused.value(QStringLiteral("error"))
                        .toString()
                        .contains(QStringLiteral("alertRepeat")) &&
                    keymap.alertRepeat() == 5,
                "alertRepeat is a whole number from 1 through 10");
    }

    // A later save can fail even though the request validated. The owner must
    // not leave a mixed batch in memory or replace the damaged file.
    const auto before_failure = keymap.remoteSettings();
    const auto malformed_path = root.filePath(QStringLiteral("broken-lapis.json"));
    const QByteArray malformed = "{unfinished remote edit";
    {
        QSaveFile replacement(malformed_path);
        require(replacement.open(QIODevice::WriteOnly), "open the replacement");
        replacement.write(malformed);
        require(replacement.commit(), "commit the malformed replacement");
    }
    keymap.setSourcePathForTesting(malformed_path);
    const auto save_failed = askVersioned(
        workspace, {{QStringLiteral("request"), QStringLiteral("changeSettings")},
                    {QStringLiteral("settings"), QJsonObject{{QStringLiteral("keepAwake"), true},
                                                             {QStringLiteral("alertRepeat"), 3}}}});
    require(!answeredOk(save_failed) &&
                save_failed.value(QStringLiteral("settings")).toObject() ==
                    QJsonObject::fromVariantMap({{QStringLiteral("keepAwake"), false},
                                                 {QStringLiteral("alertSound"), true},
                                                 {QStringLiteral("finishSound"), true},
                                                 {QStringLiteral("alertRepeat"), 5},
                                                 {QStringLiteral("notify"), true},
                                                 {QStringLiteral("showUsage"), true}}) &&
                keymap.remoteSettings() == before_failure,
            "a failed save echoes unchanged settings and preserves memory");
    require(save_failed.value(QStringLiteral("error"))
                .toString()
                .contains(QStringLiteral("Could not save")),
            "a failed save keeps a useful diagnostic");
    QFile damaged(malformed_path);
    require(damaged.open(QIODevice::ReadOnly), "read the failed-save config");
    require(damaged.readAll() == malformed, "a failed save preserves disk bytes");
    control.setKeyMap(nullptr);
}

// The phone gateway starts an agent through the window: it opens as a new tab
// in the chosen category, while the window keeps its category and agent.
void phoneStartsAnAgentInItsCategory() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-control-XXXXXX"));
    require(directory.isValid(), "control directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    const auto project = root.filePath(QStringLiteral("project"));
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        const auto desk_category = workspace.activeCategoryId();
        require(workspace.createAgent(project, QStringLiteral("desk"), QStringLiteral("grok")),
                "an agent on the Mac");
        auto* desk = workspace.focusedSession();
        require(workspace.addCategory(QStringLiteral("Later")) &&
                    workspace.selectCategory(desk_category),
                "a second category");
        const auto later =
            workspace.categories().constLast().toMap().value(QStringLiteral("id")).toString();
        lapis::desktop::WorkspaceControl control(workspace, false);
        require(control.listening(), "the window takes workspace requests");
        const auto registry = workspace.storagePath();

        const auto harnesses =
            askWorkspace(registry, {{QStringLiteral("version"), 1},
                                    {QStringLiteral("request"), QStringLiteral("harnesses")}});
        const auto listed = harnesses.value(QStringLiteral("harnesses")).toArray();
        require(harnesses.value(QStringLiteral("ok")).toBool() &&
                    std::any_of(listed.begin(), listed.end(),
                                [](const QJsonValue& harness) {
                                    return harness[QStringLiteral("id")] ==
                                               QStringLiteral("grok") &&
                                           harness[QStringLiteral("installed")].toBool();
                                }),
                "the phone learns which CLIs this Mac has");

        const auto count = workspace.sessions().size();
        auto wrong_version = createRequest(later, QStringLiteral("grok"), project);
        wrong_version[QStringLiteral("version")] = 2;
        auto missing_field = createRequest(later, QStringLiteral("grok"), project);
        missing_field.remove(QStringLiteral("harness"));
        for (const auto& bad :
             {createRequest(QStringLiteral("nowhere"), QStringLiteral("grok"), project),
              createRequest(later, QStringLiteral("nothing"), project),
              createRequest(later, QStringLiteral("grok"), root.filePath(QStringLiteral("gone"))),
              wrong_version, missing_field,
              QJsonObject{{QStringLiteral("version"), 1},
                          {QStringLiteral("request"), QStringLiteral("handover")}}}) {
            const auto refused = askWorkspace(registry, bad);
            require(!refused.value(QStringLiteral("ok")).toBool() &&
                        !refused.value(QStringLiteral("error")).toString().isEmpty(),
                    "a bad request is refused with a reason");
        }
        require(workspace.sessions().size() == count, "refused requests start nothing");

        const auto started =
            askWorkspace(registry, createRequest(later, QStringLiteral("grok"), project));
        const auto id = started.value(QStringLiteral("id")).toString();
        require(started.value(QStringLiteral("ok")).toBool() && !id.isEmpty() &&
                    !started.value(QStringLiteral("updating")).toBool(),
                "the phone starts an agent");
        require(workspace.activeCategoryId() == desk_category && workspace.focusedSession() == desk,
                "the window keeps its category and agent");
        auto* agent = workspace.session(id);
        require(agent != nullptr && agent->title() == QStringLiteral("project"),
                "the new tab is named after its folder");
        require(waitFor([agent] { return agent->inputReady(); }, 10000) &&
                    waitFor(
                        [agent, &project] {
                            return screenText(agent->snapshot())
                                .contains(QStringLiteral("grok ready in ") + project);
                        },
                        5000),
                "the agent runs in the chosen folder");
        QFile saved(registry);
        require(saved.open(QIODevice::ReadOnly), "read the registry");
        const auto agents = QJsonDocument::fromJson(saved.readAll())
                                .object()
                                .value(QStringLiteral("agents"))
                                .toArray();
        require(std::any_of(agents.begin(), agents.end(),
                            [&](const QJsonValue& entry) {
                                return entry[QStringLiteral("id")] == id &&
                                       entry[QStringLiteral("category")] == later;
                            }),
                "the registry puts it in the chosen category");
        // The phone adds a category without moving the window, and closes an
        // agent as Command-W does.
        const auto made =
            askWorkspace(registry, {{QStringLiteral("version"), 1},
                                    {QStringLiteral("request"), QStringLiteral("createCategory")},
                                    {QStringLiteral("name"), QStringLiteral("Ideas")}});
        require(made.value(QStringLiteral("ok")).toBool() &&
                    workspace.categories().constLast().toMap().value(QStringLiteral("id")) ==
                        made.value(QStringLiteral("id")).toString() &&
                    workspace.activeCategoryId() == desk_category,
                "a category from the phone, with the window left where it was");
        require(
            !askWorkspace(registry, {{QStringLiteral("version"), 1},
                                     {QStringLiteral("request"), QStringLiteral("createCategory")},
                                     {QStringLiteral("name"), QString()}})
                    .value(QStringLiteral("ok"))
                    .toBool() &&
                !askWorkspace(registry, {{QStringLiteral("version"), 1},
                                         {QStringLiteral("request"), QStringLiteral("closeAgent")},
                                         {QStringLiteral("id"), QStringLiteral("nothing")}})
                     .value(QStringLiteral("ok"))
                     .toBool(),
            "an empty name or an unknown agent is refused");
        const auto ideas = made.value(QStringLiteral("id")).toString();
        for (const auto index : {-0.5, 0.5, 2.5, 1e20}) {
            const auto moved_category = askVersioned(
                workspace, {{QStringLiteral("request"), QStringLiteral("placeCategory")},
                            {QStringLiteral("id"), ideas},
                            {QStringLiteral("index"), index}});
            const auto moved_agent =
                askVersioned(workspace, {{QStringLiteral("request"), QStringLiteral("placeAgent")},
                                         {QStringLiteral("id"), id},
                                         {QStringLiteral("category"), later},
                                         {QStringLiteral("index"), index}});
            require(!answeredOk(moved_category) &&
                        moved_category.value(QStringLiteral("error"))
                            .toString()
                            .contains(QStringLiteral("Invalid index")) &&
                        !answeredOk(moved_agent) &&
                        moved_agent.value(QStringLiteral("error"))
                            .toString()
                            .contains(QStringLiteral("Missing category or index")),
                    "fractional and out-of-range positions are not coerced to zero");
        }
        require(workspace.agentPlace(id).value(QStringLiteral("category")) ==
                        QStringLiteral("Later") &&
                    workspace.categories().constLast().toMap().value(QStringLiteral("id")) == ideas,
                "rejected positions leave the workspace in place");
        phoneArrangesTheWorkspace(workspace, {.agent = id, .later = later, .ideas = ideas});
        phoneChangesTheMacsSettings(workspace, control, root);
        // Over ssh: the CLI runs in the folder on that machine, in its login
        // shell. A stand-in ssh prints what it was given.
        QFile ssh(root.filePath(QStringLiteral("bin/ssh")));
        require(ssh.open(QIODevice::WriteOnly), "write the stand-in ssh");
        ssh.write("#!/bin/sh\nfor a in \"$@\"; do printf '[%s]\\n' \"$a\"; done\nexec sleep 600\n");
        ssh.close();
        require(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
                "make it executable");
        auto remote =
            createRequest(later, QStringLiteral("grok"), QStringLiteral("~/dev/some project"));
        remote[QStringLiteral("machine")] = QStringLiteral("-oProxyCommand=touch");
        require(!askWorkspace(registry, remote).value(QStringLiteral("ok")).toBool(),
                "a machine name cannot be an ssh option");
        remote[QStringLiteral("machine")] = QStringLiteral("devbox");
        remote[QStringLiteral("program")] = QStringLiteral("/opt/grok/bin/grok");
        const auto over_ssh = askWorkspace(registry, remote);
        auto* far = workspace.session(over_ssh.value(QStringLiteral("id")).toString());
        require(over_ssh.value(QStringLiteral("ok")).toBool() && far != nullptr &&
                    far->title() == QStringLiteral("some project"),
                "the phone starts an agent on another machine");
        require(
            waitFor(
                [far] {
                    // The command wraps on the fixture's narrow screen.
                    const auto text = screenText(far->snapshot()).remove(QLatin1Char('\n'));
                    return text.contains(QStringLiteral("[-t]")) &&
                           text.contains(QStringLiteral("[devbox]")) &&
                           text.contains(QStringLiteral(
                               R"([cd ~/'dev/some project' && exec "${SHELL:-/bin/sh}" -lic '/opt/grok/bin/grok --fullscreen --permission-mode bypassPermissions'])"));
                },
                10000) &&
                waitFor([far] { return far->inputReady(); }, 10000),
            "ssh runs the CLI in that machine's folder and login shell, in the default mode "
            "when the phone named none");
        // The Mac's own form names the machine the same way.
        QFile config(root.filePath(QStringLiteral("ssh_config")));
        require(config.open(QIODevice::WriteOnly), "write an ssh config");
        config.write("Host devbox\nHost *\n");
        config.close();
        workspace.setSshConfigForTesting(config.fileName());
        require(workspace.sshMachines() == QStringList{QStringLiteral("devbox")},
                "the form offers the ssh config's hosts");
        require(workspace.createAgent(QStringLiteral("~/dev/other"), QStringLiteral("other"),
                                      QStringLiteral("grok"), {}, {}, QStringLiteral("devbox")),
                "the Mac starts an agent on another machine");
        auto* mac_far = workspace.focusedSession();
        require(mac_far != nullptr, "the new agent is shown");
        require(waitFor(
                    [mac_far] {
                        const auto text = screenText(mac_far->snapshot());
                        return text.contains(QStringLiteral("[devbox]")) &&
                               text.contains(QStringLiteral("cd ~/dev/other"));
                    },
                    10000) &&
                    waitFor([mac_far] { return mac_far->inputReady(); }, 10000),
                "over ssh, in that machine's folder");
        for (const auto& closing : {desk->sessionId(), id, far->sessionId(), mac_far->sessionId()})
            require(workspace.closeSession(closing), "close the stand-in agents");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in agents close");
    }
    qputenv("PATH", path);
}

// With no window open, the windowless host serves the phone. A window that
// opens asks the host for the workspace, and the host hands it over and exits.
void windowTakesTheWorkspaceFromTheHost() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-host-XXXXXX"));
    require(directory.isValid(), "host directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    QProcess host;
    host.setProcessChannelMode(QProcess::MergedChannels);
    host.start(QStringLiteral(LAPIS_DESKTOP_PATH),
               {QStringLiteral("--serve"), QStringLiteral("--registry"), options.storagePath});
    const auto control = lapis::desktop::WorkspaceControl::path(options.storagePath);
    require(waitFor(
                [&control] {
                    QLocalSocket probe;
                    probe.connectToServer(control);
                    return probe.waitForConnected(100);
                },
                10000),
            "the host serves the workspace");
    // The host (another process) answers blocking requests too.
    QLocalSocket phone;
    phone.connectToServer(control);
    require(phone.waitForConnected(1000), "the phone reaches the host");
    phone.write(R"({"version":1,"request":"harnesses"})"
                "\n");
    QByteArray answer;
    while (!answer.contains('\n') && phone.waitForReadyRead(3000))
        answer += phone.readAll();
    require(QJsonDocument::fromJson(answer.trimmed()).object().value(QStringLiteral("ok")).toBool(),
            "the host answers the phone");
    QElapsedTimer clock;
    clock.start();
    {
        Workspace window(WorkspaceMode::live, options);
        require(window.workspaceError().isEmpty() && clock.elapsed() < 10000,
                "a window takes the workspace from the host");
        require(host.waitForFinished(5000) && host.exitCode() == 0 &&
                    host.readAll().contains("handed the workspace to a window"),
                "the host hands over and exits");
    }
    qputenv("PATH", path);
}

// An agent that needs you chimes at once and again while its request waits
// and you look elsewhere, up to the configured count; looking, answering or
// turning the sound off stops it. A finished turn chimes once, quietly.
void alertsChimeWhileAnAgentWaits() {
    namespace wire = lapis::session::wire;
    QTemporaryDir directory;
    require(directory.isValid(), "alerts directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    QFile config(root.filePath(QStringLiteral("lapis.json")));
    require(config.open(QIODevice::WriteOnly), "write the config");
    config.write(R"({"version": 1, "alerts": {"sound": true, "finished": true, "repeat": 3}})");
    config.close();
    lapis::desktop::KeyMap keymap;
    keymap.setSourcePathForTesting(config.fileName());
    require(keymap.load(), "alerts config loads");
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    Workspace workspace(WorkspaceMode::live, options);
    lapis::desktop::SessionPreview agent(QStringLiteral("agent"), root.path(), {}, QColor(), "");
    std::vector<lapis::desktop::Chime> played;
    bool looking = false;
    lapis::desktop::Alerts alerts(
        workspace, keymap, [&played](lapis::desktop::Chime chime) { played.push_back(chime); },
        [&looking](const lapis::desktop::SessionPreview*) { return looking; });
    alerts.setTimingForTesting({.repeatMs = 120, .quietMs = 40});
    const auto request = [&agent](bool pending) {
        wire::AttentionSnapshot state;
        state.available = state.connected = state.ready = true;
        state.source_epoch = 1;
        if (pending) {
            lapis::session::attention::Pending item;
            item.request = {.id = std::int64_t{1},
                            .thread_id = "t",
                            .turn_id = "u",
                            .item_id = "i",
                            .reason = "Approval",
                            .summary = "Run tests",
                            .choices = {"accept"}};
            item.source_epoch = item.revision = 1;
            state.requests.push_back({item, QJsonObject{}});
        }
        agent.applyAttention(state);
    };
    const auto needs = [&played] {
        return std::count(played.begin(), played.end(), lapis::desktop::Chime::needsYou);
    };
    request(true);
    // Production requests intentionally share the one finished-turn cue.
    emit workspace.turnFinished(&agent);
    waitFor([] { return false; }, 300);
    require(played == std::vector<lapis::desktop::Chime>{lapis::desktop::Chime::finished},
            "a production request keeps the shared single chime");
    played.clear();
    // The legacy explicit signal still has its existing repeat contract.
    emit workspace.agentNeedsYou(&agent);
    require(needs() == 1, "a request chimes at once");
    waitFor([] { return false; }, 700);
    require(needs() == 3, "and again while it waits, three times in all");

    played.clear();
    looking = true;
    emit workspace.agentNeedsYou(&agent);
    waitFor([] { return false; }, 300);
    require(played.empty(), "nothing plays for the agent being looked at");

    looking = false;
    emit workspace.agentNeedsYou(&agent);
    require(needs() == 1, "a request out of view chimes");
    looking = true;
    waitFor([] { return false; }, 400);
    require(needs() == 1, "looking at the agent stops the repeats");

    looking = false;
    played.clear();
    waitFor([] { return false; }, 60);
    emit workspace.agentNeedsYou(&agent);
    request(false);
    waitFor([] { return false; }, 400);
    require(needs() == 1, "an answered request stops the repeats");

    played.clear();
    waitFor([] { return false; }, 60);
    emit workspace.turnFinished(&agent);
    require(played == std::vector{lapis::desktop::Chime::finished}, "a finished turn chimes once");

    // A turn that ends on what the person already saw there says nothing new:
    // no chime, whatever Claude Code's status line below its input box does.
    // New output above the box chimes again. Every decision is logged.
    {
        lapis::session::Terminal screen({40, 8});
        const auto show = [&screen, &agent](std::string_view text) {
            screen.feed(text);
            agent.applySnapshot(screen.snapshot());
        };
        const std::string line = "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80";
        show("\x1b[2J\x1b[Hdone: tests pass\r\n" + line + "\r\n> \r\n" + line + "\r\n5h 53%");
        lapis::desktop::SeenScreens seen(workspace, [&looking](const auto*) { return looking; });
        std::vector<QJsonObject> lines;
        alerts.setSeen(&seen);
        alerts.setLog([&lines](const QJsonObject& entry) { lines.push_back(entry); });
        seen.see(&agent);
        // The null tolerance see() already documents: never on screen means
        // changed, so the chime path can never dereference a null item.
        require(!seen.unchanged(nullptr), "an item never on screen counts as changed");
        played.clear();
        waitFor([] { return false; }, 60);
        show("\x1b[5;1H5h 54%"); // the status line ticks
        emit workspace.turnFinished(&agent);
        require(
            played.empty() && !lines.empty() &&
                lines.back().value(QStringLiteral("kind")).toString() == QStringLiteral("chime") &&
                lines.back().value(QStringLiteral("cli")).toString() == QStringLiteral("Codex") &&
                lines.back().contains(QStringLiteral("at")) &&
                lines.back().value(QStringLiteral("decision")).toString() ==
                    QStringLiteral("quiet: nothing new since you looked"),
            "a turn ending on a screen already seen stays quiet, logged in the shared shape");
        show("\x1b[1;1Hnew: build failed");
        emit workspace.turnFinished(&agent);
        require(played == std::vector{lapis::desktop::Chime::finished} &&
                    lines.back().value(QStringLiteral("decision")).toString() ==
                        QStringLiteral("chimed") &&
                    lines.back().value(QStringLiteral("agent")).toString() ==
                        QStringLiteral("agent"),
                "new output chimes, and the decision names the agent");
        // The quiet paths of the request chime reach the log too.
        waitFor([] { return false; }, 60);
        looking = true;
        played.clear();
        emit workspace.agentNeedsYou(&agent);
        require(played.empty() &&
                    lines.back().value(QStringLiteral("event")).toString() ==
                        QStringLiteral("needs you") &&
                    lines.back().value(QStringLiteral("decision")).toString() ==
                        QStringLiteral("quiet: you are looking at it"),
                "a request for the agent being looked at stays quiet, and says so");
        looking = false;
        waitFor([] { return false; }, 60);
        emit workspace.agentNeedsYou(&agent);
        require(played == std::vector{lapis::desktop::Chime::needsYou} &&
                    lines.back().value(QStringLiteral("event")).toString() ==
                        QStringLiteral("needs you") &&
                    lines.back().value(QStringLiteral("decision")).toString() ==
                        QStringLiteral("chimed"),
                "a request chimes even on an unchanged screen, and its decision is logged");
        // A lone rule is a divider in the agent's output, not Claude Code's
        // box: nothing is truncated, so output below it still counts as new.
        show("\x1b[2J\x1b[Hnote\r\n" + line + "\r\nstill watching\r\n");
        seen.see(&agent);
        show("\x1b[3;1Hchanged below the lone rule");
        played.clear();
        waitFor([] { return false; }, 60);
        emit workspace.turnFinished(&agent);
        require(played == std::vector{lapis::desktop::Chime::finished},
                "a lone rule truncates nothing: output below it still chimes");
        // Claude Code's box may open with corner glyphs and heavier strokes;
        // those borders are rules, and the status line below them is not new.
        const std::string box_top = "\xe2\x95\xad" + line + "\xe2\x95\xae";    // ╭────╮
        const std::string box_bottom = "\xe2\x95\xb0" + line + "\xe2\x95\xaf"; // ╰────╯
        show("\x1b[2J\x1b[Hdone\r\n" + box_top + "\r\n> \r\n" + box_bottom + "\r\n5h 53%");
        seen.see(&agent);
        show("\x1b[5;1H5h 54%");
        played.clear();
        waitFor([] { return false; }, 60);
        emit workspace.turnFinished(&agent);
        require(played.empty() && lines.back().value(QStringLiteral("decision")).toString() ==
                                      QStringLiteral("quiet: nothing new since you looked"),
                "a box drawn with corners truncates, and a status line below it is not new");
        alerts.setSeen(nullptr);
        alerts.setLog({});
    }

    require(keymap.setAlertSound(false), "turn the sound off");
    played.clear();
    std::vector<QJsonObject> quiet_lines;
    alerts.setLog([&quiet_lines](const QJsonObject& entry) { quiet_lines.push_back(entry); });
    request(true);
    waitFor([] { return false; }, 60);
    emit workspace.agentNeedsYou(&agent);
    waitFor([] { return false; }, 300);
    require(played.empty() && !quiet_lines.empty() &&
                quiet_lines.back().value(QStringLiteral("kind")).toString() ==
                    QStringLiteral("chime") &&
                quiet_lines.back().value(QStringLiteral("decision")).toString() ==
                    QStringLiteral("alert off"),
            "no chimes with the sound off, and that decision is logged");
    alerts.setLog({});

    // Notifications: the same moments, only while lapis is in the background,
    // and not with them turned off.
    std::vector<QStringList> posted;
    bool background = false;
    lapis::desktop::Notifier notifier(
        workspace, keymap,
        [&posted](const QString& id, const QString& title, const QString& body) {
            posted.push_back({id, title, body});
        },
        [&background] { return background; });
    std::vector<QJsonObject> notes;
    notifier.setLog([&notes](const QJsonObject& entry) { notes.push_back(entry); });
    request(true);
    emit workspace.agentNeedsYou(&agent);
    require(posted.empty() && notes.back().value(QStringLiteral("decision")).toString() ==
                                  QStringLiteral("none: lapis is in front"),
            "no notification while lapis is in front, and that decision is logged");
    background = true;
    emit workspace.agentNeedsYou(&agent);
    require(
        posted.size() == 1 && posted[0][1] == QStringLiteral("agent") &&
            posted[0][2] == QStringLiteral("Codex needs you: Approval") &&
            notes.back().value(QStringLiteral("kind")).toString() ==
                QStringLiteral("notification") &&
            notes.back().value(QStringLiteral("event")).toString() == QStringLiteral("needs you") &&
            notes.back().value(QStringLiteral("decision")).toString() == QStringLiteral("posted"),
        "a request in the background posts one notification naming the agent and its CLI");
    emit workspace.turnFinished(&agent);
    require(posted.size() == 2 && posted[1][2] == QStringLiteral("Codex finished a turn") &&
                notes.back().value(QStringLiteral("decision")).toString() ==
                    QStringLiteral("posted"),
            "a finished turn posts one too");
    // A finished turn on an unchanged screen posts nothing; a request never
    // holds back, whatever the screen says.
    lapis::desktop::SeenScreens seen(workspace, [&looking](const auto*) { return looking; });
    notifier.setSeen(&seen);
    seen.see(&agent);
    emit workspace.turnFinished(&agent);
    require(posted.size() == 2 && notes.back().value(QStringLiteral("decision")).toString() ==
                                      QStringLiteral("none: nothing new since you looked"),
            "a finished turn on an unchanged screen posts nothing, and says why");
    emit workspace.agentNeedsYou(&agent);
    require(posted.size() == 3 && posted[2][2] == QStringLiteral("Codex needs you: Approval") &&
                notes.back().value(QStringLiteral("decision")).toString() ==
                    QStringLiteral("posted"),
            "a request still notifies on an unchanged screen");
    notifier.setSeen(nullptr);
    notifier.setLog({});
    // A Claude agent whose conversation is about Codex is still Claude's.
    agent.setHarnessId(QStringLiteral("claude"));
    agent.rename(QStringLiteral("Codex resume"));
    emit workspace.turnFinished(&agent);
    require(posted.size() == 4 && posted[3][1] == QStringLiteral("Codex resume") &&
                posted[3][2] == QStringLiteral("Claude finished a turn"),
            "the body names the agent's CLI, whatever its title says");
    agent.setHarnessId({});
    emit workspace.turnFinished(&agent);
    require(posted.size() == 5 && posted[4][2] == QStringLiteral("Agent finished a turn"),
            "terminal-mode notifications retain a readable subject");
    agent.setHarnessId(QStringLiteral("codex"));
    agent.rename(QStringLiteral("agent"));
    require(keymap.setNotify(false), "turn notifications off");
    emit workspace.agentNeedsYou(&agent);
    require(posted.size() == 5, "none with notifications off");

    const auto wav = lapis::desktop::chime_wav(lapis::desktop::Chime::needsYou);
    require(wav.startsWith("RIFF") && wav.mid(8, 8) == "WAVEfmt " && wav.size() == 44 + 27342 * 2,
            "a chime is a 0.62 second, 16-bit mono WAV");
    qint16 peak = 0;
    for (qsizetype i = 44; i + 1 < wav.size(); i += 2)
        peak = std::max<qint16>(
            peak, static_cast<qint16>(std::abs(qFromLittleEndian<qint16>(wav.constData() + i))));
    require(qFromLittleEndian<qint16>(wav.constData() + 44) == 0 && peak > 8000 && peak < 8500,
            "it starts from silence and peaks near -12 dBFS");
}

// The attention log's own behavior: owner-only however it starts, rotating
// beside a single predecessor before a line would cross the cap, marking the
// rotation, and stopping the write when the old file cannot move aside.
void attentionLogStaysPrivateAndRotates() {
    const qsizetype cap = qsizetype{2} * 1024 * 1024;
    const auto owner_only = [](const QString& path) {
        const QFileInfo info(path);
        // On Unix Qt reports owner access under both the Owner and User bits,
        // so the privacy property is that nothing outside the owner is set.
        return info.exists() &&
               (info.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                      QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) ==
                   QFile::Permissions{};
    };
    const auto lines_of = [](const QString& path) {
        std::vector<QJsonObject> parsed;
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "read the attention log back");
        for (const auto& raw : file.readAll().split('\n'))
            if (!raw.isEmpty())
                parsed.push_back(QJsonDocument::fromJson(raw).object());
        return parsed;
    };
    QTemporaryDir directory;
    require(directory.isValid(), "attention log directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = root.filePath(QStringLiteral("runtime/attention.jsonl"));
    const auto log = lapis::desktop::attention_log(path);
    log({{"event", QStringLiteral("chimed")}});
    require(lines_of(path).size() == 1, "the log keeps the line it was given");
    require(owner_only(path), "a log lapis creates is owner-only");

    // A world-readable log is made private before anything is appended.
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup |
                                    QFile::ReadOther);
    log({{"event", QStringLiteral("second")}});
    require(owner_only(path) && lines_of(path).size() == 2,
            "an existing open log is tightened, then written");

    // Rotation moves the file beside one predecessor and opens the fresh one
    // with a marker line, so a reader can tell a rotation from a gap.
    log({{"pad", QString(cap - 60, QLatin1Char('x'))}});
    const auto previous = path + QStringLiteral(".1");
    require(lines_of(path).size() == 3 && QFile(path).size() <= cap && !QFile::exists(previous),
            "a line that still fits crosses no cap");
    log({{"event", QStringLiteral("after")}});
    const auto rotated = lines_of(previous);
    const auto fresh = lines_of(path);
    require(owner_only(previous) && rotated.size() == 3,
            "the rotated predecessor keeps the earlier lines");
    require(owner_only(path) && fresh.size() == 2 &&
                fresh[0].value(QStringLiteral("event")).toString() == QStringLiteral("rotated") &&
                fresh[0].contains(QStringLiteral("at")) &&
                fresh[1].value(QStringLiteral("event")).toString() == QStringLiteral("after"),
            "the fresh log opens with a rotation marker before the new line");

    // A predecessor that cannot be removed stops the write instead of letting
    // the file grow without bound.
    const auto blocked = root.filePath(QStringLiteral("blocked/attention.jsonl"));
    const auto blocked_log = lapis::desktop::attention_log(blocked);
    blocked_log({{"event", QStringLiteral("first")}, {"pad", QString(cap - 30, QLatin1Char('x'))}});
    require(QDir().mkdir(blocked + QStringLiteral(".1")), "occupy the rotation target");
    const qint64 before = QFile(blocked).size();
    blocked_log({{"event", QStringLiteral("second")}});
    require(QFile(blocked).size() == before && QFileInfo(blocked + QStringLiteral(".1")).isDir(),
            "a failed rotation drops the line and keeps the cap");
}

// A chosen sound file replaces a chime and is read again when it changes; a
// finished turn without its own file plays the same one at half volume, and a
// missing file is named when the config loads and plays the taps.
void chimesPlayChosenFiles() {
    using lapis::desktop::Chime;
    using lapis::desktop::ChimeSound;
    QTemporaryDir directory;
    require(directory.isValid(), "sounds directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto write = [&root](const QString& name, const QByteArray& bytes) {
        QFile file(root.filePath(name));
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    file.write(bytes) == bytes.size(),
                "write sound fixture");
    };
    write(QStringLiteral("ding.wav"), "RIFF-ding");
    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":"ding.wav"}})");
    lapis::desktop::KeyMap keymap;
    keymap.setSourcePathForTesting(root.filePath(QStringLiteral("lapis.json")));
    lapis::desktop::ChimeSounds sounds;
    const auto ready = [&](Chime chime, const QByteArray& bytes) {
        return waitFor([&] { return sounds.sound(chime, keymap).bytes == bytes; }, 10000);
    };
    const auto diagnosed = [&](Chime chime, const QString& text) {
        return waitFor(
            [&] {
                static_cast<void>(sounds.sound(chime, keymap));
                return keymap.diagnostic().contains(text);
            },
            10000);
    };
    require(keymap.load() && keymap.alertSoundFile() == root.filePath(QStringLiteral("ding.wav")),
            "relative sound path resolves without file I/O on the GUI thread");
    require(ready(Chime::needsYou, "RIFF-ding"), "background file load completes");
    require(sounds.sound(Chime::finished, keymap).volume == 0.5F,
            "the shared cue borrows soundFile at half volume");
    std::vector<ChimeSound> attempted;
    sounds.play(Chime::finished, keymap, [&](const ChimeSound& clip) {
        attempted.push_back(clip);
        return clip.path.isEmpty();
    });
    require(attempted.size() == 2 && attempted[0].volume == 0.5F && attempted[1].volume == 1.0F &&
                attempted[1].bytes == lapis::desktop::chime_wav(Chime::finished),
            "decode failure uses the synthesized cue at its own gain, not double attenuation");
    require(diagnosed(Chime::finished, QStringLiteral("could not be played")),
            "playback failure is visible");
    sounds.play(Chime::finished, keymap, [](const ChimeSound&) { return true; });
    require(waitFor([&] { return keymap.diagnostic().isEmpty(); }, 10000),
            "playback recovery clears its diagnostic");
    sounds.play(Chime::finished, keymap, [](const ChimeSound&) { return false; });
    require(diagnosed(Chime::finished, QStringLiteral("playback is unavailable")),
            "a failed synthesized fallback is visible too");
    sounds.play(Chime::finished, keymap, [](const ChimeSound&) { return true; });
    require(waitFor([&] { return keymap.diagnostic().isEmpty(); }, 10000),
            "successful output clears the fallback failure");
    const auto modified = QFileInfo(root.filePath(QStringLiteral("ding.wav"))).lastModified();
    write(QStringLiteral("ding.wav"), "RIFF-ping");
    QFile preserved(root.filePath(QStringLiteral("ding.wav")));
    require(preserved.open(QIODevice::ReadWrite) &&
                preserved.setFileTime(modified, QFileDevice::FileModificationTime),
            "preserve mtime while replacing same-size content");
    preserved.close();
    require(ready(Chime::needsYou, "RIFF-ping"),
            "descriptor change time invalidates a metadata-preserving edit");
    write(QStringLiteral("ding.wav"), "RIFF-ding, edited");
    require(ready(Chime::needsYou, "RIFF-ding, edited"),
            "edited bytes replace the cached version asynchronously");

    write(QStringLiteral("low.wav"), "RIFF-low");
    write(QStringLiteral("lapis.json"),
          R"({"version":1,"alerts":{"soundFile":"gone.wav","finishedFile":"low.wav"}})");
    require(keymap.load() && diagnosed(Chime::needsYou, QStringLiteral("gone.wav")),
            "missing file diagnostic arrives from the loader");
    require(ready(Chime::finished, "RIFF-low") &&
                sounds.sound(Chime::finished, keymap).volume == 1.0F,
            "an explicit finishedFile uses its own volume");
    write(QStringLiteral("gone.wav"), "RIFF-created");
    require(ready(Chime::needsYou, "RIFF-created") &&
                waitFor([&] { return keymap.diagnostic().isEmpty(); }, 10000),
            "creating a file clears stale diagnostics without reloading configuration");
    require(QFile::remove(root.filePath(QStringLiteral("gone.wav"))), "remove configured file");
    require(diagnosed(Chime::needsYou, QStringLiteral("not found")) &&
                sounds.sound(Chime::needsYou, keymap).bytes ==
                    lapis::desktop::chime_wav(Chime::needsYou),
            "deletion is reported and falls back");

    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":"ding.wav"}})");
    require(keymap.load() && ready(Chime::needsYou, "RIFF-ding, edited"), "restore readable file");
    const auto permissions = QFile::permissions(root.filePath(QStringLiteral("ding.wav")));
    require(QFile::setPermissions(root.filePath(QStringLiteral("ding.wav")), {}),
            "revoke fixture permissions");
    require(diagnosed(Chime::needsYou, QStringLiteral("not readable")),
            "permission change invalidates a cached read");
    require(QFile::setPermissions(root.filePath(QStringLiteral("ding.wav")), permissions),
            "restore fixture permissions");
    require(ready(Chime::needsYou, "RIFF-ding, edited"),
            "permission-only recovery retries without a size/mtime change");

    const auto relative = QDir::home().relativeFilePath(root.filePath(QStringLiteral("ding.wav")));
    const QJsonObject home_config{
        {QStringLiteral("alerts"),
         QJsonObject{{QStringLiteral("soundFile"), QStringLiteral("~/") + relative}}}};
    write(QStringLiteral("lapis.json"), QJsonDocument(home_config).toJson());
    require(keymap.load() && keymap.alertSoundFile() == root.filePath(QStringLiteral("ding.wav")),
            "tilde expansion works without changing the process HOME");
    write(QStringLiteral("lapis.json"),
          R"({"version":1,"alerts":{"soundFile":"ding.wav","finishedFile":"gone.wav"}})");
    require(keymap.load() && diagnosed(Chime::finished, QStringLiteral("gone.wav")) &&
                sounds.sound(Chime::finished, keymap).bytes ==
                    lapis::desktop::chime_wav(Chime::finished),
            "an unavailable explicit finishedFile never borrows the other clip");
    QFile big(root.filePath(QStringLiteral("big.wav")));
    require(big.open(QIODevice::WriteOnly) &&
                big.resize(lapis::desktop::ChimeSounds::kMaxFileBytes + 1),
            "oversized fixture");
    big.close();
    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":"big.wav"}})");
    require(keymap.load() && diagnosed(Chime::needsYou, QStringLiteral("over 4 MiB")),
            "oversized file is diagnosed asynchronously");
    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":12}})");
    require(keymap.load() && keymap.diagnostic().contains(QStringLiteral("path string")),
            "malformed setting is diagnosed immediately");
    const auto fifo = root.filePath(QStringLiteral("pipe.wav"));
    require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0,
            "create special-file fixture");
    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":"pipe.wav"}})");
    require(keymap.load() && diagnosed(Chime::needsYou, QStringLiteral("not a regular file")),
            "nonblocking descriptor validation rejects a FIFO without losing a worker");
}

// A new agent's CLI updates itself first, so the agent never opens on an
// update prompt; another agent within 30 minutes starts without updating.
void harnessesUpdateBeforeNewAgents() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-update-XXXXXX"));
    require(directory.isValid(), "update directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QDir root(canonical);
    require(root.mkpath(QStringLiteral("bin")) && root.mkpath(QStringLiteral("project")),
            "fixture folders");
    const auto grok = root.filePath(QStringLiteral("bin/grok"));
    QFile script(grok);
    require(script.open(QIODevice::WriteOnly), "write the stand-in CLI");
    script.write("#!/bin/sh\n"
                 "log=\"$(dirname \"$0\")/updates\"\n"
                 "if [ \"$1\" = update ]; then echo update >> \"$log\"; sleep 1; "
                 "echo 'grok updated to 9.9'; exit 0; fi\n"
                 "echo \"started after $(wc -l < \"$log\" | tr -d ' ') update\"\n"
                 "exec sleep 600\n");
    script.close();
    require(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make it executable");
    const auto path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(root.filePath(QStringLiteral("bin"))) + ':' + path);
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.updateHarnesses = true;
    {
        Workspace workspace(WorkspaceMode::live, options);
        const auto project = root.filePath(QStringLiteral("project"));
        require(workspace.createAgent(project, QStringLiteral("first"), QStringLiteral("grok")),
                "create a Grok agent");
        auto* first = workspace.focusedSession();
        require(first->statusLabel() == QStringLiteral("Updating Grok…") && !first->live(),
                "the card waits while the CLI updates");
        require(waitFor([first] { return first->inputReady(); }, 15000),
                "the agent starts after the update");
        require(waitFor(
                    [first] {
                        return screenText(first->snapshot())
                            .contains(QStringLiteral("started after 1 update"));
                    },
                    5000),
                "the agent started on the updated CLI");
        require(workspace.createAgent(project, QStringLiteral("second"), QStringLiteral("grok")),
                "create another Grok agent");
        auto* second = workspace.focusedSession();
        require(second->statusLabel() != QStringLiteral("Updating Grok…"),
                "a recent update is not repeated");
        require(waitFor(
                    [second] {
                        return screenText(second->snapshot())
                            .contains(QStringLiteral("started after 1 update"));
                    },
                    10000),
                "the second agent started without another update");
        QFile log(root.filePath(QStringLiteral("harness-updates.log")));
        require(log.open(QIODevice::ReadOnly) &&
                    log.readAll().contains("grok update: exit 0. grok updated to 9.9"),
                "the update is logged");
        require(waitFor([second] { return second->inputReady(); }, 10000),
                "the second agent is ready");
        for (const auto& id : {first->sessionId(), second->sessionId()})
            require(workspace.closeSession(id), "close the stand-in agents");
        require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
                "the stand-in agents close");
    }
    qputenv("PATH", path);
}

// The phone's size lasts only while someone uses the phone: coming back to the
// desktop takes the size back, and so does the phone leaving.
void phoneSizeYieldsToTheDesktop() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-size-XXXXXX"));
    require(directory.isValid(), "service directory");
    WorkspaceOptions options;
    options.endpoint = QDir(QFileInfo(directory.path()).canonicalFilePath())
                           .filePath(QStringLiteral("agent.sock"));
    options.launch = lapis::session::LaunchSpec{
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"), QStringLiteral("while read line; do echo \"got:$line\"; done")},
        directory.path(),
        {80, 24},
        lapis::session::AgentMode::terminal};
    options.mode = lapis::session::wire::AttachMode::create;
    Workspace workspace(WorkspaceMode::live, options);
    auto* agent = workspace.focusedSession();
    require(agent != nullptr && waitFor([agent] { return agent->inputReady(); }, 10000),
            "the desktop is attached");
    const lapis::session::TerminalSize desk{100, 30};
    const lapis::session::TerminalSize phone_size{40, 20};
    const auto shows = [agent](lapis::session::TerminalSize size) {
        return waitFor([agent, size] { return agent->snapshot().size == size; }, 5000);
    };
    agent->resizeTerminal(desk);
    require(shows(desk), "the desktop sets its size");
    JoinedView phone(options.endpoint, lapis::session::validate_launch(*options.launch));
    require(phone.waitForText(QString(), 5000), "the phone joins");
    phone.resize(phone_size.columns, phone_size.rows);
    require(shows(phone_size), "the phone takes the size when it opens the agent");
    agent->claimTerminalSize();
    require(shows(desk), "coming back to the desktop takes the size back");
    phone.resize(phone_size.columns, phone_size.rows);
    require(shows(phone_size), "the phone takes it again");
    phone.leave();
    require(shows(desk), "the phone leaving hands the size back to the desktop");
    require(workspace.closeSession(agent->sessionId()) &&
                waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "the size fixture closes");
}

// Closing an agent while a history page shows ends it: the page hides input,
// not the service, so the agent must not be abandoned while still running.
void closeOnHistoryPageEndsTheAgent() {
    QTemporaryDir directory;
    require(directory.isValid(), "service directory");
    WorkspaceOptions options;
    options.endpoint = QDir(QFileInfo(directory.path()).canonicalFilePath())
                           .filePath(QStringLiteral("agent.sock"));
    options.launch =
        lapis::session::LaunchSpec{QStringLiteral("/bin/sh"),
                                   {QStringLiteral("-c"), QStringLiteral("exec sleep 600")},
                                   directory.path(),
                                   {80, 24},
                                   lapis::session::AgentMode::terminal};
    options.mode = lapis::session::wire::AttachMode::create;
    Workspace workspace(WorkspaceMode::live, options);
    auto* agent = workspace.focusedSession();
    require(agent != nullptr, "service-backed agent");
    require(waitFor([agent] { return agent->inputReady(); }, 10000), "agent session ready");
    agent->beginHistoryRequest();
    require(!agent->inputReady() && agent->reachable(),
            "a history page hides input but the agent stays reachable");
    const QPointer<lapis::desktop::SessionPreview> guarded(agent);
    require(workspace.closeSession(agent->sessionId(), true) && !workspace.sessions().isEmpty() &&
                guarded && guarded->closing(),
            "closing from a history page ends the agent instead of abandoning it");
    require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "the tab closes once the process has exited");
}

// Restart must not replace a connection while its close request is in flight;
// doing so would erase the ended transition and make the tab impossible to close.
void restartRefusesClosingAgent() {
    QTemporaryDir directory;
    require(directory.isValid(), "closing-agent directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString id = uuid();
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    auto record = agentRecord(canonical, id, "general");
    record.insert(QStringLiteral("harness"), QStringLiteral("gemini"));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", QJsonArray{record}}});
    Workspace workspace(WorkspaceMode::live, options);
    auto* agent = workspace.session(id);
    require(agent != nullptr, "closing-agent fixture loads");
    agent->setClosing(true);
    require(!workspace.restartAgent(id), "a closing agent is not restarted");
    require(workspace.workspaceError() == QStringLiteral("This agent is still closing."),
            "the restart error names the pending close");
}

// validate_launch owns detailed constraints; restart must retain that cause in
// the log instead of translating every rejection into a missing program.
void restartReportsValidationFailures() {
    QTemporaryDir directory;
    require(directory.isValid(), "validation directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString id = uuid();
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    auto record = agentRecord(canonical, id, "general");
    record.insert(QStringLiteral("harness"), QStringLiteral("gemini"));
    QStringList arguments;
    for (int index = 0; index < 64; ++index)
        arguments.append(QString(4096, QLatin1Char('x')));
    record.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(arguments));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", QJsonArray{record}}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "oversized-argument fixture loads");
    require(!workspace.restartAgent(id), "an invalid restored launch is rejected");
    require(workspace.workspaceError().contains(QStringLiteral("Launch values exceed 64 KiB")),
            "validation failure exposes its actual cause to the user");
}

// A catalog fallback owns the CLI program, not every saved program for that
// harness. Relocate a missing native CLI, but refuse to run it with arguments
// belonging to an ssh transport.
void restoreProgramFallbackSeparatesTransportFromHarness() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-transport-XXXXXX"));
    require(directory.isValid(), "transport-fallback directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QDir root(canonical);
    require(root.mkpath(QStringLiteral("bin")), "transport fixture bin");
    const QDir bin(root.filePath(QStringLiteral("bin")));
    const auto missing_ssh = root.filePath(QStringLiteral("missing/ssh"));
    const auto missing_grok = root.filePath(QStringLiteral("missing/grok"));
    const auto local_claude = bin.filePath(QStringLiteral("claude"));
    const auto fake_grok = bin.filePath(QStringLiteral("grok"));
    const auto bad_invocation = root.filePath(QStringLiteral("local-claude-launched"));
    writeExecutable(local_claude, "#!/bin/sh\nprintf 'local-claude-launch\\n' > '" +
                                      QFile::encodeName(bad_invocation) + "'\nexit 9\n");
    writeExecutable(fake_grok, QByteArrayLiteral("#!/bin/sh\n"
                                                 "echo grok ready\n"
                                                 "exec /bin/sleep 60\n"));
    const auto previous_path = qgetenv("PATH");
    const auto restore_path = qScopeGuard([&] { qputenv("PATH", previous_path); });
    qputenv("PATH", QFile::encodeName(bin.path()));

    const QString remote_id = uuid();
    const QString native_id = uuid();
    const QJsonArray remote_arguments{
        QStringLiteral("-o"),
        QStringLiteral("ServerAliveInterval=15"),
        QStringLiteral("-o"),
        QStringLiteral("ServerAliveCountMax=4"),
        QStringLiteral("-t"),
        QStringLiteral("devbox"),
        QStringLiteral("cd ~/dev/far && s=%1 && exec \"${SHELL:-/bin/sh}\" -lic "
                       "'claude '\"$o $s\"")
            .arg(uuid())};
    auto remote = agentRecord(canonical, remote_id, "general");
    remote.insert(QStringLiteral("program"), missing_ssh);
    remote.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    remote.insert(QStringLiteral("arguments"), remote_arguments);
    auto native = agentRecord(canonical, native_id, "general");
    native.insert(QStringLiteral("program"), missing_grok);
    native.insert(QStringLiteral("harness"), QStringLiteral("grok"));

    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", QJsonArray{remote, native}}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "transport-fallback fixture loads");
    auto* native_item = workspace.session(native_id);
    require(workspace.session(remote_id) != nullptr && native_item != nullptr,
            "transport-fallback agents load");

    require(!workspace.restartAgent(remote_id),
            "a missing remote transport is not replaced by its harness");
    require(
        workspace.workspaceError().contains(QStringLiteral("Program is not an executable file")),
        "a missing remote transport names the launch failure");
    const auto saved_agent = [&](const QString& id) {
        for (const auto& value : QJsonDocument::fromJson(readRegistry(options.storagePath))
                                     .object()
                                     .value(QStringLiteral("agents"))
                                     .toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject();
        throw std::runtime_error("transport-fallback agent is missing");
    };
    const auto saved_remote = saved_agent(remote_id);
    require(saved_remote.value(QStringLiteral("program")).toString() == missing_ssh &&
                saved_remote.value(QStringLiteral("arguments")).toArray() == remote_arguments,
            "a failed remote restart preserves the transport launch");
    require(!QFileInfo::exists(bad_invocation), "remote arguments do not launch local Claude");

    require(workspace.restartAgent(native_id),
            "a missing native harness program relocates through the catalog");
    require(waitFor([native_item] { return native_item->inputReady(); }, 10000),
            "the relocated native CLI starts");
    require(saved_agent(native_id).value(QStringLiteral("program")).toString() == fake_grok,
            "native relocation records the catalog executable");
    require(saved_agent(remote_id).value(QStringLiteral("program")).toString() == missing_ssh,
            "native relocation does not rewrite the remote transport");
    require(workspace.closeSession(native_id) && waitFor(
                                                     [&workspace, native_id] {
                                                         return workspace.sessions().size() == 1 &&
                                                                workspace.session(native_id) ==
                                                                    nullptr;
                                                     },
                                                     10000),
            "the relocated native fixture closes");
    require(!QFileInfo::exists(bad_invocation), "the transport is never invoked as local Claude");
}

// A restarted service must wait until its launch metadata is durable. The
// readable but non-writable registry makes QSaveFile refuse the save after
// restore planning, without racing a missing service or changing file flags.
void restoreSaveFailureStartsNoService() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-restore-save-XXXXXX"));
    require(directory.isValid(), "restore save-failure directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString id = uuid();
    WorkspaceOptions options;
    options.restoreAgents = true;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", QJsonArray{agentRecord(canonical, id, "general")}}});
    const auto original = readRegistry(options.storagePath);
    const auto endpoint = QDir(canonical).filePath(id + QStringLiteral(".sock"));
    const auto restore_permissions = qScopeGuard([&] {
        if (!QFile::setPermissions(options.storagePath, QFile::ReadOwner | QFile::WriteOwner))
            qWarning() << "Could not restore fixture registry permissions";
    });
    require(QFile::setPermissions(options.storagePath, QFile::ReadOwner),
            "make the registry readable while its QSaveFile write fails");
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().contains(QStringLiteral("Cannot save workspace:")),
                "restore reports the failed metadata commit");
        require(workspace.sessions().isEmpty(), "failed restore retains no session objects");
    }
    require(readRegistry(options.storagePath) == original,
            "failed restore leaves the original registry bytes intact");
    require(!QFileInfo::exists(endpoint) && !QFileInfo::exists(endpoint + QStringLiteral(".log")),
            "failed restore starts neither a service nor its log");
    require(QDir(canonical).entryList({QStringLiteral("workspace.json.*")}, QDir::Files).isEmpty(),
            "QSaveFile removed its failed restore temporary");
}

// The session service ends the agent's process group; the tab closes after.
// Agents whose session service is gone (a reboot or crash) come back when
// lapis opens, resuming the conversation their service recorded, like a
// restored terminal tab. Without the option nothing is restarted.
void agentsRestoreAfterServiceLoss() {
    // Unix socket paths are short (104 bytes on macOS): keep endpoints near /.
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-restore-XXXXXX"));
    require(directory.isValid(), "restore directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const auto script = QDir(canonical).filePath(QStringLiteral("agent.sh"));
    {
        // Runs until it reads a line.
        QFile file(script);
        require(file.open(QIODevice::WriteOnly), "write the agent script");
        file.write("#!/bin/sh\nread line\n");
    }
    require(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make the agent script executable");
    const QString resumed = uuid();
    const QString fresh = uuid();
    const QString foreign = uuid();
    const auto record = [&](const QString& id, const char* harness, const QJsonArray& arguments) {
        auto value = agentRecord(canonical, id, "general");
        value.insert(QStringLiteral("program"), script);
        value.insert(QStringLiteral("harness"), QLatin1String(harness));
        value.insert(QStringLiteral("arguments"), arguments);
        return value;
    };
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{
            {"version", 2},
            {"activeCategory", "general"},
            {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
            {"agents",
             QJsonArray{record(resumed, "kimi", {"--yolo", "--session", "old-conversation"}),
                        record(fresh, "gemini", {"--yolo"}), record(foreign, "kimi", {})}}});
    const auto endpoint = [&](const QString& id) {
        return QDir(canonical).filePath(id + QStringLiteral(".sock"));
    };
    writeObservedResume(endpoint(resumed), {QStringLiteral("kimi"), QStringLiteral("k-123")});
    writeObservedResume(endpoint(foreign), {QStringLiteral("claude"), QStringLiteral("c-9")});
    {
        Workspace untouched(WorkspaceMode::live, options);
        require(untouched.workspaceError().isEmpty(), "load without restoring");
    }
    const auto arguments = [&](const QString& id) {
        for (const auto& value : QJsonDocument::fromJson(readRegistry(options.storagePath))
                                     .object()
                                     .value(QStringLiteral("agents"))
                                     .toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject().value(QStringLiteral("arguments")).toArray();
        return QJsonArray{};
    };
    require(arguments(resumed) == QJsonArray{"--yolo", "--session", "old-conversation"},
            "restore is off unless the app asks for it");
    options.restoreAgents = true;
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "restore agents");
    require(arguments(resumed) == QJsonArray{"--yolo", "--session", "old-conversation"},
            "an explicit resume selection remains authoritative");
    require(arguments(fresh) == QJsonArray{"--yolo"},
            "a harness without a known resume option restarts fresh");
    require(arguments(foreign) == QJsonArray{},
            "a conversation recorded by another CLI is not passed on");
    for (const auto& id : {resumed, fresh, foreign}) {
        auto* item = workspace.session(id);
        require(waitFor([item] { return item->inputReady(); }, 10000),
                "a restored agent runs under a new service");
    }
    require(!workspace.restartAgent(resumed), "a running agent is not restarted");
    workspace.clearError();
    // An agent that ended restarts in its card without reopening lapis, once
    // its service has exited.
    auto* ended = workspace.session(fresh);
    ended->sendText("done\r");
    require(waitFor([ended] { return ended->connectionState() == QStringLiteral("ended"); }, 10000),
            "the agent ends");
    require(waitFor([&] { return workspace.restartAgent(fresh); }, 10000),
            "an ended agent restarts");
    workspace.clearError();
    require(waitFor([ended] { return ended->inputReady(); }, 10000),
            "the restarted agent runs again");
    for (const auto& id : {resumed, fresh, foreign})
        require(workspace.closeSession(id), "close a restored agent");
    require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "restored agents close");
    // Command-Shift-T brings the last closed agent back as a new tab, in its
    // folder and category, running again.
    require(workspace.canReopenAgent(), "closed agents can come back");
    require(workspace.reopenAgent() && workspace.sessions().size() == 1, "reopen the last one");
    auto* back = workspace.focusedSession();
    // The three closed in whichever order their processes ended.
    require(back != nullptr && QStringList({resumed, fresh, foreign}).contains(back->title()) &&
                back->directory() == canonical,
            "the reopened agent keeps its name and folder");
    require(waitFor([back] { return back->inputReady(); }, 10000), "the reopened agent runs");
    require(workspace.closeSession(back->sessionId()), "close it again");
    require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "the reopened agent closes");
}

void requireProcessArguments(const QJsonArray& arguments, const QString& directory) {
    QByteArray expected_output;
    for (const auto& value : arguments)
        expected_output += value.toString().toUtf8() + '\n';
    require(waitFor(
                [&] {
                    for (const auto& name :
                         QDir(directory).entryList({QStringLiteral("args.*.txt")}, QDir::Files)) {
                        QFile observed(QDir(directory).filePath(name));
                        if (observed.open(QIODevice::ReadOnly) &&
                            observed.readAll() == expected_output)
                            return true;
                    }
                    return false;
                },
                10000),
            "the restarted process received the saved resume arguments");
}

// A reopen's resume arguments and managed provenance must commit together
// before a process exists. A failed transaction also keeps the closed plan
// retryable instead of losing it behind a launch that never happened.
void reopenFailurePreservesTheRetryableManagedPlan() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-reopen-save-XXXXXX"));
    require(directory.isValid(), "reopen save-failure directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const auto script = QDir(canonical).filePath(QStringLiteral("agent.sh"));
    writeExecutable(script,
                    QByteArrayLiteral("#!/usr/bin/env bash\n"
                                      "printf '%s\\n' \"$@\" > \"$(dirname \"$0\")/args.$$.txt\"\n"
                                      "read -r -t 60 line\n"));
    const QString retained_id = QStringLiteral("ef715fac-a03a-45d4-8466-b0f2740c6b7b");
    const QString managed_id = uuid();
    auto retained = agentRecord(canonical, retained_id, "general");
    retained.insert(QStringLiteral("program"), script);
    retained.insert(QStringLiteral("harness"), QStringLiteral("kimi"));
    auto managed = agentRecord(canonical, managed_id, "closed");
    managed.insert(QStringLiteral("program"), script);
    managed.insert(QStringLiteral("harness"), QStringLiteral("kimi"));
    managed.insert(QStringLiteral("arguments"), QJsonArray{QStringLiteral("--user")});
    WorkspaceOptions options;
    options.restoreAgents = true;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{
            {"version", 2},
            {"activeCategory", "closed"},
            {"categories",
             QJsonArray{
                 QJsonObject{{"id", "general"}, {"name", "General"}, {"selected", retained_id}},
                 QJsonObject{{"id", "closed"}, {"name", "Closed"}, {"selected", managed_id}}}},
            {"agents", QJsonArray{retained, managed}}});
    writeObservedResume(QDir(canonical).filePath(managed_id + QStringLiteral(".sock")),
                        {QStringLiteral("kimi"), QStringLiteral("m-1")});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "load reopen save-failure fixture");
    auto* retained_item = workspace.session(retained_id);
    auto* managed_item = workspace.session(managed_id);
    require(waitFor(
                [retained_item, managed_item] {
                    return retained_item && managed_item && retained_item->inputReady() &&
                           managed_item->inputReady();
                },
                10000),
            "the reopen fixture starts");
    const auto saved_agent = [&](const QString& id) {
        for (const auto& value : QJsonDocument::fromJson(readRegistry(options.storagePath))
                                     .object()
                                     .value(QStringLiteral("agents"))
                                     .toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject();
        throw std::runtime_error("reopen fixture agent is missing");
    };
    const QJsonArray expected_arguments{QStringLiteral("--user"), QStringLiteral("--session"),
                                        QStringLiteral("m-1")};
    const auto initial = saved_agent(managed_id);
    require(initial.value(QStringLiteral("arguments")).toArray() == expected_arguments,
            "restore planning preserves user arguments around the managed pair");
    const auto initial_provenance = initial.value(QStringLiteral("managedResume")).toObject();
    require(initial_provenance.value(QStringLiteral("index")).toInt(-1) == 1 &&
                initial_provenance.value(QStringLiteral("identity")).toString() ==
                    QStringLiteral("m-1"),
            "the restored managed plan has provenance");
    requireProcessArguments(expected_arguments, canonical);
    for (const auto& name : QDir(canonical).entryList({QStringLiteral("args.*.txt")}, QDir::Files))
        require(QFile::remove(QDir(canonical).filePath(name)), "remove startup argv receipts");

    require(workspace.closeSession(managed_id) &&
                waitFor([&workspace] { return workspace.sessions().size() == 1; }, 10000),
            "close the managed reopen fixture");
    require(workspace.canReopenAgent(), "the closed managed plan is retryable");
    const auto previous_focus = workspace.focusedSession();
    const auto previous_category = workspace.activeCategoryId();
    const auto held_registry = QDir(canonical).filePath(QStringLiteral("registry-held"));
    require(QFile::rename(options.storagePath, held_registry), "move complete registry bytes");
    const auto complete_registry = readRegistry(held_registry);
    require(
        QDir().mkpath(options.storagePath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
        "replace the registry path with a directory");
    workspace.clearError();
    require(!workspace.reopenAgent(), "a failed reopen save does not report success");
    require(!workspace.workspaceError().isEmpty(), "a failed reopen names its save failure");
    require(workspace.canReopenAgent(), "a failed reopen remains retryable");
    require(workspace.sessions().size() == 1 && workspace.session(retained_id) == retained_item &&
                workspace.focusedSession() == previous_focus &&
                workspace.activeCategoryId() == previous_category,
            "a failed reopen preserves the existing session and selection");
    require(QDir(canonical).entryList({QStringLiteral("args.*.txt")}, QDir::Files).isEmpty(),
            "a failed reopen starts no agent process");
    require(readRegistry(held_registry) == complete_registry,
            "a failed reopen leaves complete registry bytes unchanged");

    require(QDir(options.storagePath).removeRecursively(), "remove the save-failure directory");
    require(QFile::rename(held_registry, options.storagePath), "restore complete registry bytes");
    QJsonObject first_published_record;
    QObject::connect(&workspace, &Workspace::sessionsChanged, &workspace, [&] {
        if (first_published_record.isEmpty() && workspace.sessions().size() == 2)
            first_published_record = saved_agent(workspace.focusedSession()->sessionId());
    });
    require(workspace.reopenAgent() && workspace.sessions().size() == 2,
            "a retryable reopen commits and starts");
    auto* reopened = workspace.focusedSession();
    require(reopened != nullptr && reopened->title() == managed_id &&
                workspace.activeCategoryId() == QStringLiteral("closed"),
            "a retried reopen restores its category and selection");
    const auto root = QJsonDocument::fromJson(readRegistry(options.storagePath)).object();
    const auto agents = root.value(QStringLiteral("agents")).toArray();
    require(agents.size() == 2 &&
                agents.first().toObject().value(QStringLiteral("id")).toString() == retained_id &&
                agents.last().toObject().value(QStringLiteral("id")).toString() ==
                    reopened->sessionId(),
            "a retried reopen appends without changing existing registry order");
    const auto reopened_record = agents.last().toObject();
    require(reopened_record.value(QStringLiteral("arguments")).toArray() == expected_arguments,
            "one save persists the full managed resume arguments");
    const auto provenance = reopened_record.value(QStringLiteral("managedResume")).toObject();
    require(first_published_record.value(QStringLiteral("managedResume")).toObject() ==
                initial_provenance,
            "the first published reopen already has durable managed provenance");
    require(provenance.value(QStringLiteral("index")).toInt(-1) == 1 &&
                provenance.value(QStringLiteral("identity")).toString() == QStringLiteral("m-1"),
            "one save persists managed provenance with the launch");
    requireProcessArguments(expected_arguments, canonical);
    require(waitFor([reopened] { return reopened->inputReady(); }, 10000),
            "the reopened process completes attachment before closing");

    require(workspace.closeSession(reopened->sessionId()), "close the retried agent");
    require(waitFor([&workspace] { return workspace.sessions().size() == 1; }, 10000),
            "the retried agent ends");
    require(workspace.closeSession(retained_id), "close the retained fixture agent");
    require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "reopen save-failure agents close");
}

// A launch already at the registry's durable argument cap must not receive an
// uncounted resume pair that makes the next startup reject the whole workspace.
void savedArgumentCapKeepsRegistryLoadable() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-resume-cap-XXXXXX"));
    require(directory.isValid(), "saved-argument-cap directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const auto script = QDir(canonical).filePath(QStringLiteral("agent.sh"));
    {
        QFile file(script);
        require(file.open(QIODevice::WriteOnly), "write saved-argument-cap script");
        file.write(
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$(dirname \"$0\")/args.$$.txt\"\nread line\n");
    }
    require(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make the saved-argument-cap script executable");
    const auto id = uuid();
    QJsonArray arguments;
    for (int index = 0; index < 64; ++index)
        arguments.append(QStringLiteral("argument-%1").arg(index));
    auto record = agentRecord(canonical, id, "general");
    record.insert(QStringLiteral("program"), script);
    record.insert(QStringLiteral("harness"), QStringLiteral("kimi"));
    record.insert(QStringLiteral("arguments"), arguments);
    WorkspaceOptions options;
    options.restoreAgents = true;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", QJsonArray{record}}});
    const auto endpoint = QDir(canonical).filePath(id + QStringLiteral(".sock"));
    writeObservedResume(endpoint, {QStringLiteral("kimi"), QStringLiteral("capped")});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "load an argument-capped registry");
    auto* item = workspace.session(id);
    require(item != nullptr && waitFor([item] { return item->inputReady(); }, 10000),
            "the capped restored agent starts");
    auto savedAgent = [&] {
        for (const auto& value : QJsonDocument::fromJson(readRegistry(options.storagePath))
                                     .object()
                                     .value(QStringLiteral("agents"))
                                     .toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject();
        throw std::runtime_error("capped agent is missing");
    }();
    require(savedAgent.value(QStringLiteral("arguments")).toArray() == arguments,
            "the durable argument cap prevents an uncounted managed append");
    require(savedAgent.value(QStringLiteral("managedResume")).toObject().isEmpty(),
            "an omitted resume append records no managed provenance");
    requireProcessArguments(arguments, canonical);
    item->sendText("done\r");
    require(waitFor([item] { return item->connectionState() == QStringLiteral("ended"); }, 10000),
            "the capped agent ends");
    require(waitFor([&] { return !QFileInfo::exists(endpoint); }, 1000),
            "the capped agent's old endpoint exits");
    require(workspace.restartAgent(id), "a capped launch can restart without the append");
    require(waitFor([item] { return item->inputReady(); }, 10000), "the capped agent restarts");
    savedAgent = [&] {
        for (const auto& value : QJsonDocument::fromJson(readRegistry(options.storagePath))
                                     .object()
                                     .value(QStringLiteral("agents"))
                                     .toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject();
        throw std::runtime_error("restarted capped agent is missing");
    }();
    require(savedAgent.value(QStringLiteral("arguments")).toArray() == arguments,
            "restart preserves the capped launch");
    require(workspace.closeSession(id) &&
                waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "the capped agent closes");
}

// Provenance, not an argument that merely looks like a resume option, says
// which pair lapis owns. A later checkpoint therefore follows each restart,
// while a user's explicit selection keeps its original identity.
void managedResumeFollowsRecovery() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-managed-resume-XXXXXX"));
    require(directory.isValid(), "managed-resume directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const auto script = QDir(canonical).filePath(QStringLiteral("agent.sh"));
    {
        QFile file(script);
        require(file.open(QIODevice::WriteOnly), "write the managed-resume script");
        file.write(
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$(dirname \"$0\")/args.$$.txt\"\nread line\n");
    }
    require(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make the managed-resume script executable");
    const QString managed = uuid();
    const QString explicit_agent = uuid();
    WorkspaceOptions options;
    options.restoreAgents = true;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    const auto endpoint = [&](const QString& id) {
        return QDir(canonical).filePath(id + QStringLiteral(".sock"));
    };
    const auto record = [&](const QString& id, const QJsonArray& arguments) {
        auto value = agentRecord(canonical, id, "general");
        value.insert(QStringLiteral("program"), script);
        value.insert(QStringLiteral("harness"), QStringLiteral("kimi"));
        value.insert(QStringLiteral("arguments"), arguments);
        return value;
    };

    // A stale index or identity loses only managed replacement; it cannot make
    // the workspace unloadable or silently replace user-owned arguments.
    auto malformed = record(managed, QJsonArray{QStringLiteral("--flag")});
    malformed.insert(QStringLiteral("managedResume"),
                     QJsonObject{{"index", 9}, {"identity", "m-1"}});
    const auto malformed_path = QDir(canonical).filePath(QStringLiteral("malformed.json"));
    writeRegistry(
        malformed_path,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", QJsonArray{malformed}}});
    {
        WorkspaceOptions bad_options;
        bad_options.storagePath = malformed_path;
        Workspace bad(WorkspaceMode::live, bad_options);
        require(bad.workspaceError().isEmpty(), "invalid provenance leaves the workspace usable");
        require(bad.renameSession(managed, QStringLiteral("recovered")),
                "save a workspace with stale provenance");
    }
    {
        const auto recovered = QJsonDocument::fromJson(readRegistry(malformed_path))
                                   .object()
                                   .value(QStringLiteral("agents"))
                                   .toArray()
                                   .first()
                                   .toObject();
        require(recovered.value(QStringLiteral("arguments")).toArray() ==
                    QJsonArray{QStringLiteral("--flag")},
                "stale provenance preserves user arguments");
        require(recovered.value(QStringLiteral("managedResume")).toObject().isEmpty(),
                "stale provenance is not republished");
    }

    writeRegistry(
        options.storagePath,
        QJsonObject{
            {"version", 2},
            {"activeCategory", "general"},
            {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
            {"agents", QJsonArray{record(managed, QJsonArray{QStringLiteral("--flag")}),
                                  record(explicit_agent,
                                         QJsonArray{QStringLiteral("--flag"),
                                                    QStringLiteral("--session=user-original")})}}});
    writeObservedResume(endpoint(managed), {QStringLiteral("kimi"), QStringLiteral("m-1")});
    writeObservedResume(endpoint(explicit_agent),
                        {QStringLiteral("kimi"), QStringLiteral("user-1")});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "load managed-resume registry");
    auto* managed_item = workspace.session(managed);
    auto* explicit_item = workspace.session(explicit_agent);
    require(waitFor(
                [managed_item, explicit_item] {
                    return managed_item->inputReady() && explicit_item->inputReady();
                },
                10000),
            "the first recovery cycle starts");

    const auto saved_agent = [&](const QString& id) {
        for (const auto& value : QJsonDocument::fromJson(readRegistry(options.storagePath))
                                     .object()
                                     .value(QStringLiteral("agents"))
                                     .toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject();
        throw std::runtime_error("saved managed-resume agent is missing");
    };
    const auto check_cycle = [&](const QString& id, bool managed_provenance,
                                 const QString& expected_identity) {
        const auto agent = saved_agent(id);
        const auto actual = agent.value(QStringLiteral("arguments")).toArray();
        const auto expected =
            managed_provenance
                ? QJsonArray{QStringLiteral("--flag"), QStringLiteral("--session"),
                             expected_identity}
                : QJsonArray{QStringLiteral("--flag"), QStringLiteral("--session=user-original")};
        require(actual == expected, "the recovery cycle preserves unrelated arguments");
        requireProcessArguments(expected, canonical);
        const auto provenance = agent.value(QStringLiteral("managedResume")).toObject();
        if (!managed_provenance) {
            require(provenance.isEmpty(), "an explicit resume pair remains user-owned");
            return;
        }
        require(provenance.value(QStringLiteral("index")).toInt() == 1 &&
                    provenance.value(QStringLiteral("identity")).toString() == expected_identity,
                "managed provenance names the replaced resume pair");
    };
    check_cycle(managed, true, QStringLiteral("m-1"));
    check_cycle(explicit_agent, false, {});

    for (const auto& [identity, managed_identity] : {std::pair{1, QStringLiteral("m-2")},
                                                     {2, QStringLiteral("m-3")},
                                                     {3, QStringLiteral("m-4")}}) {
        for (const auto& name :
             QDir(canonical).entryList({QStringLiteral("args.*.txt")}, QDir::Files))
            require(QFile::remove(QDir(canonical).filePath(name)), "remove prior argv receipts");
        if (identity == 3) {
            // Kimi has no lapis observer: its session hook's printed checkpoint
            // is how it names the conversation, and the managed pair follows it.
            lapis::session::write_resume_record(
                endpoint(managed),
                {QStringLiteral("kimi"), managed_identity, lapis::session::ResumeSource::terminal});
            lapis::session::write_resume_record(
                endpoint(explicit_agent), {QStringLiteral("kimi"), QStringLiteral("printed-id")});
        } else {
            writeObservedResume(endpoint(managed), {QStringLiteral("kimi"), managed_identity});
            writeObservedResume(
                endpoint(explicit_agent),
                {QStringLiteral("kimi"), QStringLiteral("user-") + QString::number(identity + 1)});
        }
        managed_item->sendText("done\r");
        explicit_item->sendText("done\r");
        require(waitFor(
                    [managed_item, explicit_item] {
                        return managed_item->connectionState() == QStringLiteral("ended") &&
                               explicit_item->connectionState() == QStringLiteral("ended");
                    },
                    10000),
                "the recovery cycle ends");
        require(waitFor([&] { return workspace.restartAgent(managed); }, 10000),
                "restart the managed agent after its old service exits");
        require(waitFor([&] { return workspace.restartAgent(explicit_agent); }, 10000),
                "restart the explicit agent after its old service exits");
        workspace.clearError();
        require(waitFor(
                    [managed_item, explicit_item] {
                        return managed_item->inputReady() && explicit_item->inputReady();
                    },
                    10000),
                "the recovery cycle restarts");
        check_cycle(managed, true, managed_identity);
        check_cycle(explicit_agent, false, {});
    }
    for (const auto& id : {managed, explicit_agent})
        require(workspace.closeSession(id), "close a managed-resume agent");
    require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "managed-resume agents close");
}

// Exercise restore planning without substituting a shell for a real agent.
// Destruction before the event loop cancels the queued launches.
struct ResumeArgumentsCase {
    bool saved{};
    bool explicit_resume{};
    bool symlink{};
    QJsonArray config_arguments{};
    bool add_update_setting{true};
    // Observed harnesses name their conversation to lapis's observer; a
    // checkpoint they printed never resumes it, and one saved before sources
    // were recorded does.
    lapis::session::ResumeSource source{lapis::session::ResumeSource::observer};
    QString harness{QStringLiteral("codex")};
    bool managed_pair{};
};

struct ResumeArgumentsFixture {
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-resume-XXXXXX")};
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const ScopedCodexHome codex_home{root.toUtf8()};
    const QString claude_home = QDir(root).filePath(QStringLiteral("claude-home"));
    const ScopedClaudeHome claude_home_scope{claude_home.toUtf8()};

    ResumeArgumentsFixture() {
        require(directory.isValid(), "private resume-planning directory");
        const auto sessions = QDir(root).filePath(QStringLiteral("sessions"));
        const auto date = QDir(sessions).filePath(QStringLiteral("2026/09/23"));
        const auto outside = QDir(root).filePath(QStringLiteral("outside"));
        require(QDir().mkpath(date) && QDir().mkpath(outside), "create rollout directories");
        require(QFile::link(outside, QDir(sessions).filePath(QStringLiteral("2026/09/24"))),
                "link an outside date directory");
    }
};

std::vector<ResumeArgumentsCase> resumeArgumentCases() {
    using lapis::session::ResumeSource;
    QJsonArray full_arguments;
    for (int index = 0; index < 63; ++index)
        full_arguments.append("literal-argument");
    return {
        ResumeArgumentsCase{true, false, false},
        ResumeArgumentsCase{true, true, false},
        ResumeArgumentsCase{false, true, false},
        ResumeArgumentsCase{false, false, false},
        ResumeArgumentsCase{true, false, true},
        ResumeArgumentsCase{
            false, false, false, {"--config=check_for_update_on_startup=true"}, false},
        ResumeArgumentsCase{
            false, false, false, {"-c", "check_for_update_on_startup=false"}, false},
        ResumeArgumentsCase{false, false, false, {"--", "check_for_update_on_startup=false"}},
        ResumeArgumentsCase{false, false, false, full_arguments, false},
        ResumeArgumentsCase{
            true, false, false, {}, true, ResumeSource::terminal, QStringLiteral("codex"), true},
        ResumeArgumentsCase{
            true, false, false, {}, false, ResumeSource::terminal, QStringLiteral("claude"), true},
        ResumeArgumentsCase{true, false, false, {}, true, ResumeSource::legacy},
    };
}

void writeResumeTranscript(const ResumeArgumentsFixture& fixture,
                           const ResumeArgumentsCase& variant, const QString& id) {
    if (variant.harness == QLatin1String("claude")) {
        const auto project =
            QDir(QDir(fixture.claude_home).filePath(QStringLiteral("projects"))).filePath(id);
        require(QDir().mkpath(project), "create a Claude transcript project");
        QFile transcript(QDir(project).filePath(id + QStringLiteral(".jsonl")));
        require(transcript.open(QIODevice::WriteOnly), "create a Claude transcript fixture");
        transcript.close();
        return;
    }
    const auto sessions = QDir(fixture.root).filePath(QStringLiteral("sessions"));
    const auto date = QDir(sessions).filePath(QStringLiteral("2026/09/23"));
    const auto directory = variant.symlink ? QDir(fixture.root).filePath(QStringLiteral("outside"))
                           : variant.saved ? date
                                           : sessions;
    const auto filename =
        QStringLiteral("rollout-2026-09-23T10-30-00-") + id + QStringLiteral(".jsonl");
    // An unsaved case has a root-level file outside the supported layout.
    QFile transcript(QDir(directory).filePath(filename));
    require(transcript.open(QIODevice::WriteOnly), "create transcript fixture");
    transcript.close();
}

QString resumePairOption(const QString& harness) {
    return harness == QLatin1String("codex") ? QStringLiteral("resume")
                                             : QStringLiteral("--resume");
}

QJsonArray retainedResumeArguments(const ResumeArgumentsCase& variant,
                                   const QJsonArray& user_arguments) {
    QJsonArray arguments = user_arguments;
    if (variant.managed_pair) {
        arguments.removeAt(arguments.size() - 1);
        arguments.removeAt(arguments.size() - 1);
    }
    return arguments;
}

QJsonObject resumeArgumentsRecord(const ResumeArgumentsFixture& fixture,
                                  const ResumeArgumentsCase& variant, const QString& id) {
    using lapis::session::ResumeSource;
    if (variant.managed_pair && variant.source != ResumeSource::terminal)
        throw std::runtime_error("a managed retirement fixture must use a terminal checkpoint");
    auto record = agentRecord(fixture.root, id, "general");
    record.insert(QStringLiteral("harness"), variant.harness);
    QJsonArray user_arguments = variant.config_arguments;
    user_arguments.append("--user");
    if (variant.explicit_resume) {
        user_arguments.append("resume");
        user_arguments.append("old-conversation");
    }
    if (variant.managed_pair) {
        user_arguments.append(resumePairOption(variant.harness));
        user_arguments.append("old-conversation");
        record.insert(QStringLiteral("managedResume"),
                      QJsonObject{{"index", user_arguments.size() - 2},
                                  {"identity", QStringLiteral("old-conversation")}});
    }
    record.insert(QStringLiteral("arguments"), user_arguments);
    return record;
}

void writeResumeRecord(const ResumeArgumentsCase& variant, const QString& id,
                       const QString& endpoint) {
    using lapis::session::ResumeSource;
    if (variant.source != ResumeSource::legacy) {
        lapis::session::write_resume_record(endpoint, {variant.harness, id, variant.source});
        return;
    }
    QFile legacy(endpoint + QStringLiteral(".resume"));
    const auto bytes =
        QJsonDocument(QJsonObject{{"version", 1}, {"agent", variant.harness}, {"session_id", id}})
            .toJson(QJsonDocument::Compact);
    require(legacy.open(QIODevice::WriteOnly) && legacy.write(bytes) == bytes.size() &&
                legacy.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
            "write a version 1 record");
}

bool followsObserverResume(const ResumeArgumentsCase& variant) {
    using lapis::session::ResumeSource;
    return variant.saved && !variant.explicit_resume && !variant.symlink &&
           variant.source != ResumeSource::terminal;
}

QJsonArray expectedResumeArguments(const ResumeArgumentsCase& variant,
                                   const QJsonArray& base_arguments,
                                   const QJsonArray& user_arguments, const QString& id) {
    QJsonArray expected;
    if (variant.add_update_setting)
        expected = {"-c", "check_for_update_on_startup=false"};
    const auto retained = variant.managed_pair ? base_arguments : QJsonArray(user_arguments);
    for (const auto& argument : retained)
        expected.append(argument);
    if (followsObserverResume(variant)) {
        expected.append("resume");
        expected.append(id);
    }
    return expected;
}

void requireRestoredResumeArguments(const ResumeArgumentsCase& variant,
                                    const ResumeArgumentsFixture& fixture,
                                    const QJsonObject& restored_agent, const QJsonArray& expected,
                                    const QString& id) {
    const auto endpoint = QDir(fixture.root).filePath(id + QStringLiteral(".sock"));
    const auto actual = restored_agent.value(QStringLiteral("arguments")).toArray();
    if (followsObserverResume(variant)) {
        const auto managed = restored_agent.value(QStringLiteral("managedResume")).toObject();
        require(managed.value(QStringLiteral("index")).toInt(-1) == 3 &&
                    managed.value(QStringLiteral("identity")).toString() == id,
                "Codex startup defaults shift managed provenance to the resume pair");
    }
    if (variant.managed_pair) {
        const auto managed = restored_agent.value(QStringLiteral("managedResume")).toObject();
        require(managed.isEmpty(),
                "a printed checkpoint for an observed CLI retires managed provenance");
    }
    require(actual == expected, "saved transcript lookup preserves explicit resume arguments");
    require(!QFileInfo::exists(endpoint), "restore planning has not spawned a service");
}

void codexResumeArguments() {
    ResumeArgumentsFixture fixture;
    for (const auto& variant : resumeArgumentCases()) {
        const auto id = uuid();
        writeResumeTranscript(fixture, variant, id);
        auto record = resumeArgumentsRecord(fixture, variant, id);
        const auto user_arguments = record.value(QStringLiteral("arguments")).toArray();
        const auto base_arguments = retainedResumeArguments(variant, user_arguments);
        const auto storage_path = QDir(fixture.root).filePath(id + QStringLiteral(".json"));
        const auto endpoint = QDir(fixture.root).filePath(id + QStringLiteral(".sock"));
        writeResumeRecord(variant, id, endpoint);
        writeRegistry(storage_path,
                      QJsonObject{{"version", 2},
                                  {"activeCategory", "general"},
                                  {"categories",
                                   QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                                  {"agents", QJsonArray{record}}});
        WorkspaceOptions options;
        options.storagePath = storage_path;
        options.restoreAgents = true;
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().isEmpty(), "plan restored launch");
        const auto restored_agent = QJsonDocument::fromJson(readRegistry(storage_path))
                                        .object()
                                        .value(QStringLiteral("agents"))
                                        .toArray()
                                        .first()
                                        .toObject();
        requireRestoredResumeArguments(
            variant, fixture, restored_agent,
            expectedResumeArguments(variant, base_arguments, user_arguments, id), id);
    }
}

void savedGrokDefaultsPreserveLaunchOwnership() {
    struct Case {
        QString program;
        QStringList arguments;
        bool add_fullscreen{};
        bool live{};
        bool managed{};
    };
    QStringList full;
    for (int index = 0; index < 64; ++index)
        full << QStringLiteral("literal");
    const std::vector<Case> cases{
        {"grok", {}, true},
        {"grok", {"--model", "chosen", "-r", "conversation"}, true, false, true},
        {"grok", {"--fullscreen", "--model", "chosen"}},
        {"grok", {"--fullscreen=false"}},
        {"grok", {"--no-fullscreen"}},
        {"grok", {"--", "--fullscreen"}, true},
        {"grok", full},
        {"grok", {}, false, true},
        // SSH policy has its own migration cases; this case checks that the
        // remote command itself never receives the local fullscreen default.
        {"ssh",
         {"-o", "ControlPath=none", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4",
          "-t", "fixture", "exec grok"}},
        {"custom-agent", {"--custom"}},
    };
    for (const auto& variant : cases) {
        QTemporaryDir directory(QStringLiteral("/tmp/lapis-grok-defaults-XXXXXX"));
        require(directory.isValid(), "Grok defaults directory");
        const QDir root(QFileInfo(directory.path()).canonicalFilePath());
        const auto id = uuid();
        auto record = agentRecord(root.path(), id, "general");
        const auto program = root.filePath(variant.program);
        writeExecutable(program, "#!/usr/bin/env bash\nexit 0\n");
        record.insert(QStringLiteral("program"), program);
        record.insert(QStringLiteral("harness"), QStringLiteral("grok"));
        record.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(variant.arguments));
        if (variant.managed)
            record.insert(QStringLiteral("managedResume"),
                          QJsonObject{{"index", 2}, {"identity", "conversation"}});
        WorkspaceOptions options;
        options.storagePath = root.filePath(QStringLiteral("workspace.json"));
        options.restoreAgents = true;
        writeRegistry(
            options.storagePath,
            {{"version", 2},
             {"activeCategory", "general"},
             {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
             {"agents", QJsonArray{record}}});
        QLocalServer listener;
        if (variant.live)
            require(listener.listen(record.value(QStringLiteral("endpoint")).toString()),
                    "existing service endpoint listens");
        auto expected = variant.arguments;
        if (variant.add_fullscreen)
            expected.prepend(QStringLiteral("--fullscreen"));
        // Without processing events, restore plans and persists the launch but
        // never starts the disposable executable. Repeat to check idempotence.
        for (int pass = 0; pass < 2; ++pass) {
            Workspace workspace(WorkspaceMode::live, options);
            require(workspace.workspaceError().isEmpty(), "restore saved Grok launch");
            const auto saved = QJsonDocument::fromJson(readRegistry(options.storagePath))
                                   .object()[QStringLiteral("agents")]
                                   .toArray()
                                   .first()
                                   .toObject();
            require(saved[QStringLiteral("arguments")].toArray() ==
                        QJsonArray::fromStringList(expected),
                    "startup migration preserves explicit argv and live/transport ownership");
            if (variant.managed)
                require(saved[QStringLiteral("managedResume")]
                                .toObject()[QStringLiteral("index")]
                                .toInt(-1) == 3,
                        "the owned resume pair shifts exactly once with the default");
        }
    }
}

struct PrintedCheckpointFixture {
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-cp-XXXXXX")};
    QString canonical = QFileInfo(directory.path()).canonicalFilePath();
    QString program;
    QString argv_path;
    QString id = uuid();
    WorkspaceOptions options;
    QString endpoint;

    [[nodiscard]] bool waitForArguments(const QByteArray& expected) const {
        // Service attachment can finish before the restarted child has
        // published its argv. Match the child's receipt, not input readiness.
        return waitFor(
            [&] {
                QFile file(argv_path);
                return file.open(QIODevice::ReadOnly) && file.readAll() == expected;
            },
            10000);
    }

    PrintedCheckpointFixture(const QString& harness, const QString& session_id,
                             const QJsonArray& arguments, bool legacy_record = false) {
        require(directory.isValid(), "checkpoint fixture directory");
        program = QDir(canonical).filePath(harness);
        argv_path = QDir(canonical).filePath(QStringLiteral("argv.txt"));
        const auto checkpoint =
            QJsonDocument(
                QJsonObject{{"agent", harness}, {"session_id", session_id}, {"source", "observer"}})
                .toJson(QJsonDocument::Compact)
                .toBase64();
        writeExecutable(program, QByteArray("#!/usr/bin/env bash\nprintf '%s\\n' \"$@\" > '") +
                                     QFile::encodeName(argv_path) +
                                     "'\nprintf '\\033]1337;SetUserVar=agent_checkpoint=" +
                                     checkpoint + "\\007!'\nread -r line\n");
        auto agent = agentRecord(canonical, id, "general");
        agent.insert(QStringLiteral("program"), program);
        agent.insert(QStringLiteral("harness"), harness);
        agent.insert(QStringLiteral("arguments"), arguments);
        options.storagePath = QDir(canonical).filePath(id + QStringLiteral(".json"));
        options.restoreAgents = true;
        writeRegistry(options.storagePath,
                      QJsonObject{{"version", 2},
                                  {"activeCategory", "general"},
                                  {"categories",
                                   QJsonArray{QJsonObject{
                                       {"id", "general"}, {"name", "General"}, {"selected", id}}}},
                                  {"agents", QJsonArray{agent}}});
        endpoint = QDir(canonical).filePath(id + QStringLiteral(".sock"));
        if (legacy_record) {
            QFile legacy(endpoint + QStringLiteral(".resume"));
            const auto bytes =
                QJsonDocument(
                    QJsonObject{{"version", 1}, {"agent", harness}, {"session_id", session_id}})
                    .toJson(QJsonDocument::Compact);
            require(legacy.open(QIODevice::WriteOnly) && legacy.write(bytes) == bytes.size() &&
                        legacy.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
                    "write a version 1 record");
        }
    }
};

// A CLI without a lapis observer names its conversation only in the
// checkpoint its session hook prints, so that checkpoint resumes it; printed
// output still cannot claim observer provenance or replace an identity the
// observer learned. Exercise the real service, persistence, and restart argv.
void printedCheckpointsResumeButNeverOverrideTheObserver() {
    PrintedCheckpointFixture fixture(QStringLiteral("kimi"), QStringLiteral("forged-conversation"),
                                     QJsonArray{QStringLiteral("--yolo")});
    Workspace workspace(WorkspaceMode::live, fixture.options);
    const auto id = fixture.id;
    auto* item = workspace.session(id);
    require(item != nullptr, "checkpoint fixture agent exists");
    const auto endpoint = fixture.endpoint;
    require(waitFor(
                [&] {
                    const auto record = lapis::session::read_resume_record(endpoint);
                    return item->inputReady() && record &&
                           record->session_id == QStringLiteral("forged-conversation");
                },
                10000),
            "service records the advisory checkpoint");
    const auto advisory_record = lapis::session::read_resume_record(endpoint);
    require(advisory_record && advisory_record->source == lapis::session::ResumeSource::terminal,
            "service does not trust a printed observer claim");
    item->sendText("done\r");
    require(waitFor([&] { return item->connectionState() == QStringLiteral("ended"); }, 10000),
            "checkpoint fixture exits");
    require(waitFor([&] { return workspace.restartAgent(id); }, 10000),
            "restart checkpoint fixture");
    require(waitFor([&] { return item->inputReady(); }, 10000), "checkpoint fixture restarts");
    require(fixture.waitForArguments("--yolo\n--session\nforged-conversation\n"),
            "a printed checkpoint resumes a CLI lapis has no observer for");
    item->sendText("done\r");
    require(waitFor([&] { return item->connectionState() == QStringLiteral("ended"); }, 10000),
            "advisory recovery fixture exits");
    writeObservedResume(endpoint, {QStringLiteral("kimi"), QStringLiteral("verified-id")});
    require(waitFor([&] { return workspace.restartAgent(id); }, 10000),
            "restart with an independently verified identity");
    require(waitFor([&] { return item->inputReady(); }, 10000), "verified fixture restarts");
    // Wait for the fixture's output to reach the service before checking that
    // the same printed OSC did not downgrade the pre-existing observer record.
    require(waitFor([&] { return item->snapshot().graphemes.find(U'!') != std::u32string::npos; },
                    10000),
            "service consumed the checkpoint output");
    const auto verified_record = lapis::session::read_resume_record(endpoint);
    require(verified_record && verified_record->source == lapis::session::ResumeSource::observer &&
                verified_record->session_id == QStringLiteral("verified-id"),
            "printed output cannot overwrite an observer checkpoint");
    require(fixture.waitForArguments("--yolo\n--session\nverified-id\n"),
            "only the verified identity becomes a resume argument");
    require(workspace.closeSession(id) &&
                waitFor([&] { return workspace.sessions().isEmpty(); }, 10000),
            "checkpoint fixture closes");

    // A legacy Claude identity predates source provenance, and its old launch
    // is terminal mode. Preserve the v1 record when its hook prints: restarts
    // continue to use the known conversation until an actual observer upgrades
    // it, while other CLIs still follow their checkpoints.
    PrintedCheckpointFixture legacy{
        QStringLiteral("claude"), QStringLiteral("legacy-conversation"), {}, true};
    const auto old_claude_config = qgetenv("CLAUDE_CONFIG_DIR");
    const auto restore_claude_config = qScopeGuard([&] {
        if (old_claude_config.isNull())
            qunsetenv("CLAUDE_CONFIG_DIR");
        else
            qputenv("CLAUDE_CONFIG_DIR", old_claude_config);
    });
    qputenv("CLAUDE_CONFIG_DIR", legacy.canonical.toUtf8());
    const auto project = QDir(legacy.canonical).filePath(QStringLiteral("projects/fixture"));
    require(QDir().mkpath(project), "create the private legacy transcript directory");
    QFile transcript(QDir(project).filePath(QStringLiteral("legacy-conversation.jsonl")));
    require(transcript.open(QIODevice::WriteOnly), "create a saved legacy conversation");
    transcript.close();
    const auto loaded = lapis::session::read_resume_record(legacy.endpoint);
    require(loaded && loaded->source == lapis::session::ResumeSource::legacy,
            "the fixture loaded a version 1 Claude record");
    Workspace legacy_workspace(WorkspaceMode::live, legacy.options);
    auto* legacy_item = legacy_workspace.session(legacy.id);
    require(legacy_item != nullptr, "legacy Claude fixture agent exists");
    require(waitFor(
                [legacy_item] {
                    return legacy_item->inputReady() &&
                           legacy_item->snapshot().graphemes.find(U'!') != std::u32string::npos;
                },
                10000),
            "the old Claude service consumed its printed checkpoint");
    const auto preserved = lapis::session::read_resume_record(legacy.endpoint);
    require(preserved && preserved->source == lapis::session::ResumeSource::legacy &&
                preserved->session_id == QStringLiteral("legacy-conversation"),
            "printed output preserves a legacy Claude checkpoint");
    const auto saved = QJsonDocument::fromJson(readRegistry(legacy.options.storagePath))
                           .object()
                           .value(QStringLiteral("agents"))
                           .toArray()
                           .first()
                           .toObject();
    require(saved.value(QStringLiteral("managedResume")).toObject() ==
                QJsonObject{{"index", 0}, {"identity", "legacy-conversation"}},
            "the legacy identity becomes a managed pair without explicit user resume arguments");
    legacy_item->sendText("done\r");
    require(
        waitFor([&] { return legacy_item->connectionState() == QStringLiteral("ended"); }, 10000),
        "the legacy Claude fixture exits");
    require(QFile::remove(legacy.argv_path), "remove the first legacy launch receipt");
    require(waitFor([&] { return legacy_workspace.restartAgent(legacy.id); }, 10000),
            "restart the legacy Claude fixture");
    require(waitFor(
                [&] {
                    QFile legacy_argv(legacy.argv_path);
                    return legacy_item->inputReady() && legacy_argv.open(QIODevice::ReadOnly) &&
                           legacy_argv.readAll() == QByteArray("--resume\nlegacy-conversation\n");
                },
                10000),
            "the legacy Claude identity remains valid after the restart");
    require(
        legacy_workspace.closeSession(legacy.id) &&
            waitFor([&legacy_workspace] { return legacy_workspace.sessions().isEmpty(); }, 10000),
        "the legacy Claude fixture closes");
}

// Opt-in, with LAPIS_TEST_CODEX_HOME naming a Codex home that uses a local
// fake model: a Codex agent whose service keeps no resume record (built before
// records) still gets one, from the rollout its app-server holds open.
void codexConversationRecoveredWithoutRecord() {
    const auto home = qEnvironmentVariable("LAPIS_TEST_CODEX_HOME");
    if (home.isEmpty())
        return;
    ScopedCodexHome codex_home_scope(home.toUtf8());
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-codex-XXXXXX"));
    require(directory.isValid(), "codex recovery directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const auto project = QDir(canonical).filePath(QStringLiteral("project"));
    require(QDir().mkpath(project), "project folder");
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    QString id;
    QString endpoint;
    {
        Workspace creator(WorkspaceMode::live, options);
        require(creator.createAgent(project, QStringLiteral("codex"), QStringLiteral("codex")),
                "create a Codex agent");
        auto* item = creator.focusedSession();
        id = item->sessionId();
        endpoint = QDir(canonical).filePath(id + QStringLiteral(".sock"));
        require(waitFor([item] { return item->inputReady(); }, 20000), "Codex is ready");
        waitFor([] { return false; }, 3000);
        item->sendText("hello there");
        waitFor([] { return false; }, 300);
        item->sendText("\r");
        require(waitFor([&] { return lapis::session::read_resume_record(endpoint).has_value(); },
                        20000),
                "the current service records the thread itself");
        waitFor([] { return false; }, 3000);
    }
    const auto expected = lapis::session::read_resume_record(endpoint);
    require(QFile::remove(endpoint + QStringLiteral(".resume")), "act like an older service");
    options.restoreAgents = true;
    Workspace reopened(WorkspaceMode::live, options);
    require(
        waitFor([&] { return lapis::session::read_resume_record(endpoint).has_value(); }, 20000),
        "lapis records the thread from the open rollout");
    const auto recovered = lapis::session::read_resume_record(endpoint);
    require(recovered && expected && recovered->session_id == expected->session_id,
            "the recovered thread is the agent's conversation");
    require(reopened.closeSession(id), "close the Codex agent");
    require(waitFor([&reopened] { return reopened.sessions().isEmpty(); }, 15000),
            "the Codex agent closes");
}

void closeEndsTheAgent(const char* script, int minimum_ms) {
    QTemporaryDir directory;
    require(directory.isValid(), "service directory");
    WorkspaceOptions options;
    options.endpoint = QDir(QFileInfo(directory.path()).canonicalFilePath())
                           .filePath(QStringLiteral("agent.sock"));
    options.launch = lapis::session::LaunchSpec{QStringLiteral("/bin/sh"),
                                                {QStringLiteral("-c"), QString::fromLatin1(script)},
                                                directory.path(),
                                                {80, 24},
                                                lapis::session::AgentMode::terminal};
    options.mode = lapis::session::wire::AttachMode::create;
    Workspace workspace(WorkspaceMode::live, options);
    auto* agent = workspace.focusedSession();
    require(agent != nullptr, "service-backed agent");
    require(waitFor([agent] { return agent->inputReady(); }, 10000), "agent session ready");
    QElapsedTimer clock;
    clock.start();
    require(workspace.closeSession(agent->sessionId()), "close request accepted");
    require(agent->closing() && agent->statusLabel() == QStringLiteral("Ending agent"),
            "the tab says the agent is ending");
    require(waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "the tab closes once the process has exited");
    require(clock.elapsed() >= minimum_ms, "escalation waited for the hangup first");
    require(workspace.workspaceError().isEmpty(), "no error on a normal close");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    try {
        if (argc > 1) {
            require(
                argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--case") &&
                    (QString::fromLocal8Bit(argv[2]) == QStringLiteral("remote-options") ||
                     QString::fromLocal8Bit(argv[2]) == QStringLiteral("accounts") ||
                     QString::fromLocal8Bit(argv[2]) == QStringLiteral("remote-accounts") ||
                     QString::fromLocal8Bit(argv[2]) == QStringLiteral("remote-account-reset") ||
                     QString::fromLocal8Bit(argv[2]) == QStringLiteral("startup-defaults") ||
                     QString::fromLocal8Bit(argv[2]) == QStringLiteral("launch-policy") ||
                     QString::fromLocal8Bit(argv[2]) == QStringLiteral("reload") ||
                     QString::fromLocal8Bit(argv[2]) == QStringLiteral("updater") ||
                     QString::fromLocal8Bit(argv[2]) == QStringLiteral("chimes")),
                "Usage: lapis_workspace_tests [--case "
                "remote-options|accounts|remote-account-reset|reload|updater|"
                "startup-defaults|launch-policy|chimes]");
            const auto selected = QString::fromLocal8Bit(argv[2]);
            if (selected == QStringLiteral("accounts")) {
                incompleteCodexHomeNeverStartsAnAgent();
                plansFollowTheirLoad();
            } else if (selected == QStringLiteral("remote-accounts")) {
                remoteAccountsRefuseBeforeReplacement();
            } else if (selected == QStringLiteral("remote-account-reset")) {
                remoteAccountsDropPreambleWithoutConfiguration();
            } else if (selected == QStringLiteral("remote-options")) {
                remoteOptionsRespectTheArgumentLimit();
                remoteClaudeReconnectsToItsConversation();
            } else if (selected == QStringLiteral("startup-defaults")) {
                savedGrokDefaultsPreserveLaunchOwnership();
            } else if (selected == QStringLiteral("launch-policy")) {
                modelessAgentsGetTheDefaultMode();
                resumingAConversationStartsItsCli();
            } else if (selected == QStringLiteral("updater")) {
                updaterLifecycle();
                updaterOutputIsDrainedWithABoundedTail();
                failedUpdaterStartClearsTheQueue();
                updateReloadsAgentsAfterTheirCli();
                startupAndManualUpdatesShareOneInstaller();
                skippedClaudeUpdateReportsTheCurrentOperation();
            } else if (selected == QStringLiteral("chimes")) {
                alertsChimeWhileAnAgentWaits();
                chimesPlayChosenFiles();
                attentionLogStaysPrivateAndRotates();
            } else {
                reloadStartsAgentsAgain();
                reloadFailuresRemainRetryable();
                batchReloadRetainsEarlierFailures();
            }
            std::cout << "selected workspace cases passed\n";
            return 0;
        }
        categoriesAndIdentity();
        projectPaths();
        explicitAgentIdentity();
        categoryCountsChangeOnlyWhenNeeded();
        persistence();
        restoreAgentIdentity();
        truthfulStatus();
        placeholderMatchesTerminalDefaults();
        discardOutsideActiveCategorySelectsNeighbor();
        failedWritesPreserveState();
        malformedAgentRegistry();
        unknownRegistryVersionsAreRejected();
        unseenFollowsTurnsAndSelection();
        latestAttentionGoesToTheNewest();
        tabGoesToTheReadyThenTheOldest();
        modelessAgentsGetTheDefaultMode();
        claudeAgentsUseServiceAdapter();
        agentArgumentsPersist();
        directTileSelectionNormalizesAStaleTarget();
        tileWalkDropsDisplacedAgentsThatLeave();
        tileWalkSurvivesAnotherCategory();
        outputEstimate();
        agentsStartWithoutParentSessionMarkers();
        restartRefusesClosingAgent();
        restoreSaveFailureStartsNoService();
        restoreProgramFallbackSeparatesTransportFromHarness();
        agentsRestoreAfterServiceLoss();
        reopenFailurePreservesTheRetryableManagedPlan();
        updaterLifecycle();
        updaterOutputIsDrainedWithABoundedTail();
        failedUpdaterStartClearsTheQueue();
        savedArgumentCapKeepsRegistryLoadable();
        managedResumeFollowsRecovery();
        printedCheckpointsResumeButNeverOverrideTheObserver();
        restartReportsValidationFailures();
        const bool had_codex_home = qEnvironmentVariableIsSet("CODEX_HOME");
        const auto original_codex_home = qgetenv("CODEX_HOME");
        codexResumeArguments();
        savedGrokDefaultsPreserveLaunchOwnership();
        codexConversationRecoveredWithoutRecord();
        require(qEnvironmentVariableIsSet("CODEX_HOME") == had_codex_home &&
                    qgetenv("CODEX_HOME") == original_codex_home,
                "Codex fixtures restore the caller's exact environment");
        closeEndsTheAgent("exec sleep 600", 0);
        // An agent that ignores SIGHUP is ended by the SIGTERM escalation.
        closeEndsTheAgent("trap '' HUP; exec sleep 600", 1200);
        closeOnHistoryPageEndsTheAgent();
        joinedViewStaysInSync();
        explicitLaunchesUseUpdaterPolicy();
        harnessesUpdateBeforeNewAgents();
        windowWaitsForTheRestoreHelper();
        phoneStartsAnAgentInItsCategory();
        remoteOptionsRespectTheArgumentLimit();
        remoteClaudeReconnectsToItsConversation();
        reloadStartsAgentsAgain();
        incompleteCodexHomeNeverStartsAnAgent();
        plansFollowTheirLoad();
        agentsStartAtTheStageSize();
        reloadFailuresRemainRetryable();
        batchReloadRetainsEarlierFailures();
        updateReloadsAgentsAfterTheirCli();
        startupAndManualUpdatesShareOneInstaller();
        skippedClaudeUpdateReportsTheCurrentOperation();
        resumingAConversationStartsItsCli();
        terminalsRunPlainShells();
        wheelReachesAFullScreenProgram();
        historyJumpsToTheStart();
        unseenAgentsDecodeNothing();
        windowTakesTheWorkspaceFromTheHost();
        alertsChimeWhileAnAgentWaits();
        chimesPlayChosenFiles();
        attentionLogStaysPrivateAndRotates();
        phoneSizeYieldsToTheDesktop();
        std::cout << "workspace categories, identity, persistence, status and closing passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
