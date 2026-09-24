#include "agent_checkpoint.hpp"
#include "launch_spec.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using lapis::desktop::Workspace;
using lapis::desktop::WorkspaceMode;
using lapis::desktop::WorkspaceOptions;
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
    const auto bytes =
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
        require(!workspace.removeSession(first), "ended tab removal rolls back");
        unchanged();
        require(QDir().rename(moved, original), "restore registry parent");
        require(workspace.renameSession(first, QStringLiteral("Committed")),
                "write recovers when directory returns");
        require(first_object->title() == QStringLiteral("Committed") && notifications == 1,
                "successful rename publishes exactly one identity change");
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
    // The live Codex 0.155.1 observer reports this until the first turn.
    fresh.diagnostic = QStringLiteral("Waiting for Codex thread history");
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
    fresh.diagnostic = QStringLiteral("Reconciling Codex requests");
    first.applyAttention(fresh);
    require(first.statusLabel() == QStringLiteral("No prompt yet"),
            "an older service's one-second retry does not end the wait");
    lapis::desktop::SessionPreview reconnecting(QStringLiteral("Codex"), {}, {}, QColor{}, "");
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

void writeRegistry(const QString& path, const QJsonObject& root) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open registry fixture");
    const auto bytes = QJsonDocument(root).toJson();
    require(file.write(bytes) == bytes.size(), "write registry fixture");
}

