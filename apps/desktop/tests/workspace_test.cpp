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
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using lapis::desktop::Workspace;
using lapis::desktop::WorkspaceMode;
using lapis::desktop::WorkspaceOptions;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
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
                                  {"program", "/bin/true"},
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
                        {"program", "/bin/true"},
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
    require(first.statusLabel() == QStringLiteral("Awaiting first prompt"),
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
    require(first.statusLabel() == QStringLiteral("Awaiting first prompt"),
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

QJsonObject agentRecord(const QString& directory, const QString& id, const char* category) {
    return QJsonObject{{"id", id},
                       {"title", id},
                       {"category", category},
                       {"endpoint", QDir(directory).filePath(id + QStringLiteral(".sock"))},
                       {"program", "/bin/true"},
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
                   {QStringLiteral("/bin/true"), {}, directory.path()},
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

// The session service ends the agent's process group; the tab closes after.
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
        categoryCountsChangeOnlyWhenNeeded();
        persistence();
        restoreAgentIdentity();
        truthfulStatus();
        placeholderMatchesTerminalDefaults();
        failedWritesPreserveState();
        malformedAgentRegistry();
        unseenFollowsTurnsAndSelection();
        claudeAgentsUseServiceAdapter();
        agentArgumentsPersist();
        outputEstimate();
        agentsStartWithoutParentSessionMarkers();
        closeEndsTheAgent("exec sleep 600", 0);
        // An agent that ignores SIGHUP is ended by the SIGTERM escalation.
        closeEndsTheAgent("trap '' HUP; exec sleep 600", 1200);
        std::cout << "workspace categories, identity, persistence, status and closing passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