// An agent that finishes a turn, or starts needing a response, while another
// is selected is marked until it is selected; the selected agent never is.
void unseenFollowsTurnsAndSelection() {
    Workspace workspace(WorkspaceMode::preview);
    require(workspace.selectSession(QStringLiteral("renderer")), "select renderer");
    int arrivals = 0;
    QObject::connect(&workspace, &Workspace::requestArrived, [&arrivals] { ++arrivals; });
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
    state.requests.emplace_back();
    checks->applyAttention(state);
    require(checks->unseen(), "a new request marks its agent");
    require(arrivals == 1, "a new request is announced once");
    require(count("general") == 1, "one unseen agent after the earlier one was selected");
    // Jumping to the waiting agent goes to the request first and clears it as seen.
    require(workspace.nextAttention() && workspace.focusedSession() == checks,
            "the next waiting agent is selected");
    require(!checks->unseen(), "jumping to an agent is looking at it");
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
    agent->reconnect();
    require(waitFor([agent] { return agent->inputReady(); }, 10000), "the desktop reattaches");
    phone.type("after reconnect\r");
    require(phone.waitForText(QStringLiteral("got:after reconnect"), 5000),
            "the joined view survives the desktop reattaching");
    require(workspace.closeSession(agent->sessionId()) &&
                waitFor([&workspace] { return workspace.sessions().isEmpty(); }, 10000),
            "the joined fixture closes");
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
    lapis::session::write_resume_record(endpoint(resumed),
                                        {QStringLiteral("kimi"), QStringLiteral("k-123")});
    lapis::session::write_resume_record(endpoint(foreign),
                                        {QStringLiteral("claude"), QStringLiteral("c-9")});
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

    // A stale index or identity cannot silently become launch provenance.
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
        require(!bad.workspaceError().isEmpty(), "invalid managed-resume provenance is rejected");
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
    lapis::session::write_resume_record(endpoint(managed),
                                        {QStringLiteral("kimi"), QStringLiteral("m-1")});
    lapis::session::write_resume_record(endpoint(explicit_agent),
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

    for (const auto& [identity, managed_identity] :
         {std::pair{1, QStringLiteral("m-2")}, {2, QStringLiteral("m-3")}}) {
        for (const auto& name :
             QDir(canonical).entryList({QStringLiteral("args.*.txt")}, QDir::Files))
            require(QFile::remove(QDir(canonical).filePath(name)), "remove prior argv receipts");
        lapis::session::write_resume_record(endpoint(managed),
                                            {QStringLiteral("kimi"), managed_identity});
        lapis::session::write_resume_record(
            endpoint(explicit_agent),
            {QStringLiteral("kimi"), QStringLiteral("user-") + QString::number(identity + 1)});
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

// Exercise restore planning without substituting a shell for a real Codex
// app-server. Destruction before the event loop cancels the queued launches.
void codexResumeArguments() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-resume-XXXXXX"));
    require(directory.isValid(), "private resume-planning directory");
    const auto root = QFileInfo(directory.path()).canonicalFilePath();
    const ScopedCodexHome home(root.toUtf8());
    const auto sessions = QDir(root).filePath(QStringLiteral("sessions"));
    const auto date = QDir(sessions).filePath(QStringLiteral("2026/09/23"));
    const auto outside = QDir(root).filePath(QStringLiteral("outside"));
    require(QDir().mkpath(date) && QDir().mkpath(outside), "create rollout directories");
    require(QFile::link(outside, QDir(sessions).filePath(QStringLiteral("2026/09/24"))),
            "link an outside date directory");
    struct Case {
        bool saved;
        bool explicit_resume;
        bool symlink;
    };
    for (const auto variant :
         {Case{true, false, false}, Case{true, true, false}, Case{false, true, false},
          Case{false, false, false}, Case{true, false, true}}) {
        const auto id = uuid();
        const auto filename =
            QStringLiteral("rollout-2026-09-23T10-30-00-") + id + QStringLiteral(".jsonl");
        // An unsaved case has a root-level file that is outside the supported layout.
        QFile transcript(QDir(variant.symlink ? outside
                              : variant.saved ? date
                                              : sessions)
                             .filePath(filename));
        require(transcript.open(QIODevice::WriteOnly), "create transcript fixture");
        transcript.close();
        auto record = agentRecord(root, id, "general");
        record.insert(QStringLiteral("harness"), QStringLiteral("codex"));
        const QJsonArray user_arguments = variant.explicit_resume
                                              ? QJsonArray{"--user", "resume", "old-conversation"}
                                              : QJsonArray{"--user"};
        record.insert(QStringLiteral("arguments"), user_arguments);
        WorkspaceOptions options;
        options.storagePath = QDir(root).filePath(id + QStringLiteral(".json"));
        options.restoreAgents = true;
        writeRegistry(options.storagePath,
                      QJsonObject{{"version", 2},
                                  {"activeCategory", "general"},
                                  {"categories",
                                   QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                                  {"agents", QJsonArray{record}}});
        const auto endpoint = QDir(root).filePath(id + QStringLiteral(".sock"));
        lapis::session::write_resume_record(endpoint, {QStringLiteral("codex"), id});
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().isEmpty(), "plan restored Codex launch");
        const auto actual = QJsonDocument::fromJson(readRegistry(options.storagePath))
                                .object()
                                .value(QStringLiteral("agents"))
                                .toArray()
                                .first()
                                .toObject()
                                .value(QStringLiteral("arguments"))
                                .toArray();
        const auto expected = variant.saved && !variant.explicit_resume && !variant.symlink
                                  ? QJsonArray{"--user", "resume", id}
                                  : user_arguments;
        require(actual == expected, "saved transcript lookup preserves explicit resume arguments");
        require(!QFileInfo::exists(endpoint), "restore planning has not spawned a service");
    }
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
        unseenFollowsTurnsAndSelection();
        claudeAgentsUseServiceAdapter();
        agentArgumentsPersist();
        outputEstimate();
        agentsStartWithoutParentSessionMarkers();
        restartRefusesClosingAgent();
        agentsRestoreAfterServiceLoss();
        managedResumeFollowsRecovery();
        restartReportsValidationFailures();
        const bool had_codex_home = qEnvironmentVariableIsSet("CODEX_HOME");
        const auto original_codex_home = qgetenv("CODEX_HOME");
        codexResumeArguments();
        codexConversationRecoveredWithoutRecord();
        require(qEnvironmentVariableIsSet("CODEX_HOME") == had_codex_home &&
                    qgetenv("CODEX_HOME") == original_codex_home,
                "Codex fixtures restore the caller's exact environment");
        closeEndsTheAgent("exec sleep 600", 0);
        // An agent that ignores SIGHUP is ended by the SIGTERM escalation.
        closeEndsTheAgent("trap '' HUP; exec sleep 600", 1200);
        closeOnHistoryPageEndsTheAgent();
        joinedViewStaysInSync();
        std::cout << "workspace categories, identity, persistence, status and closing passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
