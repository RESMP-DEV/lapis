#include "agent_checkpoint.hpp"
#include "alerts.hpp"
#include "config.hpp"
#include "conversation_owner.hpp"
#include "launch_spec.hpp"
#include "session_descriptor.hpp"
#include "terminals.hpp"
#include "transcript.hpp"
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
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
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
    using lapis::desktop::Agent;
    using lapis::session::AgentMode;
    for (const auto mode : {AgentMode::terminal, AgentMode::codex, AgentMode::claude}) {
        WorkspaceOptions options;
        options.endpoint = QDir(QFileInfo(directory.path()).canonicalFilePath())
                               .filePath(QStringLiteral("absent.sock"));
        options.mode = lapis::session::wire::AttachMode::reconnect;
        options.launch = lapis::session::LaunchSpec{
            QStringLiteral("/usr/bin/true"), {}, directory.path(), {80, 24}, mode};
        Workspace workspace(WorkspaceMode::live, options);
        const auto* item = workspace.focusedAgent();
        const QString expected = mode == AgentMode::codex    ? QStringLiteral("codex")
                                 : mode == AgentMode::claude ? QStringLiteral("claude")
                                                             : QString{};
        require(item && item->harnessId() == expected, "explicit attach retains harness identity");
        // Codex's notify program reports only finished turns.
        require(item->statusSource() == (mode == AgentMode::claude ? Agent::StatusSource::observer
                                                                   : Agent::StatusSource::output),
                "explicit attach selects the correct status source");
        require(!workspace.createAgent(directory.path(), QStringLiteral("Blocked")),
                "an explicit attach cannot create an unpersisted managed agent");
        require(!workspace.restartAgent(QStringLiteral("shell")),
                "an explicit attach cannot restart an unpersisted managed agent");
        require(workspace.workspaceError() ==
                    QStringLiteral("Restart requires a persisted workspace."),
                "explicit restart rejection names the persistence requirement");
        require(workspace.agents().size() == 1, "rejected creation leaves the attach intact");
    }
}
void projectPaths() {
    Workspace workspace(WorkspaceMode::fixture);
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
    Workspace workspace(WorkspaceMode::fixture);
    auto* original = workspace.agent(QStringLiteral("renderer"));
    require(!workspace.restartAgent(QStringLiteral("renderer")),
            "a preview cannot restart its sample agents");
    require(workspace.workspaceError() == QStringLiteral("Restart requires a persisted workspace."),
            "preview restart rejection names the persistence requirement");
    int list_changes = 0;
    QObject::connect(&workspace, &Workspace::agentsChanged, &workspace, [&] { ++list_changes; });
    require(workspace.selectAgent(QStringLiteral("renderer")), "select existing agent");
    require(list_changes == 0, "tab selection does not rebuild the tab model");
    require(!workspace.renameCategory(QStringLiteral("General"), QStringLiteral("general")),
            "reversed category arguments cannot resolve identity");
    require(!workspace.renameAgent(QStringLiteral("Render work"), QStringLiteral("renderer")),
            "reversed agent arguments cannot resolve identity");
    require(workspace.addCategory(QStringLiteral("Research")), "add category");
    const auto research = workspace.activeCategoryId();
    require(workspace.focusedAgent() == nullptr && workspace.categoryAgents().isEmpty(),
            "empty category must have no focused terminal");
    require(workspace.moveAgent(QStringLiteral("renderer"), research), "move existing agent");
    require(workspace.focusedAgent() == original, "move must preserve object identity");
    require(workspace.selectCategory(QStringLiteral("general")), "select original category");
    require(workspace.selectAgent(QStringLiteral("agent")), "select other agent");
    require(workspace.selectCategory(research), "return to research");
    require(workspace.focusedAgent() == original, "category restores selected agent");
    workspace.nextAgent(-1);
    require(workspace.focusedAgent() == original, "tab navigation cannot leave category");
    require(!workspace.removeCategory(research), "occupied category cannot be removed");
    require(workspace.renameAgent(QStringLiteral("renderer"), QStringLiteral("Render work")),
            "rename agent");
    require(original->title() == QStringLiteral("Render work"),
            "title changed without replacement");
    require(workspace.moveAgent(QStringLiteral("renderer"), QStringLiteral("general")),
            "return agent to general");
    require(workspace.removeCategory(research), "remove empty category");
    require(workspace.selectAgent(QStringLiteral("renderer")), "select moved agent");
    require(workspace.moveAgentBy(QStringLiteral("renderer"), -100), "reorder agent");
    require(workspace.categoryAgents().front().value<lapis::desktop::Agent*>() == original,
            "reorder within category");
    require(workspace.focusedAgent() == original, "reorder preserves selected identity");
    const auto selected = workspace.focusedAgent();
    require(workspace.replayAttention(QStringLiteral("arrival")), "request fixture");
    require(workspace.focusedAgent() == selected, "attention cannot steal focus");
    require(
        workspace.categories().front().toMap().value(QStringLiteral("attentionCount")).toInt() == 1,
        "category aggregates requests");
    require(!workspace.addCategory(QString(81, QLatin1Char('a'))), "bounded names");
}
QByteArray readRegistry(const QString& path);
void writeRegistry(const QString& path, const QJsonObject& root);
QString focusedId(const Workspace& workspace) {
    return workspace.focusedAgent() ? workspace.focusedAgent()->agentId() : QString{};
}
// Command-Shift-] and [ walk the sidebar: every category's agents in turn,
// across category edges, wrapping at the ends and passing empty categories.
void nextAgentWalksTheSidebar() {
    Workspace workspace(WorkspaceMode::fixture);
    // General: shell renderer agent; Build: (empty); Review: service checks;
    // Later: notes.
    require(workspace.addCategory(QStringLiteral("Build")), "add an empty category");
    const auto build = workspace.activeCategoryId();
    require(workspace.addCategory(QStringLiteral("Review")), "add a second category");
    const auto review = workspace.activeCategoryId();
    require(workspace.addCategory(QStringLiteral("Later")), "add a third category");
    const auto later = workspace.activeCategoryId();
    require(
        workspace.placeAgents({QStringLiteral("service"), QStringLiteral("checks")}, review, 0) &&
            workspace.moveAgent(QStringLiteral("notes"), later),
        "spread the agents over the categories");
    require(workspace.selectAgent(QStringLiteral("agent")), "start at General's last agent");
    QStringList visited;
    QStringList categories;
    for (int step = 0; step < 6; ++step) {
        workspace.nextAgentAcrossCategories(1);
        visited.append(focusedId(workspace));
        categories.append(workspace.activeCategoryId());
    }
    require(visited == QStringList{QStringLiteral("service"), QStringLiteral("checks"),
                                   QStringLiteral("notes"), QStringLiteral("shell"),
                                   QStringLiteral("renderer"), QStringLiteral("agent")},
            "forward crosses categories in sidebar order, skips the empty one and wraps");
    require(categories == QStringList{review, review, later, QStringLiteral("general"),
                                      QStringLiteral("general"), QStringLiteral("general")},
            "the active category follows the walk");
    workspace.nextAgentAcrossCategories(-1);
    require(focusedId(workspace) == QStringLiteral("renderer"), "back within a category");
    require(workspace.selectAgent(QStringLiteral("shell")), "start at the first agent");
    workspace.nextAgentAcrossCategories(-1);
    require(focusedId(workspace) == QStringLiteral("notes") &&
                workspace.activeCategoryId() == later,
            "back from the first agent wraps to the last category's last agent");
    workspace.nextAgentAcrossCategories(-1);
    require(focusedId(workspace) == QStringLiteral("checks") &&
                workspace.activeCategoryId() == review,
            "back crosses into the previous category's last agent");
    // From an empty category the walk starts beside it.
    require(workspace.selectCategory(build) && workspace.focusedAgent() == nullptr,
            "an empty category shows no agent");
    workspace.nextAgentAcrossCategories(1);
    require(focusedId(workspace) == QStringLiteral("service"),
            "forward from an empty category takes the next category's first agent");
    require(workspace.selectCategory(build), "back to the empty category");
    workspace.nextAgentAcrossCategories(-1);
    require(focusedId(workspace) == QStringLiteral("agent") &&
                workspace.activeCategoryId() == QStringLiteral("general"),
            "back from an empty category takes the previous category's last agent");
    // Each category keeps the agent the walk left it on.
    require(workspace.selectCategory(review) && focusedId(workspace) == QStringLiteral("service"),
            "a category remembers where the walk left it");
    // The in-category walk is unchanged.
    require(workspace.selectAgent(QStringLiteral("checks")), "select Review's last agent");
    workspace.nextAgent(1);
    require(focusedId(workspace) == QStringLiteral("service"),
            "nextAgent still wraps inside the category");
}
// Each category has a colour: by position, or chosen and saved by name.
void categoryColorsPersist() {
    {
        Workspace workspace(WorkspaceMode::fixture);
        for (const auto* name : {"A", "B", "C", "D", "E", "F", "G", "H"})
            require(workspace.addCategory(QString::fromLatin1(name)), "add categories");
        QStringList colors;
        for (const auto& value : workspace.categories())
            colors.append(value.toMap().value(QStringLiteral("color")).toString());
        require(colors.size() == 9, "nine categories");
        require(colors.mid(0, 8) == QStringList{QStringLiteral("blue"), QStringLiteral("green"),
                                                QStringLiteral("purple"), QStringLiteral("yellow"),
                                                QStringLiteral("cyan"), QStringLiteral("pink"),
                                                QStringLiteral("red"), QStringLiteral("grey")},
                "colours come by position");
        require(colors.at(8) == QStringLiteral("blue"), "the order cycles");
        const auto first = workspace.categories().front().toMap();
        require(first.value(QStringLiteral("agents")).toList().size() == 6 &&
                    first.value(QStringLiteral("agentCount")).toInt() == 6,
                "each category lists its agents in order");
        require(!workspace.setCategoryColor(QStringLiteral("general"), QStringLiteral("mauve")),
                "only palette names are chosen");
    }
    QTemporaryDir directory;
    require(directory.isValid(), "colour registry directory");
    WorkspaceOptions options;
    options.storagePath = QDir(directory.path()).filePath(QStringLiteral("workspace.json"));
    QString second;
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.addCategory(QStringLiteral("Research")), "add a category");
        second = workspace.activeCategoryId();
        require(workspace.setCategoryColor(second, QStringLiteral("pink")), "choose a colour");
        require(workspace.categories().at(1).toMap().value(QStringLiteral("color")).toString() ==
                    QStringLiteral("pink"),
                "the chosen colour shows");
    }
    const auto saved = [&] {
        return QJsonDocument::fromJson(readRegistry(options.storagePath))
            .object()
            .value(QStringLiteral("categories"))
            .toArray();
    };
    require(!saved().at(0).toObject().contains(QStringLiteral("color")),
            "a category without a chosen colour writes none");
    require(saved().at(1).toObject().value(QStringLiteral("color")).toString() ==
                QStringLiteral("pink"),
            "the chosen colour is saved by name");
    {
        Workspace restored(WorkspaceMode::live, options);
        require(restored.workspaceError().isEmpty(), "a registry with colours loads");
        const auto groups = restored.categories();
        require(groups.at(1).toMap().value(QStringLiteral("color")).toString() ==
                        QStringLiteral("pink") &&
                    groups.at(1).toMap().value(QStringLiteral("colorChoice")).toString() ==
                        QStringLiteral("pink"),
                "the chosen colour survives a restart");
        require(restored.setCategoryColor(second, {}), "clear the choice");
        require(restored.categories().at(1).toMap().value(QStringLiteral("color")).toString() ==
                    QStringLiteral("green"),
                "a cleared choice returns to the position's colour");
    }
    // A name this build does not know (a later build's, say) is kept as read
    // and shown as the position's colour; a registry without colours (an
    // earlier build's) loads unchanged.
    auto root = QJsonDocument::fromJson(readRegistry(options.storagePath)).object();
    auto groups = root.value(QStringLiteral("categories")).toArray();
    auto group = groups.at(1).toObject();
    group.insert(QStringLiteral("color"), QStringLiteral("orange"));
    groups.replace(1, group);
    auto first = groups.at(0).toObject();
    first.insert(QStringLiteral("color"), 7);
    groups.replace(0, first);
    root.insert(QStringLiteral("categories"), groups);
    writeRegistry(options.storagePath, root);
    {
        Workspace restored(WorkspaceMode::live, options);
        require(restored.workspaceError().isEmpty(), "an unknown colour never fails the registry");
        const auto shown = restored.categories();
        require(shown.at(0).toMap().value(QStringLiteral("color")).toString() ==
                        QStringLiteral("blue") &&
                    shown.at(1).toMap().value(QStringLiteral("color")).toString() ==
                        QStringLiteral("green"),
                "unknown and malformed colours fall back to the position's colour");
        require(restored.renameCategory(second, QStringLiteral("Renamed")), "save again");
    }
    require(saved().at(1).toObject().value(QStringLiteral("color")).toString() ==
                    QStringLiteral("orange") &&
                !saved().at(0).toObject().contains(QStringLiteral("color")),
            "an unknown name is written back as read; a malformed one is dropped");
    group = saved().at(1).toObject();
    group.remove(QStringLiteral("color"));
    groups = saved();
    groups.replace(1, group);
    root.insert(QStringLiteral("categories"), groups);
    writeRegistry(options.storagePath, root);
    {
        Workspace restored(WorkspaceMode::live, options);
        require(restored.workspaceError().isEmpty() && restored.categories().size() == 2 &&
                    restored.categories().at(1).toMap().value(QStringLiteral("color")).toString() ==
                        QStringLiteral("green"),
                "a registry without colours loads with colours by position");
    }
}
void categoryCountsChangeOnlyWhenNeeded() {
    Workspace workspace(WorkspaceMode::fixture);
    QObject receiver;
    int notifications = 0;
    QObject::connect(&workspace, &Workspace::categoriesChanged, &receiver,
                     [&] { ++notifications; });
    auto* agent = workspace.agent(QStringLiteral("agent"));
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
        require(workspace.agents().isEmpty() && !workspace.focusedAgent(),
                "normal workspace never launches shell or fixtures");
        require(
            !workspace.createAgent(directory.filePath(QStringLiteral("missing")), directory.path()),
            "invalid project directory cannot launch an agent");
        require(workspace.workspaceError().contains(QStringLiteral("project directory")),
                "invalid directory diagnostic does not depend on Codex installation");
        require(workspace.agents().isEmpty(), "invalid launch cannot add an agent tab");
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
        require(restored.agents().size() == 2, "restore both agents");
        require(restored.focusedAgent()->agentId() == second, "restore selected agent identity");
        require(restored.agent(second)->harnessId() == QStringLiteral("claude") &&
                    restored.agent(second)->agentName() == QStringLiteral("Claude"),
                "restore harness identity");
        require(restored.agent(first)->live(), "restore reconnect object");
        require(restored.agent(first)->harnessId() == QStringLiteral("codex"),
                "legacy registry remains Codex");
        require(!restored.removeAgent(first), "disconnected agent may still be running");
        require(restored.addCategory(QStringLiteral("Research")), "create destination category");
        require(restored.moveAgent(first, restored.activeCategoryId()), "persist category move");
        require(restored.focusedAgent()->agentId() == first,
                "select moved agent in empty category");
    }
    {
        Workspace restored(WorkspaceMode::live, options);
        require(restored.workspaceError().isEmpty(), "reload moved agents");
        require(restored.categoryAgents().size() == 1, "persist category membership");
        require(restored.focusedAgent()->agentId() == first, "persist per-category selection");
        require(restored.selectCategory(QStringLiteral("general")), "return original category");
        require(restored.focusedAgent()->agentId() == second,
                "preserve original category selection");
        require(restored.agent(second)->harnessId() == QStringLiteral("claude"),
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
        auto* first_object = workspace.agent(first);
        auto* second_object = workspace.agent(second);
        if (first_object == nullptr || second_object == nullptr)
            throw std::runtime_error("retain both session objects");
        first_object->setConnection(QStringLiteral("ended"), false);
        const auto categories = workspace.categories();
        const auto sessions = workspace.agents();
        const auto category_sessions = workspace.categoryAgents();
        int notifications = 0;
        const auto count = [&] { ++notifications; };
        QObject::connect(&workspace, &Workspace::categoriesChanged, &workspace, count);
        QObject::connect(&workspace, &Workspace::categoryChanged, &workspace, count);
        QObject::connect(&workspace, &Workspace::agentsChanged, &workspace, count);
        QObject::connect(&workspace, &Workspace::focusChanged, &workspace, count);
        QObject::connect(first_object, &lapis::desktop::Agent::identityChanged, &workspace, count);
        QObject::connect(second_object, &lapis::desktop::Agent::identityChanged, &workspace, count);
        require(QDir().rename(original, moved),
                "move registry parent to force real QSaveFile failure");
        const auto unchanged = [&] {
            require(workspace.categories() == categories && workspace.agents() == sessions &&
                        workspace.categoryAgents() == category_sessions,
                    "failed write preserves category metadata and object order");
            require(workspace.activeCategoryId() == QStringLiteral("general") &&
                        workspace.focusedAgent() == second_object,
                    "failed write preserves active category and selection");
            require(workspace.agent(first) == first_object &&
                        workspace.agent(second) == second_object && first_object->live() &&
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
        require(!workspace.selectAgent(first), "tab selection rolls back");
        unchanged();
        require(!workspace.renameAgent(first, QStringLiteral("Uncommitted")),
                "title rename rolls back");
        unchanged();
        require(!workspace.moveAgent(first, QStringLiteral("empty")), "category move rolls back");
        unchanged();
        require(!workspace.moveAgentBy(first, 1), "tab reorder rolls back");
        unchanged();
        require(!workspace.followConversationTitle(second, second),
                "conversation-title provenance rolls back on failed save");
        unchanged();
        require(!workspace.removeAgent(first), "ended tab removal rolls back");
        unchanged();
        require(QDir().rename(moved, original), "restore registry parent");
        require(workspace.renameAgent(first, QStringLiteral("Committed")),
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
        const QPointer<lapis::desktop::Agent> removed(first_object);
        require(workspace.removeAgent(first), "remove ended agent after save recovers");
        require(removed && !workspace.agent(first), "removed tab survives the QML call stack");
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
    // The malformed agent alone is skipped (and kept for a later lapis); the
    // categories it sat beside still come back.
    require(workspace.categories().size() == 1 &&
                workspace.categories().front().toMap().value(QStringLiteral("id")).toString() ==
                    QStringLiteral("foreign") &&
                workspace.agents().isEmpty(),
            "a malformed agent is skipped, not the workspace");
    require(workspace.agents().isEmpty() && !workspace.focusedAgent(),
            "malformed restore creates no session");
    require(workspace.addCategory(QStringLiteral("Later")), "the workspace still saves");
    const auto agents =
        QJsonDocument::fromJson(readRegistry(options.storagePath)).object().value("agents");
    require(agents.toArray() == QJsonArray{QJsonObject{{"id", "invalid"}}},
            "the malformed record is written back unchanged");
}
void placeholderMatchesTerminalDefaults() {
    const auto engine = lapis::session::Terminal({2, 2}).snapshot();
    lapis::desktop::Agent live(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    require(live.snapshot().background_rgb == engine.background_rgb &&
                live.snapshot().foreground_rgb == engine.foreground_rgb,
            "a live agent's placeholder uses the terminal defaults, not the sample palette");
    lapis::desktop::Agent fixture(QStringLiteral("Sample"), {}, {}, QColor{}, "sample");
    require(fixture.snapshot().background_rgb == 0x0d131dU, "fixtures keep the sample palette");
}
void discardOutsideActiveCategorySelectsNeighbor() {
    Workspace workspace(WorkspaceMode::fixture);
    require(workspace.addCategory(QStringLiteral("Research")), "destination category");
    const auto research = workspace.activeCategoryId();
    require(workspace.moveAgent(QStringLiteral("renderer"), research), "move first neighbor");
    require(workspace.moveAgent(QStringLiteral("agent"), research), "move second neighbor");
    require(workspace.moveAgent(QStringLiteral("service"), research), "move right neighbor");
    require(workspace.selectAgent(QStringLiteral("agent")), "select the future closed tab");
    require(workspace.selectCategory(QStringLiteral("general")), "show a different category");
    auto* agent = workspace.agent(QStringLiteral("agent"));
    require(agent != nullptr, "retain the closed preview fixture");
    agent->setConnection(QStringLiteral("ended"), false);
    require(workspace.removeAgent(QStringLiteral("agent")), "remove an inactive-category tab");
    require(workspace.selectCategory(research) &&
                workspace.focusedAgent() == workspace.agent(QStringLiteral("service")),
            "closing an unseen category remembers its own neighbor");
}
void truthfulStatus() {
    lapis::desktop::Agent item(QStringLiteral("Codex"), {}, {}, QColor{}, "");
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
    require(item.statusKind() == QStringLiteral("waiting") &&
                item.statusLabel() == QStringLiteral("Needs your answer"),
            "pending request has its own state and words");
    item.invalidateAttention();
    require(item.attentionCount() == 1, "stale requests remain visible for reconciliation");
    lapis::session::wire::AttentionSnapshot fresh;
    fresh.available = fresh.connected = true;
    // The producer owns the lifecycle phase; this diagnostic is display-only.
    fresh.diagnostic = QStringLiteral("Waiting for Codex thread history");
    fresh.observation_phase = lapis::session::attention::ObservationPhase::awaiting_first_prompt;
    lapis::desktop::Agent first(QStringLiteral("Codex"), {}, {}, QColor{}, "");
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
    lapis::desktop::Agent claude(QStringLiteral("Claude"), {}, {}, QColor{}, "");
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
    lapis::desktop::Agent legacy(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    legacy.applyAttention(fresh);
    require(legacy.statusLabel() == QStringLiteral("Status pending"),
            "legacy snapshots retain conservative unknown semantics");
    lapis::desktop::Agent reconnecting(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    fresh.observation_phase = lapis::session::attention::ObservationPhase::reconciling;
    reconnecting.applyAttention(fresh);
    require(reconnecting.statusLabel() == QStringLiteral("Status pending"),
            "reconciliation alone still reads as pending");
    require(item.statusKind() == QStringLiteral("unknown"), "stale activity must not look current");
}

void submittedAttentionKeepsItsDisplayToken() {
    lapis::desktop::Agent item(QStringLiteral("Codex"), {}, {}, QColor{}, "");
    item.setConnection(QStringLiteral("ready"), true);
    lapis::session::wire::AttentionSnapshot attention;
    attention.available = attention.connected = attention.ready = true;
    attention.activity = lapis::session::attention::Activity::working;
    lapis::session::wire::AttentionItem request;
    request.pending.request.id = std::string("uncertain");
    request.pending.source_epoch = 7;
    request.pending.revision = 9;
    request.pending.request.choices = {"accept"};
    attention.requests.push_back(request);
    int arrivals = 0;
    QObject::connect(&item, &lapis::desktop::Agent::attentionArrived, [&arrivals] { ++arrivals; });
    item.applyAttention(attention);
    require(arrivals == 1, "the original pending request arrives");
    const auto before = item.attentionRequests().first().toMap();
    request.pending.revision = 10;
    request.pending.submitted = true;
    request.pending.status = lapis::session::attention::RequestStatus::responding;
    attention.requests.front() = std::move(request);
    item.applyAttention(attention);
    const auto after = item.attentionRequests().first().toMap();
    require(arrivals == 1 &&
                after.value(QStringLiteral("token")) == before.value(QStringLiteral("token")),
            "a consumed response keeps the draft's stable display token");
    require(after.value(QStringLiteral("responding")).toBool() &&
                !after.value(QStringLiteral("enabled")).toBool(),
            "an uncertain consumed response stays held by the same request");
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
// No agent listed and none still closing: every close has ended its process.
bool closedFully(const Workspace& workspace) {
    return workspace.agents().isEmpty() && workspace.closingAgentIds().isEmpty();
}

// The choices Tab's move log beside a registry holds.
int loggedChoices(const QString& storagePath) {
    QFile log(QFileInfo(storagePath).dir().filePath(QStringLiteral("tab_away.jsonl")));
    if (!log.open(QIODevice::ReadOnly))
        return 0;
    int choices = 0;
    while (!log.atEnd())
        if (QJsonDocument::fromJson(log.readLine())
                .object()
                .value(QStringLiteral("event"))
                .toString() == QLatin1String("choice"))
            ++choices;
    return choices;
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
    Workspace workspace(WorkspaceMode::fixture);
    require(workspace.selectAgent(QStringLiteral("renderer")), "select renderer");
    int pings = 0;
    QObject::connect(&workspace, &Workspace::turnFinished, [&pings] { ++pings; });
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    const auto turn = [&](const char* id) {
        auto* item = workspace.agent(QString::fromLatin1(id));
        state.activity = lapis::session::attention::Activity::working;
        item->applyAttention(state);
        state.activity = lapis::session::attention::Activity::turn_completed;
        item->applyAttention(state);
        return item;
    };
    auto* agent = turn("agent");
    require(agent->unseen(), "an unselected agent's finished turn is marked");
    require(!turn("renderer")->unseen(), "the selected agent is already in view");
    require(!workspace.categories().front().toMap().contains(QStringLiteral("unseenCount")),
            "categories carry no unseen count: the window draws none");
    require(workspace.selectAgent(QStringLiteral("agent")) && !agent->unseen(),
            "selecting an agent clears its mark");
    require(workspace.selectAgent(QStringLiteral("renderer")), "select renderer again");
    auto* checks = workspace.agent(QStringLiteral("checks"));
    state.activity = lapis::session::attention::Activity::idle;
    checks->applyAttention(state);
    require(!checks->unseen(), "an agent that was not working has nothing new");
    const int pings_before = pings;
    state.requests.emplace_back();
    checks->applyAttention(state);
    require(checks->unseen(), "a new request marks its agent");
    require(pings == pings_before + 1, "a new request pings once, as a finished turn does");
    // Selecting the waiting agent clears it as seen.
    require(workspace.selectAgent(QStringLiteral("checks")) && workspace.focusedAgent() == checks,
            "the waiting agent is selected");
    require(!checks->unseen() && checks->attentionPending(),
            "selecting an agent is looking at it, though its request is still pending");
}

// The attention mark's state machine (Workspace::noteStatus): one mark per
// finished turn or request unless the person is looking then; looking by any
// path clears it for good; republished, reconnected and restored states never
// mark a turn already seen. `window` stands for the window being active with
// someone at the Mac.
struct MarkProbe {
    explicit MarkProbe(Workspace& workspace) : workspace_(workspace) {
        workspace.setLooking([this](const lapis::desktop::Agent* item) {
            return window && item == workspace_.focusedAgent();
        });
        workspace.setMarkLog([this](const QJsonObject& line) { log.push_back(line); });
        QObject::connect(&workspace, &Workspace::needsYou, &context_,
                         [this](lapis::desktop::Agent* item, const QString& outcome) {
                             outcomes.push_back(item->agentId() + ':' + outcome);
                         });
    }
    ~MarkProbe() {
        workspace_.setLooking({});
        workspace_.setMarkLog({});
    }
    MarkProbe(const MarkProbe&) = delete;
    MarkProbe& operator=(const MarkProbe&) = delete;
    // A service restart: the connection drops, and its requests come back
    // under a new source epoch.
    void reconnect(const char* id) {
        workspace_.agent(QString::fromLatin1(id))->invalidateAttention();
        ++snapshot.source_epoch;
        for (auto& item : snapshot.requests)
            item.pending.source_epoch = snapshot.source_epoch;
        state(id, snapshot.activity);
    }
    void state(const char* id, lapis::session::attention::Activity activity) {
        snapshot.activity = activity;
        workspace_.agent(QString::fromLatin1(id))->applyAttention(snapshot);
    }
    void finish(const char* id) {
        state(id, lapis::session::attention::Activity::working);
        state(id, lapis::session::attention::Activity::turn_completed);
    }
    [[nodiscard]] int marked(const char* id) const {
        return static_cast<int>(std::count(outcomes.begin(), outcomes.end(),
                                           QString::fromLatin1(id) + QStringLiteral(":marked")));
    }
    [[nodiscard]] bool unseen(const char* id) const {
        return workspace_.agent(QString::fromLatin1(id))->unseen();
    }
    Workspace& workspace_;
    QObject context_;
    bool window{true};
    QStringList outcomes;
    std::vector<QJsonObject> log;
    lapis::session::wire::AttentionSnapshot snapshot{[] {
        lapis::session::wire::AttentionSnapshot value;
        value.available = value.connected = value.ready = true;
        value.source_epoch = 1;
        return value;
    }()};
};

void attentionMarksFollowLooking() {
    using lapis::session::attention::Activity;
    Workspace workspace(WorkspaceMode::fixture);
    MarkProbe probe(workspace);
    require(workspace.selectAgent(QStringLiteral("renderer")), "renderer on the stage");

    // Off the stage: marked once; republished and repeated states add nothing.
    probe.finish("agent");
    require(probe.unseen("agent") && probe.marked("agent") == 1, "a turn off the stage marks once");
    probe.state("agent", Activity::turn_completed);
    probe.state("agent", Activity::idle);
    require(probe.marked("agent") == 1, "a republished or idle status is no new turn");

    // On the stage with the window in front: seen as it happens, no chime.
    probe.finish("renderer");
    require(!probe.unseen("renderer") &&
                probe.outcomes.back() == QStringLiteral("renderer:looking"),
            "the agent being looked at is seen at once");

    // On the stage with the window behind another app: nobody looked.
    probe.window = false;
    probe.finish("renderer");
    require(probe.unseen("renderer") && probe.marked("renderer") == 1,
            "the stage agent is marked while the window is not in front");
    // Moving by hand while away (the phone) is not looking either.
    require(workspace.selectAgent(QStringLiteral("agent")) && probe.unseen("agent"),
            "a selection nobody sees clears nothing");
    // Back at the window: the agent on the stage is seen, the other is not.
    probe.window = true;
    workspace.noteLooking(QStringLiteral("window"));
    require(!probe.unseen("agent") && probe.unseen("renderer"),
            "returning to the window sees the stage agent only");
    // A sidebar click is selectAgent; it clears the mark for good.
    require(workspace.selectAgent(QStringLiteral("renderer")) && !probe.unseen("renderer"),
            "showing the agent clears its mark");
    require(workspace.selectAgent(QStringLiteral("shell")), "leave it");
    probe.state("renderer", Activity::turn_completed);
    require(!probe.unseen("renderer"), "a republished finished turn does not come back");

    // A reconnect: unknown in between is not settled; the same turn stays seen,
    // and a turn that ended while unreachable counts once.
    auto* renderer = workspace.agent(QStringLiteral("renderer"));
    renderer->invalidateAttention();
    probe.state("renderer", Activity::turn_completed);
    require(!probe.unseen("renderer") && probe.marked("renderer") == 1,
            "a reconnect republishing a seen turn marks nothing");
    probe.state("renderer", Activity::working);
    renderer->invalidateAttention();
    probe.state("renderer", Activity::turn_completed);
    require(probe.unseen("renderer") && probe.marked("renderer") == 2,
            "a turn that ended while unreachable marks once");
    renderer->invalidateAttention();
    probe.state("renderer", Activity::turn_completed);
    require(probe.marked("renderer") == 2, "and only once");

    // The cross-category walk shows an agent in another category: seen.
    const auto other = workspace.createCategory(QStringLiteral("Other"));
    require(!other.isEmpty() && workspace.moveAgent(QStringLiteral("notes"), other), "notes moved");
    require(workspace.selectAgent(QStringLiteral("checks")), "checks on the stage");
    probe.finish("notes");
    require(probe.unseen("notes"), "an agent in another category is not being looked at");
    workspace.nextAgentAcrossCategories(1);
    require(workspace.focusedAgent() == workspace.agent(QStringLiteral("notes")) &&
                workspace.activeCategoryId() == other && !probe.unseen("notes"),
            "the walk across categories is looking too");

    // Requests: once per request; one already marked and back after a
    // reconnect or with a new revision marks nothing; one arriving while the
    // person looks is seen.
    require(workspace.selectAgent(QStringLiteral("shell")), "shell on the stage");
    lapis::session::wire::AttentionItem asked;
    asked.pending.request.id = std::int64_t{7};
    asked.pending.request.reason = "approval";
    asked.pending.source_epoch = asked.pending.revision = 1;
    probe.snapshot.requests = {asked};
    probe.state("checks", Activity::working);
    require(probe.unseen("checks") && probe.marked("checks") == 1, "a request marks once");
    require(workspace.selectAgent(QStringLiteral("checks")) && !probe.unseen("checks"),
            "looking at it clears the mark though the request is still open");
    require(workspace.selectAgent(QStringLiteral("shell")), "leave it");
    auto* checks = workspace.agent(QStringLiteral("checks"));
    checks->invalidateAttention();
    probe.snapshot.source_epoch = 2;
    probe.snapshot.requests.front().pending.source_epoch = 2;
    probe.snapshot.requests.front().pending.revision = 2;
    probe.state("checks", Activity::working);
    require(!probe.unseen("checks") && probe.outcomes.back() == QStringLiteral("checks:repeat"),
            "a request back after a reconnect is the one already seen");
    probe.snapshot.requests.clear();
    probe.state("checks", Activity::working);
    asked.pending.request.id = std::int64_t{8};
    probe.snapshot.requests = {asked};
    require(workspace.selectAgent(QStringLiteral("checks")), "checks on the stage");
    probe.state("checks", Activity::working);
    require(!probe.unseen("checks") && probe.outcomes.back() == QStringLiteral("checks:looking"),
            "a request that arrives while the person looks is seen");
    probe.snapshot.requests.clear();
    probe.snapshot.source_epoch = 1;

    // Nothing new on the screen since the person looked: no mark.
    workspace.setUnchanged([](const lapis::desktop::Agent*) { return true; });
    require(workspace.selectAgent(QStringLiteral("shell")), "shell on the stage");
    probe.finish("checks");
    require(!probe.unseen("checks") && probe.outcomes.back() == QStringLiteral("checks:unchanged"),
            "a turn ending on a screen already seen marks nothing");
    workspace.setUnchanged({});

    // The log names agents by id only, with why each mark came and went.
    const auto logged = [&](const char* event, const char* why) {
        return std::any_of(probe.log.begin(), probe.log.end(), [&](const QJsonObject& line) {
            return line.value(QStringLiteral("event")).toString() == QLatin1String(event) &&
                   line.value(QStringLiteral("why")).toString() == QLatin1String(why) &&
                   line.value(QStringLiteral("kind")).toString() == QStringLiteral("mark") &&
                   !line.value(QStringLiteral("id")).toString().isEmpty() &&
                   !line.contains(QStringLiteral("agent")) &&
                   !line.contains(QStringLiteral("title"));
        });
    };
    require(logged("unseen", "finished") && logged("unseen", "request") &&
                logged("seen", "focus") && logged("seen", "window") &&
                logged("seen", "looking: finished"),
            "each mark set and cleared is logged with why, by id only");
}

// Tab follows the same mark: a turn the person looked at, its guess seen too,
// waits no longer; an unseen turn or a guess not yet seen does.
void tabSkipsTurnsAlreadySeen() {
    using lapis::session::attention::Activity;
    Workspace workspace(WorkspaceMode::fixture);
    workspace.setTabAway({});
    MarkProbe probe(workspace);
    require(workspace.selectAgent(QStringLiteral("renderer")), "renderer on the stage");
    probe.finish("agent");
    require(workspace.nextPriorityAttention({}) &&
                workspace.focusedAgent() == workspace.agent(QStringLiteral("agent")),
            "an unseen turn is waiting");
    require(workspace.selectAgent(QStringLiteral("renderer")), "back");
    require(!workspace.nextPriorityAttention({{QStringLiteral("agent"), true}}),
            "a turn looked at with its guess seen is not waiting");
    require(workspace.nextPriorityAttention({{QStringLiteral("agent"), false}}),
            "a guess not yet seen is");
}

// Marks survive a restart: a turn seen before stays seen, an unseen one stays
// unseen (the stage agent's too, until the person looks), and a turn that
// ended while no window ran is marked once, however long ago it was saved.
void attentionMarksSurviveARestart() {
    using lapis::session::attention::Activity;
    QJsonObject marks;
    {
        Workspace before(WorkspaceMode::fixture);
        MarkProbe probe(before);
        require(before.selectAgent(QStringLiteral("renderer")), "renderer on the stage");
        probe.finish("agent");   // unseen
        probe.finish("checks");  // seen below
        probe.finish("service"); // the stage agent's, while away
        require(before.selectAgent(QStringLiteral("checks")), "look at checks");
        probe.window = false;
        require(before.selectAgent(QStringLiteral("service")), "service on the stage, away");
        probe.state("notes", Activity::working); // at work when the window closed
        marks = before.saveMarks();
    }
    // Saved an hour ago: an absence longer than an install's restart.
    marks.insert(QStringLiteral("savedAtMs"),
                 QDateTime::currentMSecsSinceEpoch() - qint64{60} * 60 * 1000);
    Workspace after(WorkspaceMode::fixture);
    MarkProbe probe(after);
    probe.window = false; // the window is not in front yet
    int away = 0;
    QObject::connect(&after, &Workspace::finishedWhileAway, [&away] { ++away; });
    require(after.selectAgent(QStringLiteral("service")), "service on the stage again");
    after.restoreMarks(marks);
    require(probe.unseen("agent") && !probe.unseen("checks") && probe.unseen("service"),
            "seen stays seen, unseen stays unseen, the stage agent's included");
    // The services report the turns they still hold: nothing new.
    probe.state("agent", Activity::turn_completed);
    probe.state("checks", Activity::turn_completed);
    require(!probe.unseen("checks") && probe.outcomes.isEmpty(), "republished turns mark nothing");
    // The agent that was at work finished while the window was closed.
    probe.state("notes", Activity::turn_completed);
    require(probe.unseen("notes") && away == 0,
            "a turn that ended while no window ran is marked, without a late guess");
    probe.state("notes", Activity::turn_completed);
    require(std::count_if(probe.log.begin(), probe.log.end(),
                          [](const QJsonObject& line) {
                              return line.value(QStringLiteral("id")).toString() ==
                                         QStringLiteral("notes") &&
                                     line.value(QStringLiteral("event")).toString() ==
                                         QStringLiteral("unseen");
                          }) == 1,
            "once");
    probe.window = true;
    after.noteLooking(QStringLiteral("window"));
    require(!probe.unseen("service"), "the window in front sees the stage agent's restored mark");
}

// A replay shaped like the worst sequence in the owner's logs (counts and
// order, no content): an agent finishing on the stage while lapis is behind
// another app, the person returning, statuses republished and reconnected,
// the cross-category walk and a sidebar click onto finished agents, the same
// request delivered again after a reconnect, the agent waking itself after a
// look, and an install's restart. Each turn or request marks at most once and
// nothing seen is marked again.
void attentionReplayMarksEachTurnOnce() {
    using lapis::session::attention::Activity;
    QJsonObject marks;
    QStringList outcomes;
    {
        Workspace workspace(WorkspaceMode::fixture);
        MarkProbe probe(workspace);
        const auto other = workspace.createCategory(QStringLiteral("Research"));
        require(workspace.moveAgent(QStringLiteral("service"), other) &&
                    workspace.moveAgent(QStringLiteral("notes"), other),
                "two agents in another category");
        require(workspace.selectAgent(QStringLiteral("agent")), "agent on the stage");
        // Behind another app: the stage agent and two others finish.
        probe.window = false;
        probe.finish("agent");
        probe.finish("service");
        probe.finish("checks");
        // Back: the stage agent is seen.
        probe.window = true;
        workspace.noteLooking(QStringLiteral("window"));
        // Status republished five times, a reconnect, republished again.
        for (int i = 0; i < 5; ++i)
            probe.state("service", Activity::turn_completed);
        workspace.agent(QStringLiteral("service"))->invalidateAttention();
        probe.state("service", Activity::turn_completed);
        // The walk reaches the other category's agents; a click shows checks.
        while (workspace.focusedAgent() != workspace.agent(QStringLiteral("service")))
            workspace.nextAgentAcrossCategories(1);
        require(workspace.selectAgent(QStringLiteral("checks")), "click checks");
        // A request on notes, then the same request again after a reconnect.
        lapis::session::wire::AttentionItem asked;
        asked.pending.request.id = std::string("permission:1");
        asked.pending.request.reason = "approval";
        asked.pending.source_epoch = asked.pending.revision = 1;
        probe.snapshot.requests = {asked};
        probe.state("notes", Activity::working);
        probe.reconnect("notes");
        require(workspace.selectAgent(QStringLiteral("notes")), "look at notes");
        probe.reconnect("notes");
        probe.snapshot.requests.clear();
        probe.snapshot.source_epoch = 1;
        // Agent wakes itself after the look: a new turn, marked once.
        require(workspace.selectAgent(QStringLiteral("checks")), "away from agent");
        probe.finish("agent");
        probe.state("agent", Activity::turn_completed);
        probe.state("agent", Activity::working); // at work as the window restarts
        marks = workspace.saveMarks();
        outcomes = probe.outcomes;
        require(probe.marked("agent") == 2 && probe.marked("service") == 1 &&
                    probe.marked("checks") == 1 && probe.marked("notes") == 1,
                "each turn and request marked once");
        require(!probe.unseen("service") && !probe.unseen("checks") && !probe.unseen("notes"),
                "everything looked at is seen");
        require(std::count(outcomes.begin(), outcomes.end(), QStringLiteral("notes:repeat")) == 2,
                "the request back after reconnects is a repeat");
    }
    Workspace after(WorkspaceMode::fixture);
    MarkProbe probe(after);
    const auto other = after.createCategory(QStringLiteral("Research"));
    require(after.moveAgent(QStringLiteral("service"), other) &&
                after.moveAgent(QStringLiteral("notes"), other),
            "the same places after the restart");
    require(after.selectAgent(QStringLiteral("checks")), "checks on the stage");
    after.restoreMarks(marks);
    for (const auto* id : {"service", "checks", "notes"})
        probe.state(id, Activity::turn_completed);
    require(!probe.unseen("service") && !probe.unseen("checks") && !probe.unseen("notes") &&
                probe.outcomes.isEmpty(),
            "after the restart nothing seen comes back");
    probe.state("agent", Activity::turn_completed);
    require(probe.unseen("agent"), "the turn that ended during the restart is marked");
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

// Tab's next agent: a guess not yet seen or a turn finished unseen. A turn
// looked at whose guess was seen too waits no longer, so Tab cannot bounce
// between guesses; an agent at work is not waiting even with a guess.
void tabGoesToTheReadyThenTheOldest() {
    Workspace workspace(WorkspaceMode::fixture);
    workspace.setTabAway({});
    require(workspace.selectAgent(QStringLiteral("renderer")), "select renderer");
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    const auto finish = [&](const char* id) {
        auto* item = workspace.agent(QString::fromLatin1(id));
        state.activity = lapis::session::attention::Activity::working;
        item->applyAttention(state);
        state.activity = lapis::session::attention::Activity::turn_completed;
        item->applyAttention(state);
        QThread::msleep(5);
        return item;
    };
    auto* older = finish("agent");
    auto* newer = finish("checks");
    require(workspace.nextPriorityAttention({{newer->agentId(), false}}) &&
                workspace.focusedAgent() == newer,
            "the agent with a guess not yet seen goes first");
    const QVariantMap seen{{newer->agentId(), true}};
    require(workspace.nextPriorityAttention(seen) && workspace.focusedAgent() == older,
            "then the one whose turn finished unseen");
    require(!workspace.nextPriorityAttention(seen),
            "a turn looked at with its guess seen is out of the rotation");
    auto* third = finish("renderer");
    require(workspace.nextPriorityAttention(seen) && workspace.focusedAgent() == third,
            "a new unseen turn is waiting");
    require(workspace.selectAgent(older->agentId()), "back to the older one");
    const QVariantMap fresh{{newer->agentId(), false}};
    state.activity = lapis::session::attention::Activity::working;
    newer->applyAttention(state);
    require(!workspace.nextPriorityAttention(fresh),
            "nothing is waiting once the one with a guess is at work");
}

// One agent the registry cannot be read for is skipped and kept as it was,
// never the end of the whole workspace; a long remote launch argument (an ssh
// agent's inline hook relay) is readable.
void oneBadAgentDoesNotEmptyTheWorkspace() {
    QTemporaryDir directory;
    require(directory.isValid(), "registry directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString good = QStringLiteral("0b6f1a4e-7d55-4c35-9a53-1f3f27b8c1a1");
    const QString remote = QStringLiteral("1c7e2b5f-8e66-4d46-8b64-2a4038c9d2b2");
    const QString bad = QStringLiteral("2d8f3c60-9f77-4e57-9c75-3b5149dae3c3");
    const auto record = [&](const QString& id) {
        return QJsonObject{{"id", id},
                           {"title", id.left(8)},
                           {"category", "general"},
                           {"endpoint", QDir(canonical).filePath(id + ".sock")},
                           {"program", "/usr/bin/true"},
                           {"directory", canonical}};
    };
    auto long_argument = record(remote);
    long_argument.insert("arguments",
                         QJsonArray{QStringLiteral("-t"), QString(4687, QLatin1Char('x'))});
    auto unreadable = record(bad);
    unreadable.insert("harness", "no-such-cli");
    const QJsonArray agents{record(good), long_argument, unreadable};
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    options.headless = true;
    {
        QFile file(options.storagePath);
        require(file.open(QIODevice::WriteOnly), "create registry");
        file.write(
            QJsonDocument(QJsonObject{{"version", 2},
                                      {"activeCategory", "general"},
                                      {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                            {"name", "General"}}}},
                                      {"agents", agents}})
                .toJson());
    }
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.agent(good) != nullptr && workspace.agent(remote) != nullptr,
                "the readable agents, including a long remote argument, come back");
        require(workspace.agent(bad) == nullptr &&
                    workspace.workspaceError().startsWith(QStringLiteral("Not restored")),
                "the unreadable one is skipped and named");
        require(workspace.addCategory(QStringLiteral("Later")), "the workspace still saves");
    }
    QFile saved(options.storagePath);
    require(saved.open(QIODevice::ReadOnly), "read the saved registry");
    const auto kept = QJsonDocument::fromJson(saved.readAll()).object().value("agents").toArray();
    require(std::any_of(kept.begin(), kept.end(),
                        [&](const QJsonValue& value) {
                            return value.toObject().value("id").toString() == bad &&
                                   value.toObject().value("harness").toString() ==
                                       QLatin1String("no-such-cli");
                        }),
            "the unreadable record is written back unchanged");
}

// Tab names the move, but only a later hand move does. The pending choice
// remains one choice, with the agents that began waiting added to it.
void tabAttributionFollowsTheNextHandMove() {
    QTemporaryDir directory;
    require(directory.isValid(), "attribution directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString first = QStringLiteral("ef715fac-a03a-45d4-8466-b0f2740c6b7b");
    const QString second = QStringLiteral("e93750ef-ab0d-418a-b53f-25a601827e31");
    QJsonArray agents;
    for (const auto& id : {first, second})
        agents.append(QJsonObject{{"id", id},
                                  {"title", id},
                                  {"category", "general"},
                                  {"endpoint", QDir(canonical).filePath(id + ".sock")},
                                  {"program", "/usr/bin/true"},
                                  {"directory", canonical}});
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    options.headless = true;
    QFile file(options.storagePath);
    require(file.open(QIODevice::WriteOnly), "create attribution registry");
    const auto bytes =
        QJsonDocument(QJsonObject{{"version", 1},
                                  {"activeCategory", "general"},
                                  {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                        {"name", "General"},
                                                                        {"selected", second}}}},
                                  {"agents", agents}})
            .toJson();
    require(file.write(bytes) == bytes.size(), "write attribution registry");
    file.close();
    Workspace workspace(WorkspaceMode::live, options);
    workspace.setChoiceSettleMsForTesting(20);
    require(workspace.focusedAgent()->agentId() == second, "start on the source agent");
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    auto* target = workspace.agent(first);
    target->setConnection(QStringLiteral("ready"), true);
    state.activity = lapis::session::attention::Activity::working;
    target->applyAttention(state);
    state.activity = lapis::session::attention::Activity::turn_completed;
    target->applyAttention(state);
    auto* source = workspace.agent(second);
    source->setConnection(QStringLiteral("ready"), true);
    require(workspace.nextPriorityAttention({}) && workspace.focusedAgent() == target,
            "Tab moves to the waiting agent");
    // The source begins a turn while Tab's move is on screen, so its finishing
    // turn out of view is the second choice the person settles between.
    state.activity = lapis::session::attention::Activity::working;
    source->applyAttention(state);
    state.activity = lapis::session::attention::Activity::turn_completed;
    source->applyAttention(state);
    require(source->unseen(), "a turn finished out of view marks the agent");
    require(workspace.selectAgent(second), "the person then moves back by hand");
    require(waitFor([&] { return loggedChoices(options.storagePath) == 1; }, 5000),
            "the Tab-and-hand sequence settles once");
    QFile log(QFileInfo(options.storagePath).dir().filePath(QStringLiteral("tab_away.jsonl")));
    require(log.open(QIODevice::ReadOnly), "read the attribution log");
    QString via;
    while (!log.atEnd()) {
        const auto event = QJsonDocument::fromJson(log.readLine()).object();
        if (event.value(QStringLiteral("event")).toString() == QLatin1String("choice"))
            via = event.value(QStringLiteral("via")).toString();
    }
    require(via == QLatin1String("person"), "a hand move after Tab is the person's choice");
}

WorkspaceOptions tabAttributionRegistry(QTemporaryDir& directory, const QString& selectedKind) {
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString source = QStringLiteral("3d53119b-85c5-439b-a2cb-f884742bd11f");
    const QString target = QStringLiteral("eb00ff7d-e97a-472a-b74e-01c3909d5a55");
    const QString other = QStringLiteral("778fc84f-1ad3-4a14-8e21-7719d9c5814d");
    QJsonArray agents;
    for (const auto& id : {source, target, other})
        agents.append(QJsonObject{{"id", id},
                                  {"title", id},
                                  {"category", "general"},
                                  {"endpoint", QDir(canonical).filePath(id + ".sock")},
                                  {"program", "/usr/bin/true"},
                                  {"directory", canonical}});
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    options.headless = true;
    QFile file(options.storagePath);
    require(file.open(QIODevice::WriteOnly), "create Tab attribution registry");
    const auto selected_id = selectedKind == QLatin1String("source")   ? source
                             : selectedKind == QLatin1String("target") ? target
                                                                       : other;
    const auto bytes =
        QJsonDocument(
            QJsonObject{{"version", 1},
                        {"activeCategory", "general"},
                        {"categories",
                         QJsonArray{QJsonObject{
                             {"id", "general"}, {"name", "General"}, {"selected", selected_id}}}},
                        {"agents", agents}})
            .toJson();
    require(file.write(bytes) == bytes.size(), "write Tab attribution registry");
    file.close();
    return options;
}

// The startup focus is the agent the person leaves on the first move, even
// though no focus-change signal ran before restoration.
void firstTabChoiceExcludesTheInitialFocus() {
    QTemporaryDir directory;
    require(directory.isValid(), "first Tab directory");
    const auto source = QStringLiteral("3d53119b-85c5-439b-a2cb-f884742bd11f");
    const auto target = QStringLiteral("eb00ff7d-e97a-472a-b74e-01c3909d5a55");
    auto options = tabAttributionRegistry(directory, QStringLiteral("source"));
    Workspace workspace(WorkspaceMode::live, options);
    workspace.setChoiceSettleMsForTesting(20);
    require(workspace.focusedAgent()->agentId() == source, "start on the initial focus");
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    const auto finish = [&](const QString& id) {
        auto* item = workspace.agent(id);
        item->setConnection(QStringLiteral("ready"), true);
        state.activity = lapis::session::attention::Activity::working;
        item->applyAttention(state);
        state.activity = lapis::session::attention::Activity::turn_completed;
        item->applyAttention(state);
        return item;
    };
    finish(QStringLiteral("778fc84f-1ad3-4a14-8e21-7719d9c5814d"));
    finish(target);
    const bool moved = workspace.nextPriorityAttention({});
    const auto selected =
        workspace.focusedAgent() ? workspace.focusedAgent()->agentId() : QString();
    require(moved && selected == target, "Tab moves to the newer waiting agent");
    require(waitFor([&] { return loggedChoices(options.storagePath) == 1; }, 5000),
            "the first move settles");
    QFile log(QFileInfo(options.storagePath).dir().filePath(QStringLiteral("tab_away.jsonl")));
    require(log.open(QIODevice::ReadOnly), "read the first Tab choice");
    QJsonObject choice;
    while (!log.atEnd()) {
        const auto event = QJsonDocument::fromJson(log.readLine()).object();
        if (event.value(QStringLiteral("event")).toString() == QLatin1String("choice"))
            choice = event;
    }
    const auto candidates = choice.value(QStringLiteral("candidates")).toArray();
    require(candidates.size() == 2, "the choice contains the two waiting agents");
    for (const auto& value : candidates)
        require(value.toObject().value(QStringLiteral("agent")).toString() != source,
                "the agent left at startup is excluded");
}

// A candidate closed before a choice settles is removed, never recorded as if
// it were still waiting.
void closingRemovesAPendingChoiceCandidate() {
    QTemporaryDir directory;
    require(directory.isValid(), "pending close directory");
    const auto target = QStringLiteral("eb00ff7d-e97a-472a-b74e-01c3909d5a55");
    const auto candidate = QStringLiteral("778fc84f-1ad3-4a14-8e21-7719d9c5814d");
    auto options = tabAttributionRegistry(directory, QStringLiteral("source"));
    Workspace workspace(WorkspaceMode::live, options);
    workspace.setChoiceSettleMsForTesting(20);
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    const auto finish = [&](const QString& id) {
        auto* item = workspace.agent(id);
        item->setConnection(QStringLiteral("ready"), true);
        state.activity = lapis::session::attention::Activity::working;
        item->applyAttention(state);
        state.activity = lapis::session::attention::Activity::turn_completed;
        item->applyAttention(state);
        return item;
    };
    finish(target);
    QThread::msleep(5);
    finish(candidate);
    require(workspace.nextPriorityAttention({}) && workspace.focusedAgent()->agentId() == candidate,
            "Tab stages a choice among both waiting agents");
    require(workspace.closeAgent(candidate, true), "close the pending candidate");
    QThread::msleep(60); // longer than the test settle interval
    require(loggedChoices(options.storagePath) == 0, "a closed candidate cannot form a choice");
}

// Closing the focused agent reassigns focus structurally. It must cancel Tab's
// pending choice, not attribute the survivor to the person after a timer.
void closingFocusedAgentCancelsPendingChoice() {
    QTemporaryDir directory;
    require(directory.isValid(), "focused-close directory");
    auto options = tabAttributionRegistry(directory, QStringLiteral("source"));
    Workspace workspace(WorkspaceMode::live, options);
    workspace.setChoiceSettleMsForTesting(20);
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    const auto finish = [&](const QString& id) {
        auto* item = workspace.agent(id);
        item->setConnection(QStringLiteral("ready"), true);
        state.activity = lapis::session::attention::Activity::working;
        item->applyAttention(state);
        state.activity = lapis::session::attention::Activity::turn_completed;
        item->applyAttention(state);
        return item;
    };
    const QString source = QStringLiteral("3d53119b-85c5-439b-a2cb-f884742bd11f");
    const QString target = QStringLiteral("eb00ff7d-e97a-472a-b74e-01c3909d5a55");
    finish(source);
    finish(target);
    require(workspace.nextPriorityAttention({}) && workspace.focusedAgent()->agentId() == target,
            "Tab stages a focused choice");
    require(workspace.closeAgent(target, true), "close the focused candidate");
    QThread::msleep(60);
    require(loggedChoices(options.storagePath) == 0, "a structural focus change records no choice");
}

// A failed registry save must not teach a nonexistent Tab move or leave a
// half-staged choice behind.
void failedTabSelectionRecordsNothing() {
    QTemporaryDir directory;
    require(directory.isValid(), "failed Tab directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString source = QStringLiteral("51a90682-5ee6-4ffd-a356-f24f82e9db51");
    const QString target = QStringLiteral("79858e34-34d8-4cf9-a2c1-25969a1a2d2f");
    QJsonArray agents;
    for (const auto& id : {source, target})
        agents.append(QJsonObject{{"id", id},
                                  {"title", id},
                                  {"category", "general"},
                                  {"endpoint", QDir(canonical).filePath(id + ".sock")},
                                  {"program", "/usr/bin/true"},
                                  {"directory", canonical}});
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    options.headless = true;
    QFile file(options.storagePath);
    require(file.open(QIODevice::WriteOnly), "create failed-tab registry");
    const auto bytes =
        QJsonDocument(QJsonObject{{"version", 1},
                                  {"activeCategory", "general"},
                                  {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                        {"name", "General"},
                                                                        {"selected", source}}}},
                                  {"agents", agents}})
            .toJson();
    require(file.write(bytes) == bytes.size(), "write failed-tab registry");
    file.close();
    Workspace workspace(WorkspaceMode::live, options);
    auto* waiting = workspace.agent(target);
    require(waiting != nullptr && workspace.focusedAgent()->agentId() == source,
            "load the failed-tab fixture");
    waiting->setConnection(QStringLiteral("ready"), true);
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    state.activity = lapis::session::attention::Activity::working;
    waiting->applyAttention(state);
    state.activity = lapis::session::attention::Activity::turn_completed;
    waiting->applyAttention(state);
    require(QFile::setPermissions(canonical, QFile::ReadOwner | QFile::ExeOwner),
            "make the registry parent unwritable");
    require(!workspace.nextPriorityAttention({}), "the Tab selection fails with the registry");
    require(workspace.focusedAgent()->agentId() == source,
            "a failed Tab move leaves the source focused");
    require(!QFileInfo::exists(
                QFileInfo(options.storagePath).dir().filePath(QStringLiteral("tab_away.jsonl"))),
            "a failed Tab move writes no event log");
    require(
        QFile::setPermissions(canonical, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
        "restore registry parent permissions");
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
        return workspace.agent(id)->statusSource();
    };
    using Source = lapis::desktop::Agent::StatusSource;
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().isEmpty(), "load adapter registry");
        require(source(workspace, managed) == Source::observer,
                "Claude agents read status from the service adapter");
        require(source(workspace, legacy) == Source::output,
                "pre-adapter Claude agents keep terminal mode and the output estimate");
        require(source(workspace, codex) == Source::output,
                "Codex estimates status from terminal output");
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

// An earlier lapis saved stage tiles per category. Such a registry still
// loads, the stage shows the category's selected agent, the next key walks the
// strip, and the next save drops the tiles.
void savedTilesLoadAsTheSelectedAgent() {
    QTemporaryDir directory;
    require(directory.isValid(), "saved-tiles directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString left = uuid();
    const QString right = uuid();
    const QString other = uuid();
    auto agent = [&](const QString& id) { return agentRecord(canonical, id, "work"); };
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
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
                 {"tiles", QJsonObject{{"stacked", false},
                                       {"ratio", 0.5},
                                       {"children", QJsonArray{QJsonObject{{"agent", left}},
                                                               QJsonObject{{"agent", right}}}}}}}}},
            {"agents", QJsonArray{agent(left), agent(right), agent(other)}}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "a registry with saved tiles loads");
    require(workspace.focusedAgent() == workspace.agent(right),
            "the stage shows the category's selected agent");
    workspace.nextAgent(1);
    require(workspace.focusedAgent() == workspace.agent(other),
            "the next key walks the strip, not the old tiles");
    const auto groups = QJsonDocument::fromJson(readRegistry(options.storagePath))
                            .object()
                            .value(QStringLiteral("categories"))
                            .toArray();
    require(groups.size() == 1, "the saved registry keeps its category");
    const auto saved = groups.first().toObject();
    require(!saved.contains(QStringLiteral("tiles")) &&
                saved.value(QStringLiteral("selected")).toString() == other,
            "the next save drops the tiles and keeps the selection");
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
    require(workspace.agents().isEmpty() && !workspace.focusedAgent(),
            "unknown registry versions create no sessions");
    require(readRegistry(options.storagePath) == bytes, "unknown versions remain for migration");
}

// Harnesses without an observer get an output-timing estimate that
// says so; reattaching does not count as activity.
void outputEstimate() {
    lapis::desktop::Agent item(QStringLiteral("Grok"), {}, {}, QColor{}, "");
    item.setHarnessId(QStringLiteral("grok"));
    item.setStatusSource(lapis::desktop::Agent::StatusSource::output);
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
        auto* agent = workspace.focusedAgent();
        require(waitFor([agent] { return agent->inputReady(); }, 10000), "agent ready");
        require(workspace.closeAgent(agent->agentId()) &&
                    waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
    auto* agent = workspace.focusedAgent();
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
    require(workspace.closeAgent(agent->agentId()) &&
                waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
        auto* agent = workspace->focusedAgent();
        const auto id = agent->agentId();
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
            require(workspace->closeAgent(id) &&
                        waitFor([&] { return closedFully(*workspace); }, 10000),
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
    auto* agent = workspace.focusedAgent();
    require(waitFor([agent] { return agent->inputReady(); }, 10000),
            "high-volume updater completes before agent launch");
    const auto logged = fixture.read("harness-updates.log");
    require(logged.contains("tail-marker") && logged.size() < 2048,
            "only the bounded updater tail reaches the log");
    require(workspace.closeAgent(agent->agentId()) &&
                waitFor([&workspace] { return closedFully(workspace); }, 10000),
            "close output fixture agent");
}

void failedUpdaterStartClearsTheQueue() {
    UpdaterFixture fixture("#!/nonexistent/lapis-fake-updater\n");
    Workspace workspace(WorkspaceMode::live, fixture.options);
    fixture.create(workspace);
    auto* agent = workspace.focusedAgent();
    require(
        waitFor([agent] { return agent->statusLabel() != QStringLiteral("Updating Grok…"); }, 2000),
        "FailedToStart releases the queue");
    require(fixture.read("harness-updates.log").contains("grok update: could not start"),
            "failed start is logged without waiting for finished");
}

// harnessUpdates in lapis.json pins a CLI: its agents start without an
// update, while every other CLI still updates first.
void pinnedCliIsNotUpdated() {
    const QByteArray script = "#!/bin/sh\n"
                              "if [ \"$1\" = update ]; then\n"
                              "  echo \"${0##*/}\" >> \"$HOME/updates\"\n"
                              "  exit 0\n"
                              "fi\n"
                              "echo ready\n"
                              "exec /bin/sleep 600\n";
    UpdaterFixture fixture(script, QStringLiteral("grok"));
    writeExecutable(fixture.root.filePath(QStringLiteral("bin/claude")), script);
    Workspace workspace(WorkspaceMode::live, fixture.options);
    workspace.setHarnessUpdatesOff({QStringLiteral("grok")});
    fixture.create(workspace);
    auto* pinned = workspace.focusedAgent();
    require(pinned->statusLabel() != QStringLiteral("Updating Grok…"),
            "a pinned CLI's agent does not wait for an update");
    require(waitFor([pinned] { return pinned->inputReady(); }, 10000), "the pinned agent starts");
    require(!QFileInfo::exists(fixture.root.filePath(QStringLiteral("updates"))),
            "the pinned CLI was not updated on start");
    require(workspace.createAgent(fixture.root.filePath(QStringLiteral("project")),
                                  QStringLiteral("unpinned"), QStringLiteral("claude")),
            "create an agent of a CLI that still updates");
    auto* updated = workspace.focusedAgent();
    require(waitFor([updated] { return updated->inputReady(); }, 10000),
            "the unpinned agent starts after its update");
    require(fixture.read(QStringLiteral("updates")) == "claude\n",
            "only the unpinned CLI was updated");
    for (const auto& id : {pinned->agentId(), updated->agentId()})
        require(workspace.closeAgent(id), "close a pinned-update fixture agent");
    require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
            "pinned-update fixture agents close");
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
        auto* agent = workspace.focusedAgent();
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
        auto* agent = workspace.focusedAgent();
        require(agent && waitFor([agent] { return agent->inputReady(); }, 20000),
                "explicit reattachment retains its mode");
        require(fixture.read("starts") == original_pid &&
                    !QFileInfo::exists(fixture.root.filePath("updates")) &&
                    !QFileInfo::exists(fixture.root.filePath("harness-updates.log")),
                "reattachment keeps the same process without updating");
        if (mode == lapis::session::wire::AttachMode::discover)
            require(workspace.closeAgent(agent->agentId()) &&
                        waitFor([&workspace] { return closedFully(workspace); }, 10000),
                    "explicit fixture closes after both reattachments");
    }
    options.mode = lapis::session::wire::AttachMode::create;
    options.updateHarnesses = false;
    options.endpoint = fixture.root.filePath(QStringLiteral("disabled.sock"));
    Workspace workspace(WorkspaceMode::live, options);
    auto* agent = workspace.focusedAgent();
    require(agent && waitFor([agent] { return agent->inputReady(); }, 20000),
            "update-disabled explicit creation starts");
    require(!QFileInfo::exists(fixture.root.filePath("updates")) &&
                !QFileInfo::exists(fixture.root.filePath("harness-updates.log")),
            "the explicit update opt-out is honored");
    require(workspace.closeAgent(agent->agentId()) &&
                waitFor([&workspace] { return closedFully(workspace); }, 10000),
            "update-disabled fixture closes");

    options.updateHarnesses = true;
    options.harnessUpdatesOff = {QStringLiteral("claude")};
    options.endpoint = fixture.root.filePath(QStringLiteral("pinned.sock"));
    {
        Workspace pinnedWorkspace(WorkspaceMode::live, options);
        auto* pinnedAgent = pinnedWorkspace.focusedAgent();
        require(pinnedAgent && pinnedAgent->statusLabel() != QStringLiteral("Updating Claude…"),
                "constructor-supplied update pins skip the startup update");
        require(waitFor([pinnedAgent] { return pinnedAgent->inputReady(); }, 10000),
                "the constructor-pinned agent starts");
        require(!QFileInfo::exists(fixture.root.filePath(QStringLiteral("updates"))),
                "constructor-supplied update pins do not create the update marker");
        require(!QFileInfo::exists(fixture.root.filePath(QStringLiteral("harness-updates.log"))),
                "constructor-supplied update pins do not log a startup update");
        require(pinnedWorkspace.closeAgent(pinnedAgent->agentId()) &&
                    waitFor([&pinnedWorkspace] { return closedFully(pinnedWorkspace); }, 10000),
                "constructor-pinned fixture agent closes");
    }
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
    std::exception_ptr worker_error;
    std::thread release([&helper, &options, &worker_error] {
        try {
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
        } catch (...) {
            worker_error = std::current_exception();
        }
    });
    QElapsedTimer clock;
    clock.start();
    Workspace window(WorkspaceMode::live, options);
    release.join();
    if (worker_error)
        std::rethrow_exception(worker_error);
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
    const QStringList policy{"-o", "ControlPath=none",      "-o", "ServerAliveInterval=15",
                             "-o", "ServerAliveCountMax=4", "-o", "ConnectTimeout=15"};
    const std::vector<Case> cases{{56, {}, policy},
                                  {58, {}, {}, true},
                                  {62, {}, {}, true},
                                  {64, policy, {}},
                                  {62, policy.mid(2), policy.mid(0, 2)},
                                  {56, {"-o", "ControlPath=shared"}, policy},
                                  {56, {"ControlPath=none"}, policy},
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
            require(workspace.agents().size() == 1, "a rejected migration retains the tab");
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

// A running service whose saved session descriptor names another session (a
// service started at login whose attachment timed out before it was
// confirmed) is reattached by discovery, without a restart.
void staleDescriptorReattachesTheRunningService() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-stale-XXXXXX"));
    require(directory.isValid(), "stale descriptor directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.restoreAgents = true;
    QString id;
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.createAgent(root.path(), QStringLiteral("stale"), QStringLiteral("grok")),
                "a local agent");
        auto* agent = workspace.focusedAgent();
        require(agent != nullptr && waitFor([agent] { return agent->inputReady(); }, 10000),
                "it attaches");
        id = agent->agentId();
    }
    const auto endpoint = QJsonDocument::fromJson(readRegistry(options.storagePath))
                              .object()[QStringLiteral("agents")]
                              .toArray()
                              .first()
                              .toObject()[QStringLiteral("endpoint")]
                              .toString();
    {
        // Another session's id in the descriptor's session field.
        QFile descriptor(endpoint + QStringLiteral(".session"));
        require(descriptor.open(QIODevice::ReadWrite), "open the saved descriptor");
        auto bytes = descriptor.readAll();
        require(bytes.startsWith("LAPIS-S1\n") && bytes.size() > 25, "a saved descriptor");
        for (int at = 9; at < 25; ++at)
            bytes[at] = static_cast<char>(static_cast<unsigned char>(bytes[at]) ^ 0x5aU);
        require(descriptor.seek(0) && descriptor.write(bytes) == bytes.size(),
                "write a stale descriptor");
    }
    {
        Workspace workspace(WorkspaceMode::live, options);
        auto* agent = workspace.agent(id);
        require(agent != nullptr && waitFor([agent] { return agent->inputReady(); }, 15000),
                "the running service is reattached despite the stale descriptor");
        require(screenText(agent->snapshot()).contains(QStringLiteral("grok ready")),
                "it is the same running agent, not a new one");
        require(workspace.closeAgent(id, true) &&
                    waitFor([&workspace] { return closedFully(workspace); }, 10000),
                "the stand-in agent closes");
    }
    qputenv("PATH", path);
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
case "$host:$n" in typo:*) exit 255;; boot:1) exit 255;; devbox:1) sleep 1; exit 255;; devbox:2) sleep 1; exit 1;; esac
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
    // A saved agent whose first connection after login fails at once (the
    // network not up yet) reconnects to its conversation by itself.
    const auto booted_id = QStringLiteral("5a1d0000-0000-4000-8000-000000000002");
    const auto booted_conversation = QStringLiteral("5a1d0000-0000-4000-8000-0000000000b0");
    auto booted = agentRecord(root.path(), booted_id, "general");
    booted.insert(QStringLiteral("title"), QStringLiteral("booted"));
    booted.insert(QStringLiteral("program"), root.filePath(QStringLiteral("bin/ssh")));
    booted.insert(QStringLiteral("directory"), QDir::homePath());
    booted.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    booted.insert(
        QStringLiteral("arguments"),
        QJsonArray{"-t", "boot",
                   QStringLiteral(R"(cd ~ && s=%1 && exec "${SHELL:-/bin/sh}" -lic claude)")
                       .arg(booted_conversation)});
    writeRegistry(options.storagePath,
                  QJsonObject{{"version", 2},
                              {"activeCategory", "general"},
                              {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                    {"name", "General"},
                                                                    {"selected", older_id}}}},
                              {"agents", QJsonArray{older, booted}}});
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(waitFor([&] { return calls(QStringLiteral("boot")) >= 2; }, 15000) &&
                    conversation(call(QStringLiteral("boot"), 2)) == booted_conversation,
                "a restored agent that could not connect at login reconnects to its "
                "conversation");
        require(waitFor([&] { return calls(QStringLiteral("older")) >= 1; }, 10000) &&
                    call(QStringLiteral("older"), 1)
                        .startsWith(QStringLiteral("-o\nControlPath=none\n-o\n"
                                                   "ServerAliveInterval=15\n-o\n"
                                                   "ServerAliveCountMax=4\n-o\n"
                                                   "ConnectTimeout=15\n-t\nolder\n")),
                "an agent saved before gets its own connection and keepalives");
        workspace.setSshConfigForTesting(config.fileName());
        workspace.setReconnectTimingForTesting({.first_hold = std::chrono::milliseconds(300),
                                                .wait = std::chrono::milliseconds(2000)});
        require(workspace.createAgent(QStringLiteral("~/dev/far"), QStringLiteral("far"),
                                      QStringLiteral("claude"), {}, {}, QStringLiteral("devbox")),
                "a Claude Code agent on another machine");
        auto* far = workspace.focusedAgent();
        require(far != nullptr, "the new agent is shown");
        const auto id = far->agentId();
        require(waitFor([&] { return calls(QStringLiteral("devbox")) >= 1; }, 10000), "ssh starts");
        const auto first = call(QStringLiteral("devbox"), 1);
        require(
            first.startsWith(QStringLiteral("-o\nControlPath=none\n-o\nServerAliveInterval=15\n"
                                            "-o\nServerAliveCountMax=4\n-o\nConnectTimeout=15\n"
                                            "-t\ndevbox\n")) &&
                first.contains(QStringLiteral("cd ~/dev/far && s=")) &&
                !conversation(first).isEmpty() &&
                first.contains(QStringLiteral(R"(-lic 'export LAPIS_HOOK_NONCE=)")) &&
                first.contains(QStringLiteral(
                    R"(export CLAUDE_CODE_NO_FLICKER="${CLAUDE_CODE_NO_FLICKER:-0}"; claude --permission-mode bypassPermissions --settings )")) &&
                first.contains(QStringLiteral(R"(}'\'' '"$o $s")")),
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
        require(!workspace.closeAgent(id, true),
                "a failed discard during a pending retry reports failure");
        require(workspace.workspaceError().contains(QStringLiteral("Cannot save workspace:")) &&
                    workspace.agent(id) == far,
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
        // Instrumented event delivery can stretch a 300 ms hold, so prove the
        // never-held policy with a test-only wide threshold and fast retries.
        workspace.setReconnectTimingForTesting(
            {.first_hold = std::chrono::seconds(5), .wait = std::chrono::milliseconds(20)});
        require(workspace.createAgent(QStringLiteral("~"), QStringLiteral("typo"),
                                      QStringLiteral("claude"), {}, {}, QStringLiteral("typo")),
                "an agent on a machine that never answers");
        lapis::desktop::Agent* typo = nullptr;
        for (const auto& listed : workspace.agents())
            if (auto* item = listed.value<lapis::desktop::Agent*>();
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
        for (const auto& closing : {older_id, booted_id, id, typo->agentId()})
            require(workspace.closeAgent(closing, true), "close the stand-in agents");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
                "the stand-in agents close");
    }
    qputenv("PATH", path);
}

// An agent on another machine reports its turns through its own terminal.
// A stand-in ssh runs the command it is given here, as that machine would,
// in a stand-in login shell with a fixture home; stand-in CLIs run the hooks
// (Claude Code) and notify program (Codex) from their command lines. Each
// finished turn pings as a local agent's does, the sequences never reach the
// screen, a selected Codex profile's own notify program still runs, and a
// remote agent saved before the hooks gains them on restart.
void remoteAgentsReportTurnsThroughTheirTerminal() {
    using lapis::desktop::Agent;
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-remote-hooks-XXXXXX"));
    require(directory.isValid(), "remote hooks directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    require(root.mkpath(QStringLiteral("home")), "a home on the other machine");
    require(root.mkpath(QStringLiteral("home/.codex")), "a Codex home on the other machine");
    const auto base_marker = root.filePath(QStringLiteral("remote-base-notify-ran"));
    const auto profile_marker = root.filePath(QStringLiteral("remote-profile-notify-ran"));
    writeExecutable(root.filePath(QStringLiteral("bin/remote-base-notify")),
                    "#!/bin/sh\nprintf '%s' \"$1\" > '" + QFile::encodeName(base_marker) + "'\n");
    writeExecutable(root.filePath(QStringLiteral("bin/remote-profile-notify")),
                    "#!/bin/sh\nprintf '%s' \"$1\" > '" + QFile::encodeName(profile_marker) +
                        "'\n");
    QFile remote_config(root.filePath(QStringLiteral("home/.codex/config.toml")));
    require(remote_config.open(QIODevice::WriteOnly), "write the remote base Codex config");
    remote_config.write("notify = [\"" +
                        QFile::encodeName(root.filePath(QStringLiteral("bin/remote-base-notify"))) +
                        "\"]\n");
    remote_config.close();
    QFile remote_profile(root.filePath(QStringLiteral("home/.codex/work.config.toml")));
    require(remote_profile.open(QIODevice::WriteOnly), "write the remote work Codex profile");
    remote_profile.write(
        "notify = [\"" +
        QFile::encodeName(root.filePath(QStringLiteral("bin/remote-profile-notify"))) + "\"]\n");
    remote_profile.close();
    writeExecutable(root.filePath(QStringLiteral("bin/ssh")), R"(#!/bin/sh
for last; do :; done
d=$(cd "$(dirname "$0")/.." && pwd)
export HOME="$d/home" SHELL="$d/bin/login-shell"
cd "$HOME" && exec /bin/sh -c "$last"
)");
    // Its login shell keeps this PATH, where a real one would read profiles.
    writeExecutable(root.filePath(QStringLiteral("bin/login-shell")),
                    R"(#!/bin/sh
[ "$1" = -lic ] && exec /bin/sh -c "$2"
exit 64
)");
    writeExecutable(root.filePath(QStringLiteral("bin/claude")), R"(#!/usr/bin/env python3
import json, subprocess, sys
args = sys.argv[1:]
hooks = json.loads(args[args.index("--settings") + 1])["hooks"]
session = args[args.index("--session-id") + 1] if "--session-id" in args else "restored"
def fire(event, **fields):
    payload = dict(hook_event_name=event, session_id=session, **fields)
    for group in hooks[event]:
        for hook in group["hooks"]:
            # As Claude Code does: no controlling terminal, so no /dev/tty.
            subprocess.run(["sh", "-c", hook["command"]], input=json.dumps(payload).encode(),
                           start_new_session=True)
fire("SessionStart", source="startup")
print("claude ready", flush=True)
for n, line in enumerate(sys.stdin, 1):
    fire("UserPromptSubmit", prompt_id=f"p{n}", prompt=line.strip())
    print("thinking", flush=True)
    tasks = []
    if session != "restored" and n == 1:
        tasks = 0
    elif session != "restored" and n == 2:
        tasks = [{"status": "running"}] * 65
    fire("Stop", prompt_id=f"p{n}", background_tasks=tasks, session_crons=[])
    print(f"turn done {n}", flush=True)
)");
    writeExecutable(root.filePath(QStringLiteral("bin/codex")), R"(#!/usr/bin/env python3
import json, subprocess, sys, time
args = sys.argv[1:]
notify = next(json.loads(a.split("=", 1)[1]) for a in args if a.startswith("notify="))
print("codex ready", flush=True)
for n, line in enumerate(sys.stdin, 1):
    for i in range(30):
        print(f"working {i}", flush=True)
        time.sleep(0.05)
    done = {"type": "agent-turn-complete", "turn-id": f"t{n}", "input-messages": [line]}
    subprocess.run(notify + [json.dumps(done)], start_new_session=True)
    print("turn done", flush=True)
)");
    QFile config(root.filePath(QStringLiteral("ssh_config")));
    require(config.open(QIODevice::WriteOnly), "write an ssh config");
    config.write("Host devbox\n");
    config.close();
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.restoreAgents = true;
    // Saved before remote agents carried hooks.
    const auto older_id = QStringLiteral("5a1d0000-0000-4000-8000-000000000002");
    auto older = agentRecord(root.path(), older_id, "general");
    older.insert(QStringLiteral("title"), QStringLiteral("older"));
    older.insert(QStringLiteral("program"), root.filePath(QStringLiteral("bin/ssh")));
    older.insert(QStringLiteral("directory"), QDir::homePath());
    older.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    older.insert(
        QStringLiteral("arguments"),
        QJsonArray{
            "-o", "ControlPath=none", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4",
            "-t", "devbox",
            R"(cd ~ && exec "${SHELL:-/bin/sh}" -lic 'claude --permission-mode bypassPermissions')"});
    writeRegistry(options.storagePath,
                  QJsonObject{{"version", 2},
                              {"activeCategory", "general"},
                              {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                    {"name", "General"},
                                                                    {"selected", older_id}}}},
                              {"agents", QJsonArray{older}}});
    {
        Workspace workspace(WorkspaceMode::live, options);
        workspace.setSshConfigForTesting(config.fileName());
        QHash<QString, int> pings;
        QObject::connect(&workspace, &Workspace::turnFinished,
                         [&pings](Agent* item) { ++pings[item->agentId()]; });
        const auto shows = [](Agent* item, const QString& text) {
            return screenText(item->snapshot()).contains(text);
        };
        const auto clean = [](Agent* item) {
            const auto text = screenText(item->snapshot());
            return !text.contains(QStringLiteral("7717")) &&
                   !text.contains(QStringLiteral("lapis-"));
        };
        auto* restored = workspace.agent(older_id);
        require(restored != nullptr &&
                    waitFor([&] { return shows(restored, QStringLiteral("claude ready")); }, 15000),
                "a saved remote Claude Code agent starts with the hooks it lacked");
        require(workspace.createAgent(QStringLiteral("~"), QStringLiteral("far claude"),
                                      QStringLiteral("claude"), {}, {}, QStringLiteral("devbox")),
                "a Claude Code agent on another machine");
        auto* claude = workspace.focusedAgent();
        require(claude != nullptr, "the remote Claude Code agent is shown");
        require(claude != restored && claude->statusSource() == Agent::StatusSource::output,
                "it starts from the output estimate");
        require(waitFor(
                    [&] {
                        return claude->inputReady() &&
                               shows(claude, QStringLiteral("claude ready"));
                    },
                    15000),
                "the remote Claude Code stand-in starts");
        for (int turn = 1; turn <= 3; ++turn) {
            require(claude->sendText("next prompt\r"), "type a prompt");
            const auto done = QStringLiteral("turn done %1").arg(turn);
            require(waitFor([&] { return shows(claude, done); }, 15000),
                    "the remote Claude Code stand-in finishes its output");
            if (turn <= 2) {
                waitFor([&] { return false; }, 300);
                require(pings.value(claude->agentId()) == 0 &&
                            claude->statusKind() == QStringLiteral("working"),
                        "malformed or oversized background work cannot finish a remote turn");
            } else {
                require(waitFor([&] { return pings.value(claude->agentId()) == 1; }, 15000) &&
                            claude->statusKind() == QStringLiteral("finished"),
                        "a valid empty Stop hook finishes the remote turn and pings once");
            }
        }
        require(clean(claude) && shows(claude, QStringLiteral("turn done")),
                "hook sequences never reach the screen");
        require(restored->sendText("hello\r") &&
                    waitFor([&] { return pings.value(older_id) == 1; }, 15000),
                "the restored agent's hooks reach lapis too");
        workspace.setHarnessArguments(
            {{QStringLiteral("codex"), {QStringLiteral("-p"), QStringLiteral("work")}}});
        require(workspace.createAgent(QStringLiteral("~"), QStringLiteral("far codex"),
                                      QStringLiteral("codex"), {}, {}, QStringLiteral("devbox")),
                "a Codex agent on another machine");
        auto* codex = workspace.focusedAgent();
        require(codex != nullptr, "the Codex agent is shown");
        require(codex != claude && codex != restored, "the Codex agent is distinct");
        codex->setOutputTimingForTesting({.settle_ms = 0, .burst_ms = 1500, .quiet_ms = 4000});
        require(
            waitFor(
                [&] { return codex->inputReady() && shows(codex, QStringLiteral("codex ready")); },
                15000),
            "the remote Codex stand-in starts");
        for (int turn = 1; turn <= 2; ++turn) {
            require(codex->sendText("next prompt\r"), "type a Codex prompt");
            require(waitFor([&] { return pings.value(codex->agentId()) == turn; }, 15000) &&
                        codex->statusKind() == QStringLiteral("finished"),
                    "Codex's notify program finishes each turn once; a prompt starts the next");
        }
        require(clean(codex), "notify sequences never reach the screen");
        require(QFileInfo::exists(profile_marker) && !QFileInfo::exists(base_marker),
                "the remote relay runs the selected profile's notify program");
        for (const auto& closing : {older_id, claude->agentId(), codex->agentId()})
            require(workspace.closeAgent(closing, true), "close the stand-in agents");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
                "the stand-in agents close");
    }
    qputenv("PATH", path);
}

// A local Codex agent reports each finished turn through the notify program
// its session service adds, as an agent on another machine does: the turn
// pings once, no hook bytes reach the screen, the person's own notify program
// from the selected profile's config still runs, the notify event's thread is saved as the
// conversation, and a restart resumes it.
// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void localCodexReportsTurnsThroughNotify() {
    using lapis::desktop::Agent;
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-local-codex-XXXXXX"));
    require(directory.isValid(), "local Codex directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    const auto thread = QStringLiteral("019a0000-0000-7000-8000-00000000c0de");
    const auto home = root.filePath(QStringLiteral("codex-home"));
    const auto base_marker = root.filePath(QStringLiteral("base-notify-ran"));
    const auto profile_marker = root.filePath(QStringLiteral("profile-notify-ran"));
    const auto starts = root.filePath(QStringLiteral("codex-starts"));
    require(root.mkpath(QStringLiteral("codex-home")), "a Codex home");
    writeExecutable(root.filePath(QStringLiteral("bin/base-notify")),
                    "#!/bin/sh\nprintf '%s' \"$1\" > '" + QFile::encodeName(base_marker) + "'\n");
    writeExecutable(root.filePath(QStringLiteral("bin/profile-notify")),
                    "#!/bin/sh\nprintf '%s' \"$1\" > '" + QFile::encodeName(profile_marker) +
                        "'\n");
    QFile config(root.filePath(QStringLiteral("codex-home/config.toml")));
    require(config.open(QIODevice::WriteOnly), "write the person's Codex config");
    config.write("model = \"stand-in\"\nnotify = [\"" +
                 QFile::encodeName(root.filePath(QStringLiteral("bin/base-notify"))) +
                 "\"]\n\n[tui]\nnotifications = false\n");
    config.close();
    QFile profile_config(root.filePath(QStringLiteral("codex-home/work.config.toml")));
    require(profile_config.open(QIODevice::WriteOnly), "write the selected Codex profile");
    profile_config.write("notify = [\"" +
                         QFile::encodeName(root.filePath(QStringLiteral("bin/profile-notify"))) +
                         "\"]\n");
    profile_config.close();
    // Codex runs its notify program with the event as the last argument after
    // each finished turn, and keeps the thread's rollout under CODEX_HOME.
    writeExecutable(root.filePath(QStringLiteral("bin/codex")), R"(#!/usr/bin/env python3
import json, os, subprocess, sys, time
args = sys.argv[1:]
with open(os.environ["LAPIS_TEST_STARTS"], "a") as starts:
    starts.write(json.dumps(args) + "\n")
if args[:1] != ["-c"] or not args[1].startswith("notify="):
    sys.exit(3)
notify = json.loads(args[1].split("=", 1)[1])
thread = args[args.index("resume") + 1] if "resume" in args else os.environ["LAPIS_TEST_THREAD"]
day = os.path.join(os.environ["CODEX_HOME"], "sessions", "2026", "11", "09")
os.makedirs(day, exist_ok=True)
with open(os.path.join(day, f"rollout-2026-11-09T12-00-00-{thread}.jsonl"), "a") as rollout:
    meta = {"type": "session_meta", "payload": {"id": thread, "source": "cli"}}
    rollout.write(json.dumps(meta) + "\n")
print("codex ready", flush=True)
for n, line in enumerate(sys.stdin, 1):
    if line.strip() == "exit":
        sys.exit(0)
    for i in range(30):
        print(f"working {i}", flush=True)
        time.sleep(0.05)
    done ={"type": "agent-turn-complete", "thread-id": thread, "turn-id": f"t{n}",
            "input-messages": [line.strip()], "last-assistant-message": "private reply"}
    subprocess.run(notify + [json.dumps(done)], start_new_session=True)
    print(f"turn done {n}", flush=True)
)");
    const auto previous_home = qgetenv("CODEX_HOME");
    const bool had_home = qEnvironmentVariableIsSet("CODEX_HOME");
    qputenv("CODEX_HOME", QFile::encodeName(home));
    qputenv("LAPIS_TEST_STARTS", QFile::encodeName(starts));
    qputenv("LAPIS_TEST_THREAD", thread.toUtf8());
    const auto restore = qScopeGuard([&] {
        if (had_home)
            qputenv("CODEX_HOME", previous_home);
        else
            qunsetenv("CODEX_HOME");
        qunsetenv("LAPIS_TEST_STARTS");
        qunsetenv("LAPIS_TEST_THREAD");
        qputenv("PATH", path);
    });
    const auto started = [&starts] {
        QStringList lines;
        QFile file(starts);
        if (file.open(QIODevice::ReadOnly))
            for (const auto& line : file.readAll().split('\n'))
                if (!line.isEmpty())
                    lines.append(QString::fromUtf8(line));
        return lines;
    };
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.restoreAgents = true;
    // Session services outlive the window by design, so the test ends each one
    // it started and waits for it to exit, on a failed requirement too.
    const auto servicesListening = [&root] {
        for (const auto& name : root.entryList({QStringLiteral("*.sock")}, QDir::System)) {
            QLocalSocket probe;
            probe.connectToServer(root.filePath(name));
            if (probe.waitForConnected(250))
                return true;
        }
        return false;
    };
    const auto stop_services = qScopeGuard([&] {
        if (!servicesListening())
            return;
        // Only this fixture's directory names these processes.
        qWarning() << "local-codex: stopping session services left in" << root.path();
        QProcess::execute(QStringLiteral("/usr/bin/pkill"),
                          {QStringLiteral("-f"), root.path() + QLatin1Char('/')});
        static_cast<void>(waitFor([&] { return !servicesListening(); }, 5000));
    });
    {
        Workspace workspace(WorkspaceMode::live, options);
        const auto close_owned = qScopeGuard([&] {
            for (const auto& value : workspace.agents())
                if (auto* item = value.value<Agent*>()) {
                    static_cast<void>(waitFor(
                        [item] {
                            return item->reachable() ||
                                   item->connectionState() == QStringLiteral("ended");
                        },
                        10000));
                    static_cast<void>(workspace.closeAgent(item->agentId()));
                }
            static_cast<void>(waitFor([&] { return closedFully(workspace); }, 10000));
            static_cast<void>(waitFor([&] { return !servicesListening(); }, 10000));
        });
        QHash<QString, int> pings;
        QObject::connect(&workspace, &Workspace::turnFinished,
                         [&pings](Agent* item) { ++pings[item->agentId()]; });
        workspace.setHarnessArguments(
            {{QStringLiteral("codex"), {QStringLiteral("-p"), QStringLiteral("work")}}});
        require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                      QStringLiteral("local codex"), QStringLiteral("codex")),
                "a local Codex agent");
        auto* codex = workspace.focusedAgent();
        require(codex != nullptr && codex->statusSource() == Agent::StatusSource::output,
                "a local Codex agent starts from the output estimate");
        const auto id = codex->agentId();
        codex->setOutputTimingForTesting({.settle_ms = 0, .burst_ms = 1500, .quiet_ms = 4000});
        const auto shows = [&codex](const QString& text) {
            return screenText(codex->snapshot()).contains(text);
        };
        require(waitFor([&] { return codex->inputReady() && shows(QStringLiteral("codex ready")); },
                        15000),
                "the local Codex stand-in starts with lapis's notify program");
        for (int turn = 1; turn <= 2; ++turn) {
            require(codex->sendText("next prompt\r"), "type a Codex prompt");
            require(waitFor([&] { return pings.value(id) == turn; }, 15000) &&
                        codex->statusKind() == QStringLiteral("finished"),
                    "Codex's notify program finishes each turn once; a prompt starts the next");
        }
        require(waitFor([&] { return shows(QStringLiteral("turn done 2")); }, 10000),
                "the stand-in's output is shown");
        const auto screen = screenText(codex->snapshot());
        require(!screen.contains(QStringLiteral("7717")) &&
                    !screen.contains(QStringLiteral("lapis-")),
                "notify sequences never reach the screen");
        require(waitFor([&] { return QFileInfo::exists(profile_marker); }, 10000),
                "the selected profile's notify program still runs");
        require(!QFileInfo::exists(base_marker),
                "the profile notify replaces the base config notify");
        QFile chained(profile_marker);
        require(chained.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(chained.readAll())
                                                             .object()
                                                             .value(QStringLiteral("thread-id"))
                                                             .toString() == thread,
                "it receives Codex's whole event");
        const auto endpoint = root.filePath(id + QStringLiteral(".sock"));
        const auto saved = lapis::session::read_resume_record(endpoint);
        require(saved && saved->agent == QStringLiteral("codex") && saved->session_id == thread &&
                    saved->source == lapis::session::ResumeSource::observer,
                "the notify event's thread is saved as the conversation");
        require(started().size() == 1, "one Codex start so far");
        const auto first_start = QJsonDocument::fromJson(started().at(0).toUtf8()).array();
        require(first_start.contains(QStringLiteral("-p")) &&
                    first_start.contains(QStringLiteral("work")),
                "Codex receives the selected profile");
        require(
            codex->sendText("exit\r") &&
                waitFor([&] { return codex->connectionState() == QStringLiteral("ended"); }, 15000),
            "the stand-in exits");
        // The ended agent's service finishes its own exit first.
        require(waitFor([&] { return workspace.restartAgent(id); }, 10000),
                "the ended agent restarts");
        require(waitFor([&] { return started().size() == 2; }, 15000),
                "a restart starts Codex again");
        const auto restarted = QJsonDocument::fromJson(started().at(1).toUtf8()).array();
        require(restarted.contains(QStringLiteral("resume")) && restarted.contains(thread) &&
                    restarted.at(0) == QStringLiteral("-c") &&
                    restarted.contains(QStringLiteral("-p")) &&
                    restarted.contains(QStringLiteral("work")),
                "the restart resumes the saved thread, with the notify program again");
        auto* again = workspace.agent(id);
        require(again != nullptr && waitFor(
                                        [&] {
                                            return screenText(again->snapshot())
                                                .contains(QStringLiteral("codex ready"));
                                        },
                                        15000),
                "the resumed stand-in starts");
        require(waitFor([&] { return again->reachable(); }, 15000),
                "the resumed agent's service is reachable");
        require(workspace.closeAgent(id), "close the stand-in agent");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
                "the stand-in agent closes");
        require(waitFor([&] { return !servicesListening(); }, 10000), "its session service exits");
    }
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
            const auto* item = workspace.agent(id);
            return item != nullptr && item->inputReady();
        };
        const auto project = root.filePath(QStringLiteral("project"));
        require(workspace.createAgent(project, QStringLiteral("one"), QStringLiteral("grok")),
                "a first agent");
        const auto one = workspace.focusedAgent()->agentId();
        require(workspace.createAgent(project, QStringLiteral("two"), QStringLiteral("grok")),
                "a second agent");
        const auto two = workspace.focusedAgent()->agentId();
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
        const auto far = workspace.focusedAgent()->agentId();
        require(waitFor([&] { return ready(far); }, 10000), "it runs");
        require(workspace.reloadAgent(far) == 0 &&
                    workspace.workspaceError().contains(QStringLiteral("/resume")) && ready(far),
                "an agent whose conversation lapis cannot name is left running");
        workspace.clearError();
        require(workspace.reloadAll() == 2 &&
                    waitFor([&] { return starts() == 7 && ready(one) && ready(two); }, 10000),
                "reloading the window starts the rest again");
        for (const auto& closing : {one, two, far})
            require(workspace.closeAgent(closing, true), "close the stand-in agents");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
        auto* item = workspace.focusedAgent();
        require(
            item != nullptr &&
                waitFor(
                    [item] {
                        return screenText(item->snapshot()).contains(QStringLiteral("size 27 91"));
                    },
                    10000),
            "its CLI starts at the stage's grid");
        require(waitFor([item] { return item->inputReady(); }, 10000),
                "its terminate handshake is ready");
        require(workspace.closeAgent(item->agentId(), true), "close it");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000), "it closes");
    }
    qputenv("PATH", path);
}

// An agent restarted as the window opens waits for the stage's grid, so a
// resumed conversation is drawn at the width it is shown in, not the
// service's default that the window then widens.
void restoredAgentsStartAtTheStageSize() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-rsize-XXXXXX"));
    require(directory.isValid(), "restore size directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const auto script = QDir(canonical).filePath(QStringLiteral("agent.sh"));
    writeExecutable(script, "#!/bin/sh\necho \"size $(stty size)\"\nexec sleep 600\n");
    const QString id = uuid();
    auto record = agentRecord(canonical, id, "general");
    record.insert(QStringLiteral("program"), script);
    // A terminal CLI: a record without a harness is Codex, which runs its
    // app-server instead of drawing the stand-in's output.
    record.insert(QStringLiteral("harness"), QStringLiteral("grok"));
    WorkspaceOptions options;
    options.restoreAgents = true;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{
                                       {"id", "general"}, {"name", "General"}, {"selected", id}}}},
                    {"agents", QJsonArray{record}}});
    {
        Workspace workspace(WorkspaceMode::live, options);
        auto* item = workspace.focusedAgent();
        require(item != nullptr, "the restored agent is shown");
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < 500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        require(!screenText(item->snapshot()).contains(QStringLiteral("size")),
                "it waits for the stage's grid");
        workspace.setLaunchSize(QSize(91, 27));
        require(waitFor(
                    [item] {
                        return screenText(item->snapshot()).contains(QStringLiteral("size 27 91"));
                    },
                    10000),
                "it starts at the stage's grid");
        require(waitFor([item] { return item->inputReady(); }, 10000),
                "its terminate handshake is ready");
        require(workspace.closeAgent(item->agentId(), true), "close it");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000), "it closes");
    }
}

// Agents saved while lapis managed Claude Code and Codex plans still load and
// start. Their retired "account" field is ignored and dropped on the next
// save; a remote command saved with a plan preamble loses it at its next
// start, and a local one gets no plan credential, so both use the machine's
// own sign-in.
void retiredPlansLeaveSavedAgentsStartable() {
    QTemporaryDir directory(QCoreApplication::applicationDirPath() +
                            QStringLiteral("/../../ra-XXXXXX"));
    require(directory.isValid(), "retired plan registry directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    require(root.mkpath(QStringLiteral("project")) && root.mkpath(QStringLiteral("bin")),
            "create the retired-plan fixture");
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
    const auto local_id = uuid();
    auto local = agentRecord(root.path(), local_id, "general");
    local.insert(QStringLiteral("harness"), QStringLiteral("claude"));
    local.insert(QStringLiteral("account"), QStringLiteral("spare"));
    writeRegistry(root.filePath(QStringLiteral("workspace.json")),
                  {{"version", 2},
                   {"activeCategory", "general"},
                   {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                   {"agents", QJsonArray{record, local}}});
    // A local agent's environment also carries the visual view's chart style
    // once one is written under HOME, unless MATPLOTLIBRC is set. Point both
    // at the fixture so the developer's own home cannot change the result.
    const auto original_home = qgetenv("HOME");
    const auto original_matplotlibrc = qgetenv("MATPLOTLIBRC");
    const bool had_matplotlibrc = qEnvironmentVariableIsSet("MATPLOTLIBRC");
    const auto restore_environment = qScopeGuard([&] {
        qputenv("HOME", original_home);
        if (had_matplotlibrc)
            qputenv("MATPLOTLIBRC", original_matplotlibrc);
        else
            qunsetenv("MATPLOTLIBRC");
    });
    qputenv("HOME", QFile::encodeName(project));
    qunsetenv("MATPLOTLIBRC");
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.agents().size() == 2 && workspace.workspaceError().isEmpty(),
            "agents saved with a plan load without an error");
    require(workspace.restartAgent(id) && workspace.restartAgent(local_id),
            "agents saved with a plan restart");
    auto* local_item = workspace.agent(local_id);
    require(local_item != nullptr && local_item->serviceEnvironment().isEmpty(),
            "a local agent saved with a plan starts with no plan credential");
    const auto saved = QJsonDocument::fromJson(readRegistry(options.storagePath))
                           .object()
                           .value(QStringLiteral("agents"))
                           .toArray();
    QJsonObject saved_remote;
    for (const auto& value : saved) {
        require(!value.toObject().contains(QStringLiteral("account")),
                "saving drops the retired plan field");
        if (value.toObject().value(QStringLiteral("id")).toString() == id)
            saved_remote = value.toObject();
    }
    require(saved_remote.value(QStringLiteral("arguments")).toArray().last().toString() ==
                    base_command &&
                !QJsonDocument(saved_remote).toJson(QJsonDocument::Compact).contains("{ a=spare"),
            "a restart drops the saved remote plan preamble");
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
    auto* item = workspace.agent(id);
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
    auto* running = workspace.agent(working_id);
    require(waitFor([&] { return running->inputReady(); }, 10000), "the valid peer starts");
    require(workspace.closeAgent(working_id) &&
                waitFor([&] { return workspace.agent(working_id) == nullptr; }, 10000),
            "batch fixture closes its child");
}

// Agents that start while their CLI updates share one installer and start
// once it ends, whether it succeeded or not.
void startupUpdatesShareOneInstaller() {
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
        const auto flag = [&](const char* name) {
            QFile file(fixture.root.filePath(QStringLiteral("bin/") + QLatin1String(name)));
            require(file.open(QIODevice::WriteOnly), "create updater flag");
        };
        flag("hold");
        if (failed)
            flag("fail");
        Workspace workspace(WorkspaceMode::live, fixture.options);
        fixture.create(workspace);
        auto* one = workspace.focusedAgent();
        const auto first_id = one->agentId();
        fixture.create(workspace);
        auto* two = workspace.focusedAgent();
        const auto second_id = two->agentId();
        require(
            waitFor([&] { return QFileInfo::exists(fixture.root.filePath("bin/updates")); }, 10000),
            "the one installer started");
        require(fixture.read("bin/updates") == "update\n" && !one->live() && !two->live(),
                "both agents wait for it");
        require(QFile::remove(fixture.root.filePath("bin/hold")), "release installer");
        require(waitFor(
                    [&] {
                        return one->inputReady() && two->inputReady() &&
                               QFileInfo::exists(fixture.root.filePath("bin/starts")) &&
                               fixture.read("bin/starts").count('\n') == 2 &&
                               one->statusLabel() != QStringLiteral("Updating Grok…");
                    },
                    10000),
                "each starts once, whatever the result");
        require(fixture.read("bin/updates") == "update\n", "both used one installer");
        for (const auto& id : {first_id, second_id})
            require(workspace.closeAgent(id, true), "close shared updater consumer");
        require(waitFor([&] { return closedFully(workspace); }, 10000), "consumers close");
    }
}

// OMP's inline UI rebuilds its scrollback on resize unless its config overlay
// says preserve. New local agents get that overlay through their service
// environment; a remote machine does not receive lapis's local file.
void ompGetsItsResizeOverlay() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-omp-overlay-XXXXXX"));
    require(directory.isValid(), "OMP overlay directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    require(root.mkpath(QStringLiteral("bin")) && root.mkpath(QStringLiteral("project")),
            "fixture folders");
    const auto capture = root.filePath(QStringLiteral("bin/env"));
    const auto remoteCapture = root.filePath(QStringLiteral("bin/remote-env"));
    QFile script(root.filePath(QStringLiteral("bin/omp")));
    require(script.open(QIODevice::WriteOnly), "write the stand-in OMP");
    script.write(QByteArray("#!/bin/sh\n"
                            "if [ \"$1\" = update ]; then echo updated; exit 0; fi\n"
                            "if [ \"${LAPIS_REMOTE_TEST:-0}\" = 1 ]; then\n"
                            "  printf '%s' \"$PI_CONFIG_FILES\" > '") +
                 remoteCapture.toUtf8() +
                 QByteArray("'\nelse\n"
                            "  printf '%s' \"$PI_CONFIG_FILES\" > '") +
                 capture.toUtf8() +
                 QByteArray("'\nfi\n"
                            "echo omp ready\nexec sleep 600\n"));
    script.close();
    require(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make the stand-in executable");
    // The other machine runs its command in a stand-in login shell with a
    // fixture home: the user's own login shell would read their profiles,
    // which can reorder PATH or define an `omp` function, and so run the
    // real CLI instead of the stand-in.
    require(root.mkpath(QStringLiteral("home")), "a home on the other machine");
    writeExecutable(root.filePath(QStringLiteral("bin/ssh")), R"(#!/bin/sh
for last; do :; done
d=$(cd "$(dirname "$0")/.." && pwd)
export HOME="$d/home" SHELL="$d/bin/login-shell"
cd "$HOME" && exec env -u PI_CONFIG_FILES LAPIS_REMOTE_TEST=1 /bin/sh -c "$last"
)");
    writeExecutable(root.filePath(QStringLiteral("bin/login-shell")), R"(#!/bin/sh
[ "$1" = -lic ] && exec /bin/sh -c "$2"
exit 64
)");
    QFile config(root.filePath(QStringLiteral("ssh_config")));
    require(config.open(QIODevice::WriteOnly), "write an ssh config");
    config.write("Host devbox\n");
    config.close();
    const auto path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(root.filePath(QStringLiteral("bin"))) + ':' + path);
    // A user's shell may already export PI_CONFIG_FILES. Pin it here so this
    // test does not inherit the developer's overlays, and so the append that
    // the OMP service performs can be asserted exactly.
    const auto previousConfig = qgetenv("PI_CONFIG_FILES");
    const bool hadConfig = qEnvironmentVariableIsSet("PI_CONFIG_FILES");
    const auto userOverlay = root.filePath(QStringLiteral("user.yml"));
    qputenv("PI_CONFIG_FILES", QFile::encodeName(userOverlay));
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        workspace.setSshConfigForTesting(config.fileName());
        require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                      QStringLiteral("overlay"), QStringLiteral("omp")),
                "an OMP stand-in");
        auto* agent = workspace.focusedAgent();
        require(agent != nullptr && waitFor([&] { return agent->inputReady(); }, 10000),
                "the OMP stand-in starts");
        const auto overlay = root.filePath(QStringLiteral("omp-lapis.yml"));
        QFile written(overlay);
        require(written.open(QIODevice::ReadOnly), "lapis writes its OMP overlay");
        const auto overlay_text = written.readAll();
        require(overlay_text.contains("resizeScrollback: preserve"),
                "the overlay preserves scrollback on resize");
        const auto extension = root.filePath(QStringLiteral("omp-lapis-turns.ts"));
        require(overlay_text.contains("extensions:\n  - \"" + QFile::encodeName(extension) + "\""),
                "the overlay loads lapis's OMP turn extension");
        QFile turns(extension);
        require(turns.open(QIODevice::ReadOnly), "read lapis's OMP turn extension");
        const auto turn_source = turns.readAll();
        require(turn_source.contains("lapis-init;omp;${nonce}") &&
                    turn_source.contains(
                        QByteArrayLiteral("const main = (ctx: any) => ctx?.mode === \"tui\";")),
                "the extension reports only OMP's top-level TUI runner");
        // The stand-in's shell truncates the capture before it writes, so an
        // existence check alone can read an empty file under load.
        const auto expected_config = userOverlay + QDir::listSeparator() + overlay;
        require(waitFor(
                    [&] {
                        QFile probe(capture);
                        return probe.open(QIODevice::ReadOnly) &&
                               QString::fromUtf8(probe.readAll()) == expected_config;
                    },
                    5000),
                "the stand-in recorded its environment");
        QFile captured(capture);
        require(captured.open(QIODevice::ReadOnly), "the stand-in recorded its environment");
        require(QString::fromUtf8(captured.readAll()) == expected_config,
                "the OMP service keeps the user's overlays and appends lapis");
        require(workspace.createAgent(QStringLiteral("~"), QStringLiteral("remote overlay"),
                                      QStringLiteral("omp"), {}, {}, QStringLiteral("devbox")),
                "an OMP stand-in on another machine");
        auto* remote = workspace.focusedAgent();
        require(remote != nullptr && remote != agent, "the remote OMP stand-in is shown");
        require(waitFor(
                    [&] {
                        return remote->inputReady() && QFileInfo::exists(remoteCapture) &&
                               screenText(remote->snapshot()).contains(QStringLiteral("omp ready"));
                    },
                    10000),
                "the remote OMP stand-in starts");
        QFile remoteCaptured(remoteCapture);
        require(remoteCaptured.open(QIODevice::ReadOnly),
                "the remote stand-in recorded its environment");
        require(remoteCaptured.readAll().isEmpty(),
                "the local OMP overlay does not cross the ssh boundary");
        require(workspace.closeAgent(agent->agentId()), "close the local OMP stand-in");
        require(workspace.closeAgent(remote->agentId()), "close the remote OMP stand-in");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
                "the OMP stand-ins close");
    }
    if (hadConfig)
        qputenv("PI_CONFIG_FILES", previousConfig);
    else
        qunsetenv("PI_CONFIG_FILES");
    qputenv("PATH", path);
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
        auto* agent = workspace.focusedAgent();
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
        require(workspace.closeAgent(agent->agentId()), "close the stand-in");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
        auto* agent = workspace.focusedAgent();
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
        require(workspace.closeAgent(agent->agentId()), "close the stand-in");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
        auto* agent = workspace.focusedAgent();
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
        require(workspace.closeAgent(agent->agentId()), "close the stand-in");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
        id = current->agentId();
        require(waitFor([current] { return current->inputReady(); }, 10000) &&
                    waitFor(
                        [current] {
                            return screenText(current->snapshot())
                                .contains(QStringLiteral("shell ready -l -i"));
                        },
                        5000),
                "a login shell starts at home");
        require(terminals.show(QString()) && terminals.current()->agentId() == id,
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
        require(terminals.show(QString()) && terminals.current()->agentId() != id,
                "the next one is a fresh shell");
        auto* fresh = terminals.current();
        require(fresh != nullptr, "a fresh terminal");
        const auto fresh_id = fresh->agentId();
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

// An effort asked for over the control socket reaches the CLI as its own
// option, after the model; a CLI without one refuses it rather than ignoring it.
void askedEffortReachesTheCli() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-effort-XXXXXX"));
    require(directory.isValid(), "effort directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    const auto restore_path = qScopeGuard([&] { qputenv("PATH", path); });
    QFile script(root.filePath(QStringLiteral("bin/claude")));
    require(script.open(QIODevice::WriteOnly), "write the stand-in Claude Code");
    script.write("#!/bin/sh\necho \"claude args: $*\"\nexec sleep 600\n");
    script.close();
    require(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make it executable");
    QFile codex_script(root.filePath(QStringLiteral("bin/codex")));
    require(codex_script.open(QIODevice::WriteOnly), "write the stand-in Codex CLI");
    codex_script.write("#!/bin/sh\necho \"codex args: $*\"\nexec sleep 600\n");
    codex_script.close();
    require(codex_script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make the Codex stand-in executable");
    QFile ssh(root.filePath(QStringLiteral("bin/ssh")));
    require(ssh.open(QIODevice::WriteOnly), "write the stand-in ssh");
    ssh.write("#!/bin/sh\n"
              "last=\n"
              "for argument in \"$@\"; do last=$argument; done\n"
              "exec /bin/sh -c \"$last\"\n");
    ssh.close();
    require(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make the ssh stand-in executable");
    QFile config(root.filePath(QStringLiteral("ssh_config")));
    require(config.open(QIODevice::WriteOnly), "write an ssh config");
    config.write("Host devbox\n");
    config.close();
    const auto project = root.filePath(QStringLiteral("project"));
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    Workspace workspace(WorkspaceMode::live, options);
    workspace.setSshConfigForTesting(config.fileName());
    const QString configured_model = QStringLiteral("custom model [v2]");
    lapis::desktop::AgentDefaults defaults;
    defaults.models.insert(QStringLiteral("claude"), {configured_model});
    defaults.models.insert(QStringLiteral("codex"), {configured_model});
    workspace.setAgentDefaults(defaults);
    const auto close_owned = qScopeGuard([&] {
        for (const auto& value : workspace.agents())
            if (auto* item = value.value<lapis::desktop::Agent*>())
                static_cast<void>(workspace.closeAgent(item->agentId()));
        static_cast<void>(waitFor([&] { return closedFully(workspace); }, 10000));
    });
    lapis::desktop::WorkspaceControl control(workspace, false);
    require(!workspace.createAgent(project, QStringLiteral("invalid model"),
                                   QStringLiteral("claude"), QStringLiteral("not configured *")),
            "an arbitrary model outside the strict launch grammar is refused");
    auto request = createRequest(workspace.activeCategoryId(), QStringLiteral("claude"), project);
    request.insert(QStringLiteral("model"), configured_model);
    request.insert(QStringLiteral("effort"), QStringLiteral("medium"));
    const auto started = askWorkspace(workspace.storagePath(), request);
    auto* agent = workspace.agent(started.value(QStringLiteral("id")).toString());
    require(agent != nullptr &&
                waitFor(
                    [agent] {
                        return screenText(agent->snapshot())
                            .remove(QLatin1Char('\n'))
                            .contains(QStringLiteral("--model custom model [v2] --effort medium "
                                                     "--permission-mode bypassPermissions"));
                    },
                    10000),
            "Claude Code starts with the effort after its model");
    auto codex_request =
        createRequest(workspace.activeCategoryId(), QStringLiteral("codex"), project);
    codex_request.insert(QStringLiteral("machine"), QStringLiteral("devbox"));
    codex_request.insert(QStringLiteral("program"), root.filePath(QStringLiteral("bin/codex")));
    codex_request.insert(QStringLiteral("model"), configured_model);
    codex_request.insert(QStringLiteral("effort"), QStringLiteral("max"));
    const auto codex_started = askWorkspace(workspace.storagePath(), codex_request);
    auto* codex_agent = workspace.agent(codex_started.value(QStringLiteral("id")).toString());
    require(codex_agent != nullptr &&
                waitFor(
                    [codex_agent] {
                        return screenText(codex_agent->snapshot())
                                   .remove(QLatin1Char('\n'))
                                   .contains(QStringLiteral("-m custom model [v2]")) &&
                               screenText(codex_agent->snapshot())
                                   .remove(QLatin1Char('\n'))
                                   .contains(QStringLiteral("-c model_reasoning_effort=max"));
                    },
                    10000),
            "Codex accepts the maximum effort");
    for (const auto& [harness, effort] :
         {std::pair{"grok", "medium"}, std::pair{"claude", "extreme"}}) {
        auto refused =
            createRequest(workspace.activeCategoryId(), QString::fromLatin1(harness), project);
        refused.insert(QStringLiteral("effort"), QString::fromLatin1(effort));
        require(
            !askWorkspace(workspace.storagePath(), refused).value(QStringLiteral("ok")).toBool(),
            "an effort the CLI cannot take is refused");
    }
    require(workspace.agents().size() == 2, "a refused effort starts nothing");
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
            for (const auto& value : workspace.agents())
                if (auto* item = value.value<lapis::desktop::Agent*>())
                    static_cast<void>(workspace.closeAgent(item->agentId()));
            static_cast<void>(waitFor([&] { return closedFully(workspace); }, 10000));
        });
        require(!workspace.resumeAgent(project, QStringLiteral("x"), QStringLiteral("grok"),
                                       QStringLiteral("-rf")),
                "an option is never taken for a conversation");
        require(workspace.resumeAgent(project, QStringLiteral("project"), QStringLiteral("grok"),
                                      QStringLiteral("conv-123")),
                "a past conversation resumes, named after its folder");
        auto* agent = workspace.focusedAgent();
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
        auto* phone = workspace.agent(started.value(QStringLiteral("id")).toString());
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
                                return entry[QStringLiteral("id")] == agent->agentId() &&
                                       managed.value(QStringLiteral("identity")) ==
                                           QStringLiteral("conv-123");
                            }),
                "the resume pair is recorded as lapis's");
        // An agent named after its folder takes its conversation's title; a
        // name chosen on the Mac or the phone stays.
        const auto conversations = workspace.agentConversations();
        require(conversations.value(agent->agentId()) == QStringLiteral("conv-123") &&
                    conversations.value(phone->agentId()) == QStringLiteral("conv-456"),
                "each agent's conversation is known");
        require(workspace.followConversationTitle(agent->agentId(),
                                                  QStringLiteral("  Fix   the resize bug ")) &&
                    agent->title() == QStringLiteral("Fix the resize bug"),
                "the conversation's title names the agent");
        require(
            workspace.followConversationTitle(agent->agentId(), QStringLiteral("After /clear")) &&
                agent->title() == QStringLiteral("After /clear"),
            "it keeps following its conversation");
        const auto rocket = QString::fromUcs4(U"\U0001f680");
        const auto long_title = rocket.repeated(41);
        const auto elided_title = rocket.repeated(39) + QChar(0x2026);
        require(workspace.followConversationTitle(agent->agentId(), long_title) &&
                    agent->title() == elided_title,
                "auto titles keep complete supplementary Unicode scalars");
        // A name from before chosen names were recorded stays.
        require(
            workspace.createAgent(project, QFileInfo(project).fileName(), QStringLiteral("grok")),
            "an agent with its own name");
        auto* own = workspace.focusedAgent();
        require(own != nullptr, "the named agent is shown");
        require(!workspace.followConversationTitle(own->agentId(), QFileInfo(project).fileName()) &&
                    own->title() == QFileInfo(project).fileName(),
                "an explicit folder default is user-owned");
        const auto renamed = askWorkspace(
            workspace.storagePath(), {{QStringLiteral("version"), 1},
                                      {QStringLiteral("request"), QStringLiteral("renameAgent")},
                                      {QStringLiteral("id"), phone->agentId()},
                                      {QStringLiteral("title"), QStringLiteral("Named here")}});
        require(renamed.value(QStringLiteral("ok")).toBool() &&
                    phone->title() == QStringLiteral("Named here") &&
                    !workspace.followConversationTitle(phone->agentId(),
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
                                    return entry[QStringLiteral("id")] == phone->agentId() &&
                                           entry[QStringLiteral("named")].toBool() &&
                                           entry[QStringLiteral("title")] ==
                                               QStringLiteral("Named here");
                                }),
                    "the chosen name is saved as chosen");
            require(std::any_of(saved_agents.begin(), saved_agents.end(),
                                [&](const QJsonValue& entry) {
                                    return entry[QStringLiteral("id")] == own->agentId() &&
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
        for (const auto& closing : {agent->agentId(), phone->agentId(), own->agentId()})
            require(workspace.closeAgent(closing), "close the stand-in agents");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
        auto* literal = workspace.focusedAgent();
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
        require(workspace.closeAgent(literal->agentId()), "close the literal-argument fixture");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
    const auto* focused = workspace.focusedAgent();
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
    require(workspace.activeCategoryId() == shown && workspace.focusedAgent() == focused,
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
    lapis::desktop::Config config_model;
    config_model.setSourcePathForTesting(file.fileName());
    require(config_model.load(), "the config loads");
    control.setConfig(&config_model);
    const auto shown =
        askVersioned(workspace, {{QStringLiteral("request"), QStringLiteral("settings")}})
            .value(QStringLiteral("settings"))
            .toObject();
    require(shown.value(QStringLiteral("keepAwake")).toBool(),
            "the phone reads the Mac's settings");
    // Older phone builds decode the retired request chime's two settings and
    // the retired usage meter's and notifications' switches, and still send
    // them: the answer carries them as off, and changes are ignored.
    require(shown.value(QStringLiteral("alertSound")) == QJsonValue(false) &&
                shown.value(QStringLiteral("alertRepeat")) == QJsonValue(1) &&
                shown.value(QStringLiteral("showUsage")) == QJsonValue(false) &&
                shown.value(QStringLiteral("notify")) == QJsonValue(false),
            "the answer keeps the retired settings an older phone requires");
    const auto changed = askVersioned(
        workspace, {{QStringLiteral("request"), QStringLiteral("changeSettings")},
                    {QStringLiteral("settings"), QJsonObject{{QStringLiteral("keepAwake"), false},
                                                             {QStringLiteral("alertSound"), true},
                                                             {QStringLiteral("alertRepeat"), 11},
                                                             {QStringLiteral("showUsage"), true},
                                                             {QStringLiteral("notify"), true}}}});
    const auto changed_settings = changed.value(QStringLiteral("settings")).toObject();
    require(answeredOk(changed) && !config_model.keepAwake() &&
                !changed_settings.value(QStringLiteral("keepAwake")).toBool() &&
                changed_settings.value(QStringLiteral("alertSound")) == QJsonValue(false) &&
                changed_settings.value(QStringLiteral("showUsage")) == QJsonValue(false) &&
                changed_settings.value(QStringLiteral("notify")) == QJsonValue(false),
            "the phone changes them, and the retired settings are ignored");
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
                    !config_model.keepAwake(),
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
                    !config_model.keepAwake(),
                "a missing or non-object settings value is refused");
    }

    // A later save can fail even though the request validated. The owner must
    // not leave a mixed batch in memory or replace the damaged file.
    const auto before_failure = config_model.remoteSettings();
    const auto malformed_path = root.filePath(QStringLiteral("broken-lapis.json"));
    const QByteArray malformed = "{unfinished remote edit";
    {
        QSaveFile replacement(malformed_path);
        require(replacement.open(QIODevice::WriteOnly), "open the replacement");
        replacement.write(malformed);
        require(replacement.commit(), "commit the malformed replacement");
    }
    config_model.setSourcePathForTesting(malformed_path);
    const auto save_failed = askVersioned(
        workspace,
        {{QStringLiteral("request"), QStringLiteral("changeSettings")},
         {QStringLiteral("settings"), QJsonObject{{QStringLiteral("keepAwake"), true},
                                                  {QStringLiteral("finishSound"), false}}}});
    require(!answeredOk(save_failed) &&
                save_failed.value(QStringLiteral("settings")).toObject() ==
                    QJsonObject::fromVariantMap({{QStringLiteral("keepAwake"), false},
                                                 {QStringLiteral("alertSound"), false},
                                                 {QStringLiteral("finishSound"), true},
                                                 {QStringLiteral("alertRepeat"), 1},
                                                 {QStringLiteral("notify"), false},
                                                 {QStringLiteral("showUsage"), false}}) &&
                config_model.remoteSettings() == before_failure,
            "a failed save echoes unchanged settings and preserves memory");
    require(save_failed.value(QStringLiteral("error"))
                .toString()
                .contains(QStringLiteral("Could not save")),
            "a failed save keeps a useful diagnostic");
    QFile damaged(malformed_path);
    require(damaged.open(QIODevice::ReadOnly), "read the failed-save config");
    require(damaged.readAll() == malformed, "a failed save preserves disk bytes");
    control.setConfig(nullptr);
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
        auto* desk = workspace.focusedAgent();
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

        const auto count = workspace.agents().size();
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
        require(workspace.agents().size() == count, "refused requests start nothing");

        const auto started =
            askWorkspace(registry, createRequest(later, QStringLiteral("grok"), project));
        const auto id = started.value(QStringLiteral("id")).toString();
        require(started.value(QStringLiteral("ok")).toBool() && !id.isEmpty() &&
                    !started.value(QStringLiteral("updating")).toBool(),
                "the phone starts an agent");
        require(workspace.activeCategoryId() == desk_category && workspace.focusedAgent() == desk,
                "the window keeps its category and agent");
        auto* agent = workspace.agent(id);
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
        auto* far = workspace.agent(over_ssh.value(QStringLiteral("id")).toString());
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
        auto* mac_far = workspace.focusedAgent();
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
        for (const auto& closing : {desk->agentId(), id, far->agentId(), mac_far->agentId()})
            require(workspace.closeAgent(closing), "close the stand-in agents");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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

// Only the moment an agent becomes unseen chimes, an open request included;
// looking, a mark already set, a screen already seen or turning the sound off
// keeps it quiet. The workspace's mark decides (Workspace::needsYou).
void alertsChimeWhileAnAgentWaits() {
    namespace wire = lapis::session::wire;
    QTemporaryDir directory;
    require(directory.isValid(), "alerts directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    QFile config(root.filePath(QStringLiteral("lapis.json")));
    require(config.open(QIODevice::WriteOnly), "write the config");
    // "sound" and "repeat" are the retired request chime's, and "notify",
    // "awayAfter", "remindAfter" and "judge" the retired notifications' and
    // ping judge's, as "tabAway.rank" is the retired learned order's; older
    // files keep them.
    config.write(R"({"version": 1, "alerts": {"sound": true, "finished": true, "repeat": 3,
        "notify": true, "awayAfter": 5, "remindAfter": 9999, "judge": true},
        "tabAway": {"rank": "learned"}})");
    config.close();
    lapis::desktop::Config config_model;
    config_model.setSourcePathForTesting(config.fileName());
    require(config_model.load() && config_model.diagnostic().isEmpty(),
            "alerts config with the retired keys loads cleanly");
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    Workspace workspace(WorkspaceMode::live, options);
    lapis::desktop::Agent agent(QStringLiteral("agent"), root.path(), {}, QColor(), "");
    std::vector<lapis::desktop::Chime> played;
    bool looking = false;
    lapis::desktop::Alerts alerts(workspace, config_model, [&played](lapis::desktop::Chime chime) {
        played.push_back(chime);
    });
    alerts.setQuietForTesting(40);
    const auto ping = [&workspace, &agent](const char* outcome) {
        emit workspace.needsYou(&agent, QString::fromLatin1(outcome));
    };
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
    request(true);
    // Requests share the one finished-turn cue, and it does not repeat.
    ping("marked");
    waitFor([] { return false; }, 300);
    require(played == std::vector<lapis::desktop::Chime>{lapis::desktop::Chime::finished},
            "a request chimes once with the finished-turn cue");
    for (const auto* again : {"waiting", "repeat"}) {
        played.clear();
        waitFor([] { return false; }, 60);
        ping(again);
        require(played.empty(), "an agent already unseen, or a request already marked, is quiet");
    }

    played.clear();
    waitFor([] { return false; }, 60);
    ping("looking");
    require(played.empty(), "nothing plays for the agent being looked at");
    request(false);

    played.clear();
    waitFor([] { return false; }, 60);
    ping("marked");
    require(played == std::vector{lapis::desktop::Chime::finished}, "a finished turn chimes once");

    // A turn that ends on what the person already saw there says nothing new:
    // no mark and no chime, whatever Claude Code's status line below its input
    // box does. New output above the box chimes again. Every decision is
    // logged. The workspace asks SeenScreens (setUnchanged); `finish` stands
    // in for that question here.
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
        const auto finish = [&] { ping(seen.unchanged(&agent) ? "unchanged" : "marked"); };
        // The null tolerance see() already documents: never on screen means
        // changed, so the chime path can never dereference a null item.
        require(!seen.unchanged(nullptr), "an item never on screen counts as changed");
        played.clear();
        waitFor([] { return false; }, 60);
        show("\x1b[5;1H5h 54%"); // the status line ticks
        finish();
        require(
            played.empty() && !lines.empty() &&
                lines.back().value(QStringLiteral("kind")).toString() == QStringLiteral("chime") &&
                lines.back().value(QStringLiteral("cli")).toString() == QStringLiteral("Codex") &&
                lines.back().contains(QStringLiteral("at")) &&
                lines.back().value(QStringLiteral("decision")).toString() ==
                    QStringLiteral("quiet: nothing new since you looked"),
            "a turn ending on a screen already seen stays quiet, logged in the shared shape");
        show("\x1b[1;1Hnew: build failed");
        finish();
        require(
            played == std::vector{lapis::desktop::Chime::finished} &&
                lines.back().value(QStringLiteral("decision")).toString() ==
                    QStringLiteral("chimed") &&
                lines.back().value(QStringLiteral("mark")).toString() == QStringLiteral("marked") &&
                lines.back().value(QStringLiteral("agent")).toString() == QStringLiteral("agent"),
            "new output chimes, and the decision names the agent and its mark");
        // The quiet path for the agent being looked at reaches the log too,
        // and what is on its screen then counts as seen.
        waitFor([] { return false; }, 60);
        show("\x1b[2;1Hwatched as it ended");
        played.clear();
        ping("looking");
        require(played.empty() &&
                    lines.back().value(QStringLiteral("event")).toString() ==
                        QStringLiteral("finished") &&
                    lines.back().value(QStringLiteral("decision")).toString() ==
                        QStringLiteral("quiet: you are looking at it") &&
                    seen.unchanged(&agent),
                "a turn for the agent being looked at stays quiet, says so, and is seen");
        // A lone rule is a divider in the agent's output, not Claude Code's
        // box: nothing is truncated, so output below it still counts as new.
        show("\x1b[2J\x1b[Hnote\r\n" + line + "\r\nstill watching\r\n");
        seen.see(&agent);
        show("\x1b[3;1Hchanged below the lone rule");
        played.clear();
        waitFor([] { return false; }, 60);
        finish();
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
        finish();
        require(played.empty() && lines.back().value(QStringLiteral("decision")).toString() ==
                                      QStringLiteral("quiet: nothing new since you looked"),
                "a box drawn with corners truncates, and a status line below it is not new");
        alerts.setSeen(nullptr);
        alerts.setLog({});
    }

    require(config_model.setFinishSound(false), "turn the sound off");
    {
        QFile written(config.fileName());
        require(written.open(QIODevice::ReadOnly), "read the saved alerts config");
        const auto root_object = QJsonDocument::fromJson(written.readAll()).object();
        const auto saved = root_object.value("alerts").toObject();
        require(root_object.value("tabAway").toObject().value("rank").toString() ==
                    QLatin1String("learned"),
                "saving keeps the retired Tab order key as written");
        require(!saved.value(QStringLiteral("finished")).toBool(true) &&
                    saved.value(QStringLiteral("judge")).toBool() &&
                    saved.value(QStringLiteral("notify")).toBool() &&
                    saved.value(QStringLiteral("awayAfter")).toInt() == 5 &&
                    saved.value(QStringLiteral("remindAfter")).toInt() == 9999 &&
                    saved.value(QStringLiteral("repeat")).toInt() == 3,
                "saving the chime keeps the retired alert keys as written");
    }
    played.clear();
    std::vector<QJsonObject> quiet_lines;
    alerts.setLog([&quiet_lines](const QJsonObject& entry) { quiet_lines.push_back(entry); });
    request(true);
    waitFor([] { return false; }, 60);
    ping("marked");
    waitFor([] { return false; }, 300);
    require(played.empty() && !quiet_lines.empty() &&
                quiet_lines.back().value(QStringLiteral("kind")).toString() ==
                    QStringLiteral("chime") &&
                quiet_lines.back().value(QStringLiteral("decision")).toString() ==
                    QStringLiteral("chime off"),
            "no chimes with the sound off, and that decision is logged");
    alerts.setLog({});

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
    lapis::desktop::Config config_model;
    config_model.setSourcePathForTesting(root.filePath(QStringLiteral("lapis.json")));
    lapis::desktop::ChimeSounds sounds;
    const auto ready = [&](Chime chime, const QByteArray& bytes) {
        return waitFor([&] { return sounds.sound(chime, config_model).bytes == bytes; }, 10000);
    };
    const auto diagnosed = [&](Chime chime, const QString& text) {
        return waitFor(
            [&] {
                static_cast<void>(sounds.sound(chime, config_model));
                return config_model.diagnostic().contains(text);
            },
            10000);
    };
    require(config_model.load() &&
                config_model.alertSoundFile() == root.filePath(QStringLiteral("ding.wav")),
            "relative sound path resolves without file I/O on the GUI thread");
    require(ready(Chime::needsYou, "RIFF-ding"), "background file load completes");
    require(sounds.sound(Chime::finished, config_model).volume == 0.5F,
            "the shared cue borrows soundFile at half volume");
    std::vector<ChimeSound> attempted;
    sounds.play(Chime::finished, config_model, [&](const ChimeSound& clip) {
        attempted.push_back(clip);
        return clip.path.isEmpty();
    });
    require(attempted.size() == 2 && attempted[0].volume == 0.5F && attempted[1].volume == 1.0F &&
                attempted[1].bytes == lapis::desktop::chime_wav(Chime::finished),
            "decode failure uses the synthesized cue at its own gain, not double attenuation");
    require(diagnosed(Chime::finished, QStringLiteral("could not be played")),
            "playback failure is visible");
    sounds.play(Chime::finished, config_model, [](const ChimeSound&) { return true; });
    require(waitFor([&] { return config_model.diagnostic().isEmpty(); }, 10000),
            "playback recovery clears its diagnostic");
    sounds.play(Chime::finished, config_model, [](const ChimeSound&) { return false; });
    require(diagnosed(Chime::finished, QStringLiteral("playback is unavailable")),
            "a failed synthesized fallback is visible too");
    sounds.play(Chime::finished, config_model, [](const ChimeSound&) { return true; });
    require(waitFor([&] { return config_model.diagnostic().isEmpty(); }, 10000),
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
    require(config_model.load() && diagnosed(Chime::needsYou, QStringLiteral("gone.wav")),
            "missing file diagnostic arrives from the loader");
    require(ready(Chime::finished, "RIFF-low") &&
                sounds.sound(Chime::finished, config_model).volume == 1.0F,
            "an explicit finishedFile uses its own volume");
    write(QStringLiteral("gone.wav"), "RIFF-created");
    require(ready(Chime::needsYou, "RIFF-created") &&
                waitFor([&] { return config_model.diagnostic().isEmpty(); }, 10000),
            "creating a file clears stale diagnostics without reloading configuration");
    require(QFile::remove(root.filePath(QStringLiteral("gone.wav"))), "remove configured file");
    require(diagnosed(Chime::needsYou, QStringLiteral("not found")) &&
                sounds.sound(Chime::needsYou, config_model).bytes ==
                    lapis::desktop::chime_wav(Chime::needsYou),
            "deletion is reported and falls back");

    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":"ding.wav"}})");
    require(config_model.load() && ready(Chime::needsYou, "RIFF-ding, edited"),
            "restore readable file");
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
    require(config_model.load() &&
                config_model.alertSoundFile() == root.filePath(QStringLiteral("ding.wav")),
            "tilde expansion works without changing the process HOME");
    write(QStringLiteral("lapis.json"),
          R"({"version":1,"alerts":{"soundFile":"ding.wav","finishedFile":"gone.wav"}})");
    require(config_model.load() && diagnosed(Chime::finished, QStringLiteral("gone.wav")) &&
                sounds.sound(Chime::finished, config_model).bytes ==
                    lapis::desktop::chime_wav(Chime::finished),
            "an unavailable explicit finishedFile never borrows the other clip");
    QFile big(root.filePath(QStringLiteral("big.wav")));
    require(big.open(QIODevice::WriteOnly) &&
                big.resize(lapis::desktop::ChimeSounds::kMaxFileBytes + 1),
            "oversized fixture");
    big.close();
    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":"big.wav"}})");
    require(config_model.load() && diagnosed(Chime::needsYou, QStringLiteral("over 4 MiB")),
            "oversized file is diagnosed asynchronously");
    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":12}})");
    require(config_model.load() &&
                config_model.diagnostic().contains(QStringLiteral("path string")),
            "malformed setting is diagnosed immediately");
    const auto fifo = root.filePath(QStringLiteral("pipe.wav"));
    require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0,
            "create special-file fixture");
    write(QStringLiteral("lapis.json"), R"({"version":1,"alerts":{"soundFile":"pipe.wav"}})");
    require(config_model.load() && diagnosed(Chime::needsYou, QStringLiteral("not a regular file")),
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
        auto* first = workspace.focusedAgent();
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
        auto* second = workspace.focusedAgent();
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
        for (const auto& id : {first->agentId(), second->agentId()})
            require(workspace.closeAgent(id), "close the stand-in agents");
        require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
    auto* agent = workspace.focusedAgent();
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
    require(workspace.closeAgent(agent->agentId()) &&
                waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
    auto* agent = workspace.focusedAgent();
    require(agent != nullptr, "service-backed agent");
    require(waitFor([agent] { return agent->inputReady(); }, 10000), "agent session ready");
    agent->beginHistoryRequest();
    require(!agent->inputReady() && agent->reachable(),
            "a history page hides input but the agent stays reachable");
    const QPointer<lapis::desktop::Agent> guarded(agent);
    const auto id = agent->agentId();
    require(workspace.closeAgent(id, true) && workspace.agents().isEmpty() &&
                workspace.closingAgentIds() == QStringList{id} && guarded && guarded->closing(),
            "closing from a history page ends the agent instead of abandoning it");
    require(waitFor([&workspace] { return workspace.closingAgentIds().isEmpty(); }, 10000),
            "the close finishes once the process has exited");
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
    auto* agent = workspace.agent(id);
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
    auto* native_item = workspace.agent(native_id);
    require(workspace.agent(remote_id) != nullptr && native_item != nullptr,
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
    require(workspace.closeAgent(native_id) && waitFor(
                                                   [&workspace, native_id] {
                                                       return workspace.agents().size() == 1 &&
                                                              workspace.agent(native_id) == nullptr;
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
        require(workspace.agents().isEmpty(), "failed restore retains no session objects");
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
        auto* item = workspace.agent(id);
        require(waitFor([item] { return item->inputReady(); }, 10000),
                "a restored agent runs under a new service");
    }
    require(!workspace.restartAgent(resumed), "a running agent is not restarted");
    workspace.clearError();
    // An agent that ended restarts in its card without reopening lapis, once
    // its service has exited.
    auto* ended = workspace.agent(fresh);
    ended->sendText("done\r");
    require(waitFor([ended] { return ended->connectionState() == QStringLiteral("ended"); }, 10000),
            "the agent ends");
    require(waitFor([&] { return workspace.restartAgent(fresh); }, 10000),
            "an ended agent restarts");
    workspace.clearError();
    require(waitFor([ended] { return ended->inputReady(); }, 10000),
            "the restarted agent runs again");
    for (const auto& id : {resumed, fresh, foreign})
        require(workspace.closeAgent(id), "close a restored agent");
    require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
            "restored agents close");
    // Command-Shift-T brings the last closed agent back as a new tab, in its
    // folder and category, running again.
    require(workspace.canReopenAgent(), "closed agents can come back");
    require(workspace.reopenAgent() && workspace.agents().size() == 1, "reopen the last one");
    auto* back = workspace.focusedAgent();
    // The three closed in whichever order their processes ended.
    require(back != nullptr && QStringList({resumed, fresh, foreign}).contains(back->title()) &&
                back->directory() == canonical,
            "the reopened agent keeps its name and folder");
    require(waitFor([back] { return back->inputReady(); }, 10000), "the reopened agent runs");
    require(workspace.closeAgent(back->agentId()), "close it again");
    require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
    auto* retained_item = workspace.agent(retained_id);
    auto* managed_item = workspace.agent(managed_id);
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

    require(workspace.closeAgent(managed_id) &&
                waitFor([&workspace] { return workspace.agents().size() == 1; }, 10000),
            "close the managed reopen fixture");
    require(workspace.canReopenAgent(), "the closed managed plan is retryable");
    const auto previous_focus = workspace.focusedAgent();
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
    require(workspace.agents().size() == 1 && workspace.agent(retained_id) == retained_item &&
                workspace.focusedAgent() == previous_focus &&
                workspace.activeCategoryId() == previous_category,
            "a failed reopen preserves the existing session and selection");
    require(QDir(canonical).entryList({QStringLiteral("args.*.txt")}, QDir::Files).isEmpty(),
            "a failed reopen starts no agent process");
    require(readRegistry(held_registry) == complete_registry,
            "a failed reopen leaves complete registry bytes unchanged");

    require(QDir(options.storagePath).removeRecursively(), "remove the save-failure directory");
    require(QFile::rename(held_registry, options.storagePath), "restore complete registry bytes");
    QJsonObject first_published_record;
    QObject::connect(&workspace, &Workspace::agentsChanged, &workspace, [&] {
        if (first_published_record.isEmpty() && workspace.agents().size() == 2)
            first_published_record = saved_agent(workspace.focusedAgent()->agentId());
    });
    require(workspace.reopenAgent() && workspace.agents().size() == 2,
            "a retryable reopen commits and starts");
    auto* reopened = workspace.focusedAgent();
    require(reopened != nullptr && reopened->title() == managed_id &&
                workspace.activeCategoryId() == QStringLiteral("closed"),
            "a retried reopen restores its category and selection");
    const auto root = QJsonDocument::fromJson(readRegistry(options.storagePath)).object();
    const auto agents = root.value(QStringLiteral("agents")).toArray();
    require(agents.size() == 2 &&
                agents.first().toObject().value(QStringLiteral("id")).toString() == retained_id &&
                agents.last().toObject().value(QStringLiteral("id")).toString() ==
                    reopened->agentId(),
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

    require(workspace.closeAgent(reopened->agentId()), "close the retried agent");
    require(waitFor([&workspace] { return workspace.agents().size() == 1; }, 10000),
            "the retried agent ends");
    require(workspace.closeAgent(retained_id), "close the retained fixture agent");
    require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
    auto* item = workspace.agent(id);
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
    require(workspace.closeAgent(id) &&
                waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
        require(bad.renameAgent(managed, QStringLiteral("recovered")),
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
    auto* managed_item = workspace.agent(managed);
    auto* explicit_item = workspace.agent(explicit_agent);
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
        require(workspace.closeAgent(id), "close a managed-resume agent");
    require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
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
    if (variant.harness == QLatin1String("codex")) {
        writeExecutable(QDir(fixture.root).filePath(QStringLiteral("codex")),
                        "#!/usr/bin/env bash\nexit 0\n");
        record.insert(QStringLiteral("program"),
                      QDir(fixture.root).filePath(QStringLiteral("codex")));
    }
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
    const auto retained =
        variant.managed_pair ? QJsonArray(base_arguments) : QJsonArray(user_arguments);
    QJsonArray plan = retained;
    if (followsObserverResume(variant) && plan.size() + 2 <= 64) {
        plan.append(QStringLiteral("resume"));
        plan.append(id);
    }
    // A direct Codex launch takes the startup default the workspace applies: no
    // update check, prepended, and dropped rather than truncated once the saved
    // argument limit is hit. Its screen stays full screen.
    QJsonArray missing;
    if (variant.harness == QLatin1String("codex")) {
        if (variant.add_update_setting) {
            missing.append(QStringLiteral("-c"));
            missing.append(QStringLiteral("check_for_update_on_startup=false"));
        }
    }
    if (missing.isEmpty() || plan.size() + missing.size() > 64)
        return plan; // The launch is already at the saved argument limit.
    QJsonArray expected = missing;
    for (const auto& value : plan)
        expected.append(value);
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
        require(managed.value(QStringLiteral("index")).toInt(-1) == expected.size() - 2 &&
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
          "-o", "ConnectTimeout=15", "-t", "fixture", "exec grok"}},
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

// Saved Codex launches gain the update setting once at restart, before any
// owned resume pair, unless the person chose it; lapis never picks the screen
// mode, so a screen setting of the person's own stays as written.
void savedCodexDefaultsKeepTheScreenMode() {
    struct Case {
        QStringList arguments;
        QStringList expected;
        bool managed{};
        QString program{QStringLiteral("codex")};
    };
    const QString update = QStringLiteral("check_for_update_on_startup=false");
    const std::vector<Case> cases{
        {{}, {"-c", update}},
        {{"-c", update}, {"-c", update}},
        {{"--no-alt-screen"}, {"-c", update, "--no-alt-screen"}},
        {{"-c", "tui.alt_screen=\"always\"", "-c", update},
         {"-c", "tui.alt_screen=\"always\"", "-c", update}},
        {{"--", "--no-alt-screen"}, {"-c", update, "--", "--no-alt-screen"}},
        {{"resume", "conversation"}, {"-c", update, "resume", "conversation"}, true},
        {{"-o", "ControlPath=none", "-t", "fixture", "codex"},
         {"-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4", "-o", "ConnectTimeout=15",
          "-o", "ControlPath=none", "-t", "fixture", "codex"},
         false,
         QStringLiteral("ssh")},
    };
    for (const auto& variant : cases) {
        QTemporaryDir directory(QStringLiteral("/tmp/lapis-codex-defaults-XXXXXX"));
        require(directory.isValid(), "Codex defaults directory");
        const QDir root(QFileInfo(directory.path()).canonicalFilePath());
        const auto id = uuid();
        auto record = agentRecord(root.path(), id, "general");
        const auto program = root.filePath(variant.program);
        writeExecutable(program, "#!/usr/bin/env bash\nexit 0\n");
        record.insert(QStringLiteral("program"), program);
        record.insert(QStringLiteral("harness"), QStringLiteral("codex"));
        record.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(variant.arguments));
        if (variant.managed)
            record.insert(QStringLiteral("managedResume"),
                          QJsonObject{{"index", 0}, {"identity", "conversation"}});
        WorkspaceOptions options;
        options.storagePath = root.filePath(QStringLiteral("workspace.json"));
        options.restoreAgents = true;
        writeRegistry(
            options.storagePath,
            {{"version", 2},
             {"activeCategory", "general"},
             {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
             {"agents", QJsonArray{record}}});
        for (int pass = 0; pass < 2; ++pass) {
            Workspace workspace(WorkspaceMode::live, options);
            require(workspace.workspaceError().isEmpty(), "restore saved Codex launch");
            const auto saved = QJsonDocument::fromJson(readRegistry(options.storagePath))
                                   .object()[QStringLiteral("agents")]
                                   .toArray()
                                   .first()
                                   .toObject();
            const auto expected = variant.expected;
            require(saved[QStringLiteral("arguments")].toArray() ==
                        QJsonArray::fromStringList(expected),
                    "Codex defaults are direct-local once, keeping explicit screen choices");
            if (variant.managed)
                require(saved[QStringLiteral("managedResume")]
                                .toObject()[QStringLiteral("index")]
                                .toInt(-1) == 2,
                        "the owned resume pair shifts exactly once with the defaults");
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
    auto* item = workspace.agent(id);
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
    require(workspace.closeAgent(id) && waitFor([&] { return closedFully(workspace); }, 10000),
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
    auto* legacy_item = legacy_workspace.agent(legacy.id);
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
    require(legacy_workspace.closeAgent(legacy.id) &&
                waitFor([&legacy_workspace] { return closedFully(legacy_workspace); }, 10000),
            "the legacy Claude fixture closes");
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
    auto* agent = workspace.focusedAgent();
    require(agent != nullptr, "service-backed agent");
    require(waitFor([agent] { return agent->inputReady(); }, 10000), "agent session ready");
    const auto id = agent->agentId();
    const QPointer<lapis::desktop::Agent> guarded(agent);
    QElapsedTimer clock;
    clock.start();
    require(workspace.closeAgent(id), "close request accepted");
    // Gone within the call, with no event-loop turn in between.
    const auto call_us = clock.nsecsElapsed() / 1000;
    require(workspace.agents().isEmpty() && workspace.focusedAgent() == nullptr &&
                workspace.closingAgentIds() == QStringList{id} && guarded && guarded->closing(),
            "the agent leaves every list before closeAgent returns");
    std::cout << "close-to-gone: " << call_us << " us inside closeAgent, 0 event-loop turns\n";
    require(waitFor([&workspace] { return workspace.closingAgentIds().isEmpty(); }, 10000),
            "the process ends in the background");
    require(clock.elapsed() >= minimum_ms, "escalation waited for the hangup first");
    require(workspace.workspaceError().isEmpty(), "no error on a normal close");
    require(waitFor([&guarded] { return guarded.isNull(); }, 2000), "the ended agent is deleted");
}

QStringList listedIds(const QVariantList& agents) {
    QStringList ids;
    for (const auto& value : agents)
        ids.append(value.value<lapis::desktop::Agent*>()->agentId());
    return ids;
}
QJsonObject registryRoot(const QString& path) {
    return QJsonDocument::fromJson(readRegistry(path)).object();
}
QStringList registryIds(const QString& path, const char* list) {
    QStringList ids;
    for (const auto& value : registryRoot(path).value(QLatin1String(list)).toArray())
        ids.append(value.toObject().value(QStringLiteral("id")).toString());
    return ids;
}

// Closing is instant: the agent leaves the sidebar, strip, counts and focus
// within closeAgent, the neighbour takes the stage as on final removal, and
// the registry lists it under "closing" until its process ends. Several
// closes in a row each go at once; the last agent of a category leaves it
// empty; the agents left keep running.
void closingIsInstant() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-close-XXXXXX"));
    require(directory.isValid(), "close directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        QStringList ids;
        for (const auto* title : {"one", "two", "three", "four"}) {
            require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                          QString::fromLatin1(title), QStringLiteral("grok")),
                    "a stand-in agent");
            ids.append(workspace.focusedAgent()->agentId());
        }
        for (const auto& value : workspace.agents()) {
            auto* item = value.value<lapis::desktop::Agent*>();
            require(waitFor([item] { return item->inputReady(); }, 10000), "it attaches");
        }
        const auto category = workspace.activeCategoryId();
        const auto count = [&workspace, &category] {
            for (const auto& value : workspace.categories()) {
                const auto group = value.toMap();
                if (group.value(QStringLiteral("id")).toString() == category)
                    return group.value(QStringLiteral("agentCount")).toInt();
            }
            return -1;
        };
        require(workspace.selectAgent(ids[1]), "show the second agent");
        int focus_changes = 0;
        QObject::connect(&workspace, &Workspace::focusChanged,
                         [&focus_changes] { ++focus_changes; });
        QElapsedTimer clock;
        clock.start();
        require(workspace.closeAgent(ids[1]), "close the shown agent");
        const auto call_us = clock.nsecsElapsed() / 1000;
        require(listedIds(workspace.agents()) == QStringList({ids[0], ids[2], ids[3]}) &&
                    listedIds(workspace.categoryAgents()) ==
                        QStringList({ids[0], ids[2], ids[3]}) &&
                    count() == 3 && focusedId(workspace) == ids[2] && focus_changes > 0 &&
                    workspace.agent(ids[1]) == nullptr,
                "within the call it is off every list and its right neighbour is shown");
        require(registryIds(options.storagePath, "agents") ==
                        QStringList({ids[0], ids[2], ids[3]}) &&
                    registryIds(options.storagePath, "closing") == QStringList{ids[1]},
                "the registry already lists it as closing, not as an agent");
        std::cout << "close-to-gone (4 agents, registry save included): " << call_us
                  << " us inside closeAgent, 0 event-loop turns\n";
        // Two more at once: the shown one and the last, whose left
        // neighbour then takes the stage.
        require(workspace.closeAgent(ids[2]) && workspace.closeAgent(ids[3]),
                "close two more in a row");
        require(listedIds(workspace.agents()) == QStringList{ids[0]} &&
                    focusedId(workspace) == ids[0] && count() == 1 &&
                    workspace.closingAgentIds().size() == 3,
                "both leave at once while three closes are in flight");
        require(workspace.closeAgent(ids[2]), "closing one already closing is accepted");
        require(waitFor([&workspace] { return workspace.closingAgentIds().isEmpty(); }, 10000),
                "all three end in the background");
        require(registryIds(options.storagePath, "closing").isEmpty() &&
                    !registryRoot(options.storagePath).contains(QStringLiteral("closing")),
                "ended agents leave the registry");
        auto* kept = workspace.agent(ids[0]);
        require(kept != nullptr && kept->inputReady() && workspace.workspaceError().isEmpty(),
                "the agent left keeps running, with no error");
        require(workspace.closeAgent(ids[0]) && workspace.agents().isEmpty() &&
                    workspace.focusedAgent() == nullptr && count() == 0,
                "the category's last agent leaves it empty at once");
        require(waitFor([&workspace] { return workspace.closingAgentIds().isEmpty(); }, 10000),
                "the last one ends");
    }
    qputenv("PATH", path);
}

// A close that does not finish brings the agent back to its place, still
// running, with an error: an agent that ignores the hangup and SIGTERM
// outlasts a short deadline here (the product's is 10 s; the service's
// SIGKILL comes at 3 s).
void failedCloseReturnsTheAgent() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-close-XXXXXX"));
    require(directory.isValid(), "close directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    {
        QFile script(root.filePath(QStringLiteral("bin/grok")));
        require(script.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite the stand-in");
        script.write("#!/bin/sh\ntrap '' HUP TERM\necho \"grok ready\"\nexec sleep 600\n");
    }
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    {
        Workspace workspace(WorkspaceMode::live, options);
        workspace.setCloseDeadlineForTesting(std::chrono::milliseconds(300));
        QStringList ids;
        for (const auto* title : {"left", "stubborn", "right"}) {
            require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                          QString::fromLatin1(title), QStringLiteral("grok")),
                    "a stand-in agent");
            ids.append(workspace.focusedAgent()->agentId());
        }
        for (const auto& value : workspace.agents()) {
            auto* item = value.value<lapis::desktop::Agent*>();
            require(waitFor([item] { return item->inputReady(); }, 10000), "it attaches");
        }
        require(workspace.selectAgent(ids[1]), "show the stubborn agent");
        auto* stubborn = workspace.agent(ids[1]);
        require(waitFor(
                    [stubborn] {
                        return screenText(stubborn->snapshot()).contains(QStringLiteral("ready"));
                    },
                    10000),
                "the stubborn agent ignores the hangup before it is closed");
        const bool could_reopen = workspace.canReopenAgent();
        require(workspace.closeAgent(ids[1]) && focusedId(workspace) == ids[2] &&
                    workspace.agent(ids[1]) == nullptr,
                "it leaves at once");
        require(waitFor([&workspace, &ids] { return workspace.agent(ids[1]) != nullptr; }, 3000),
                "past the deadline it comes back");
        auto* back = workspace.agent(ids[1]);
        require(listedIds(workspace.agents()) == ids && !back->closing() && back->live() &&
                    workspace.closingAgentIds().isEmpty() && focusedId(workspace) == ids[2],
                "in its old place, without taking the stage");
        require(workspace.workspaceError().contains(QStringLiteral("did not end within")),
                "with an error saying so");
        require(registryIds(options.storagePath, "agents") == ids &&
                    registryIds(options.storagePath, "closing").isEmpty(),
                "the registry lists it as an agent again");
        require(workspace.canReopenAgent() == could_reopen, "its reopen entry is taken back");
        // The service's SIGKILL ends it after all; it then closes at once.
        require(
            waitFor([back] { return back->connectionState() == QStringLiteral("ended"); }, 10000),
            "the process ends later");
        require(workspace.closeAgent(ids[1]) && workspace.agent(ids[1]) == nullptr &&
                    workspace.closingAgentIds().isEmpty(),
                "an ended agent is discarded at once");
        workspace.setCloseDeadlineForTesting(std::chrono::milliseconds(10000));
        for (const auto& id : {ids[0], ids[2]})
            require(workspace.closeAgent(id), "close the others");
        require(waitFor([&workspace] { return workspace.closingAgentIds().isEmpty(); }, 10000),
                "the others end");
    }
    qputenv("PATH", path);
}

// A window that quits (or restarts) mid-close leaves the agent under
// "closing": once its service is gone, the next window neither shows nor
// restarts it; a service still running is reattached out of sight and asked
// again. The other agents are untouched.
void relaunchMidCloseFinishesTheClose() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-close-XXXXXX"));
    require(directory.isValid(), "close directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const auto path = installStandInGrok(root);
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    options.restoreAgents = true;
    QString kept;
    QString closed;
    QString interrupted;
    {
        Workspace workspace(WorkspaceMode::live, options);
        for (auto* id : {&kept, &closed, &interrupted}) {
            require(workspace.createAgent(root.filePath(QStringLiteral("project")),
                                          QStringLiteral("agent"), QStringLiteral("grok")),
                    "a stand-in agent");
            *id = workspace.focusedAgent()->agentId();
        }
        for (const auto& value : workspace.agents()) {
            auto* item = value.value<lapis::desktop::Agent*>();
            require(waitFor([item] { return item->inputReady(); }, 10000), "it attaches");
        }
        // The window goes away within the same turn as the close.
        require(workspace.closeAgent(closed), "close one");
    }
    const auto answers = [&root](const QString& id) {
        QLocalSocket probe;
        probe.connectToServer(root.filePath(id + QStringLiteral(".sock")));
        const bool connected = probe.waitForConnected(250);
        probe.abort();
        return connected;
    };
    require(registryIds(options.storagePath, "closing") == QStringList{closed},
            "the quit window left it under closing");
    // Its service got the request before the window went: it ends.
    require(waitFor([&] { return !answers(closed); }, 10000),
            "the closed agent's service ends without its window");
    require(answers(interrupted) && answers(kept), "the other services still run");
    // The interrupted one: a window that died before asking its service.
    {
        auto document = registryRoot(options.storagePath);
        QJsonArray agents;
        QJsonArray closing = document.value(QStringLiteral("closing")).toArray();
        for (const auto& value : document.value(QStringLiteral("agents")).toArray())
            (value.toObject().value(QStringLiteral("id")).toString() == interrupted ? closing
                                                                                    : agents)
                .append(value);
        document.insert(QStringLiteral("agents"), agents);
        document.insert(QStringLiteral("closing"), closing);
        writeRegistry(options.storagePath, document);
    }
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(listedIds(workspace.agents()) == QStringList{kept},
                "neither closing agent is shown after the relaunch");
        require(workspace.closingAgentIds() == QStringList{interrupted},
                "the ended one is dropped, the running one is finished out of sight");
        require(waitFor([&workspace] { return workspace.closingAgentIds().isEmpty(); }, 10000),
                "the running one is asked again and ends");
        // A service reports `ended` before it drains and exits.
        require(waitFor([&] { return !answers(interrupted); }, 10000),
                "the interrupted agent's service exits");
        static_cast<void>(waitFor([] { return false; }, 1000));
        require(!answers(closed), "the ended agent was not started again");
        require(!answers(interrupted), "the interrupted agent was not started again");
        require(registryIds(options.storagePath, "agents") == QStringList{kept} &&
                    registryIds(options.storagePath, "closing").isEmpty(),
                "the registry keeps only the agent left");
        auto* item = workspace.agent(kept);
        require(item != nullptr && waitFor([item] { return item->inputReady(); }, 10000) &&
                    screenText(item->snapshot()).contains(QStringLiteral("grok ready")),
                "the other agent is the same one, still running");
        require(workspace.workspaceError().isEmpty(), "no error");
        require(workspace.closeAgent(kept) &&
                    waitFor([&workspace] { return workspace.closingAgentIds().isEmpty(); }, 10000),
                "close the last stand-in");
    }
    qputenv("PATH", path);
}

class ScopedHome {
  public:
    explicit ScopedHome(const QByteArray& value) : previous_(qgetenv("HOME")) {
        qputenv("HOME", value);
    }
    ~ScopedHome() { qputenv("HOME", previous_); }
    ScopedHome(const ScopedHome&) = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;

  private:
    QByteArray previous_;
};

void writeLines(const QString& path, const QList<QByteArray>& lines) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create a conversation folder");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "create a conversation file");
    for (const auto& line : lines)
        require(file.write(line + '\n') == line.size() + 1, "write a conversation line");
}

// The visual view's file for a local Codex or Antigravity agent: the
// conversation lapis knows by id, else, before anything names it, the one
// the CLI started in the agent's folder since the agent's service started,
// never one older or another agent's, and never a subagent's or an exec run.
void visualConversationFiles() {
    QTemporaryDir directory;
    require(directory.isValid(), "visual conversation directory");
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const ScopedCodexHome codex_home{QDir(root).filePath(QStringLiteral("codex")).toUtf8()};
    const ScopedHome home{QDir(root).filePath(QStringLiteral("home")).toUtf8()};
    const auto work = QDir(root).filePath(QStringLiteral("work"));
    require(QDir().mkpath(work), "agent folder");
    const auto endpoint = [&root](const QString& id) {
        return QDir(root).filePath(id + QStringLiteral(".sock"));
    };
    // The agents' services "started" now: their endpoints are made now.
    // Agent ids are UUIDs; each role names one.
    QHash<QString, QString> ids;
    QJsonArray agents;
    for (const auto* role : {"codex-named", "codex-fresh", "agy-fresh", "agy-named", "grok"}) {
        const auto id = uuid();
        ids.insert(QString::fromLatin1(role), id);
        QFile made(endpoint(id));
        require(made.open(QIODevice::WriteOnly), "make an endpoint");
        agents.append(
            QJsonObject{{"id", id},
                        {"title", QString::fromLatin1(role)},
                        {"category", "general"},
                        {"endpoint", endpoint(id)},
                        {"program", "/usr/bin/true"},
                        {"harness", QString::fromLatin1(role).section(QLatin1Char('-'), 0, 0)},
                        {"directory", work}});
    }
    const auto named_thread = uuid();
    writeObservedResume(endpoint(ids.value(QStringLiteral("codex-named"))),
                        {QStringLiteral("codex"), named_thread});
    const auto named_agy = uuid();
    lapis::session::write_resume_record(
        endpoint(ids.value(QStringLiteral("agy-named"))),
        {QStringLiteral("agy"), named_agy, lapis::session::ResumeSource::terminal});

    const auto now = QDateTime::currentDateTimeUtc();
    const auto stamp = [](const QDateTime& at) { return at.toString(Qt::ISODateWithMs).toUtf8(); };
    const auto sessions = QDir(root).filePath(QStringLiteral("codex/sessions"));
    const auto today = QDir(sessions).filePath(
        QDateTime::currentDateTime().date().toString(QStringLiteral("yyyy/MM/dd")));
    const auto rollout = [&](const QString& folder, const QString& thread, const QDateTime& at,
                             const char* source) {
        const auto path = QDir(folder).filePath(QStringLiteral("rollout-2026-11-10T10-00-00-") +
                                                thread + QStringLiteral(".jsonl"));
        writeLines(path, {R"({"type":"session_meta","payload":{"id":")" + thread.toUtf8() +
                          R"(","timestamp":")" + stamp(at) + R"(","cwd":")" + work.toUtf8() +
                          R"(","source":")" + source + R"("}})"});
        return path;
    };
    WorkspaceOptions options;
    options.storagePath = QDir(root).filePath(QStringLiteral("ws.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", agents}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "visual agents restored");
    const auto file = [&workspace, &ids](const char* role) {
        return workspace.conversationFile(ids.value(QString::fromLatin1(role)));
    };
    require(file("codex-fresh").isEmpty() && file("agy-fresh").isEmpty(),
            "nothing before either CLI saved a conversation");

    // Codex: the named thread, wherever its day folder is.
    const auto named_path =
        rollout(QDir(sessions).filePath(QStringLiteral("2026/11/09")), named_thread, now, "cli");
    require(file("codex-named") == named_path, "Codex: the thread notify named");
    // Before the first turn ends: the TUI thread started in the folder since
    // the service, not an older one, an exec run, or the named thread.
    (void)rollout(today, uuid(), now.addSecs(-3600), "cli");
    (void)rollout(today, uuid(), now.addSecs(2), "exec");
    require(file("codex-fresh").isEmpty(), "Codex: older and exec threads never count");
    const auto fresh_path = rollout(today, uuid(), now.addSecs(1), "cli");
    require(file("codex-fresh") == fresh_path, "Codex: the thread this agent started");

    // Antigravity: the named conversation, else the folder's latest one.
    const auto agy_home = QDir(root).filePath(QStringLiteral("home/.gemini/antigravity-cli"));
    const auto transcript = [&](const QString& conversation, const QDateTime& at) {
        const auto path = QDir(agy_home).filePath(
            QStringLiteral("brain/%1/.system_generated/logs/transcript_full.jsonl")
                .arg(conversation));
        writeLines(path, {R"({"step_index":0,"type":"USER_INPUT","created_at":")" +
                          at.toString(Qt::ISODate).toUtf8() + R"(","content":"x"})"});
        return path;
    };
    const auto cache = QDir(agy_home).filePath(QStringLiteral("cache/last_conversations.json"));
    const auto latest = [&](const QString& conversation) {
        writeLines(
            cache,
            {QJsonDocument(QJsonObject{{work, conversation}}).toJson(QJsonDocument::Compact)});
    };
    const auto named_agy_path = transcript(named_agy, now);
    require(file("agy-named") == named_agy_path, "agy: the conversation its hook named");
    const auto old = uuid();
    (void)transcript(old, now.addSecs(-3600));
    latest(old);
    require(file("agy-fresh").isEmpty(), "agy: a conversation from before the agent never counts");
    latest(named_agy);
    require(file("agy-fresh").isEmpty(), "agy: another agent's conversation never counts");
    const auto fresh_agy = uuid();
    const auto fresh_agy_path = transcript(fresh_agy, now.addSecs(1));
    latest(fresh_agy);
    require(file("agy-fresh") == fresh_agy_path, "agy: the conversation this agent started");

    require(file("grok").isEmpty(), "a CLI the view cannot read has no file");
    require(workspace.forkConversation(ids.value(QStringLiteral("codex-named")), uuid(), true)
                .isEmpty(),
            "only Claude Code conversations fork");
}

// Stand-ins for agents' session services: each runs a "CLI" child that
// holds open the file its control file names, as agy holds its presence
// lock and conversation, and the service's lock names its pid as the real
// one's does. Writing another path to the control file is a /clear.
class FakeServices {
  public:
    explicit FakeServices(const QString& root) : root_(root) {
        writeExecutable(root_.filePath(QStringLiteral("cli.sh")),
                        "#!/bin/sh\nwhile :; do exec 3<\"$(cat \"$1\")\"; sleep 0.1; done\n");
        writeExecutable(root_.filePath(QStringLiteral("service.sh")),
                        "#!/bin/sh\n\"$(dirname \"$0\")/cli.sh\" \"$1\"\n");
    }
    ~FakeServices() {
        for (const auto& id : processes_.keys())
            stop(id);
    }
    FakeServices(const FakeServices&) = delete;
    FakeServices& operator=(const FakeServices&) = delete;

    // Positional contract; callers and tests pin the order.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void start(const QString& id, const QString& endpoint) {
        hold(id, QStringLiteral("/dev/null"));
        auto process = std::make_unique<QProcess>();
        // Its own group, so stop() ends the "CLI" with it.
        process->setChildProcessModifier([] { ::setpgid(0, 0); });
        process->start(root_.filePath(QStringLiteral("service.sh")), {control(id)});
        require(process->waitForStarted(5000), "a stand-in service starts");
        QFile lock(endpoint + QStringLiteral(".lock"));
        require(lock.open(QIODevice::WriteOnly | QIODevice::Truncate), "write its lock");
        lock.write(QByteArray::number(process->processId()) + "\nlapis_session_service\n" +
                   QSysInfo::machineHostName().toUtf8() + '\n');
        processes_.insert(id, std::shared_ptr<QProcess>(std::move(process)));
    }
    // Positional contract; callers and tests pin the order.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void hold(const QString& id, const QString& path) {
        QSaveFile file(control(id));
        require(file.open(QIODevice::WriteOnly) && file.write(QFile::encodeName(path)) > 0 &&
                    file.commit(),
                "name the file the stand-in CLI holds");
    }
    void stop(const QString& id) {
        const auto process = processes_.take(id);
        if (!process)
            return;
        const qint64 pid = process->processId();
        if (pid > 0)
            ::kill(-static_cast<pid_t>(pid), SIGKILL);
        process->waitForFinished(5000);
    }

  private:
    [[nodiscard]] QString control(const QString& id) const {
        return root_.filePath(id + QStringLiteral(".holds"));
    }
    QDir root_;
    QHash<QString, std::shared_ptr<QProcess>> processes_;
};

// Two agy agents in one folder each show only the conversation their own agy
// is writing, whichever started first or wrote last, though agy's cache names
// one conversation per folder. The choice is pinned in the registry: it holds
// through a window restart, an outside agy run in the folder, and the
// processes ending, follows the agent's own /clear, and is what a restore
// resumes.
void agyAgentsKeepTheirOwnConversations() {
    using lapis::desktop::conversation_owner::held_conversation;
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-pins-XXXXXX"));
    require(directory.isValid(), "pin directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const ScopedHome home{root.filePath(QStringLiteral("home")).toUtf8()};
    const ScopedCodexHome codex_home{root.filePath(QStringLiteral("codex")).toUtf8()};
    const auto work = root.filePath(QStringLiteral("work"));
    require(QDir().mkpath(work), "agent folder");
    const QDir agy(root.filePath(QStringLiteral("home/.gemini/antigravity-cli")));
    require(QDir().mkpath(agy.filePath(QStringLiteral("presence"))), "agy presence folder");
    const auto now = QDateTime::currentDateTimeUtc();
    const auto transcript = [&agy](const QString& conversation) {
        return agy.filePath(QStringLiteral("brain/%1/.system_generated/logs/transcript_full.jsonl")
                                .arg(conversation));
    };
    // A conversation agy began at `at` (made-up steps), and its presence lock.
    const auto begin = [&](const QString& conversation, int seconds) {
        writeLines(transcript(conversation),
                   {R"({"step_index":0,"type":"USER_INPUT","created_at":")" +
                    now.addSecs(seconds).toString(Qt::ISODate).toUtf8() +
                    R"(","content":"made up"})"});
        const auto lock = agy.filePath(QStringLiteral("presence/%1.lock").arg(conversation));
        QFile made(lock);
        require(made.open(QIODevice::WriteOnly), "make a presence lock");
        return lock;
    };
    const auto last = [&](const QString& conversation) {
        writeLines(
            agy.filePath(QStringLiteral("cache/last_conversations.json")),
            {QJsonDocument(QJsonObject{{work, conversation}}).toJson(QJsonDocument::Compact)});
    };
    writeExecutable(root.filePath(QStringLiteral("agy")), "#!/bin/sh\nexit 0\n");

    // A starts, then B, in the same folder.
    FakeServices services(root.path());
    const auto first = uuid();
    const auto second = uuid();
    const auto endpoint = [&root](const QString& id) {
        return root.filePath(id + QStringLiteral(".sock"));
    };
    QJsonArray agents;
    for (const auto& id : {first, second}) {
        QFile made(endpoint(id));
        require(made.open(QIODevice::WriteOnly), "make an endpoint");
        services.start(id, endpoint(id));
        auto record = agentRecord(work, id, "general");
        record.insert(QStringLiteral("endpoint"), endpoint(id));
        record.insert(QStringLiteral("program"), root.filePath(QStringLiteral("agy")));
        record.insert(QStringLiteral("harness"), QStringLiteral("agy"));
        agents.append(record);
    }
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    writeRegistry(options.storagePath,
                  {{"version", 2},
                   {"activeCategory", "general"},
                   {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                   {"agents", agents}});
    const auto a = uuid();
    const auto b = uuid();
    services.hold(first, begin(a, 1));
    services.hold(second, begin(b, 2));
    last(b); // agy's cache now names B's conversation for the folder
    require(waitFor(
                [&] {
                    return held_conversation(QStringLiteral("agy"), endpoint(first), {}) == a &&
                           held_conversation(QStringLiteral("agy"), endpoint(second), {}) == b;
                },
                5000),
            "each agent's process holds its own conversation");
    const auto pinned = [&options](const QString& id) {
        for (const auto& value : QJsonDocument::fromJson(readRegistry(options.storagePath))
                                     .object()
                                     .value(QStringLiteral("agents"))
                                     .toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject().value(QStringLiteral("conversation")).toString();
        return QString();
    };
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.workspaceError().isEmpty(), "agy agents restored");
        require(workspace.conversationFile(second) == transcript(b), "B shows its conversation");
        require(workspace.conversationFile(first) == transcript(a),
                "A shows its own though agy's cache names B's");
        require(workspace.agentConversation(first) == a && workspace.agentConversation(second) == b,
                "each agent's conversation is the one its process holds");
    }
    require(pinned(first) == a && pinned(second) == b, "the registry keeps both pins");
    {
        // The window restarts; then agy runs outside lapis in the folder.
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.conversationFile(first) == transcript(a) &&
                    workspace.conversationFile(second) == transcript(b),
                "a restarted window shows each agent its own conversation");
        const auto outside = uuid();
        (void)begin(outside, 3);
        last(outside);
        require(workspace.conversationFile(first) == transcript(a) &&
                    workspace.conversationFile(second) == transcript(b),
                "an outside agy run in the folder takes neither");
        // /clear in A: its agy now writes a new conversation.
        const auto cleared = uuid();
        services.hold(first, begin(cleared, 4));
        last(cleared);
        require(
            waitFor([&] { return workspace.conversationFile(first) == transcript(cleared); }, 5000),
            "A follows its own new conversation");
        require(workspace.conversationFile(second) == transcript(b), "B keeps its own");
        require(pinned(first) == cleared, "the new conversation is pinned");
    }
    // With both processes gone only the pins say which is whose.
    services.stop(first);
    services.stop(second);
    const auto cleared = pinned(first);
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.conversationFile(first) == transcript(cleared) &&
                    workspace.conversationFile(second) == transcript(b),
                "the pins hold once the processes end");
    }
    // A full restore resumes each pinned conversation.
    for (const auto& id : {first, second}) {
        QFile::remove(endpoint(id));
        QFile::remove(endpoint(id) + QStringLiteral(".lock"));
    }
    options.restoreAgents = true;
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "agy agents restart");
    const auto arguments = [&options](const QString& id) {
        for (const auto& value : QJsonDocument::fromJson(readRegistry(options.storagePath))
                                     .object()
                                     .value(QStringLiteral("agents"))
                                     .toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                return value.toObject().value(QStringLiteral("arguments")).toArray();
        return QJsonArray();
    };
    require(arguments(first) == QJsonArray{QStringLiteral("--conversation"), cleared} &&
                arguments(second) == QJsonArray{QStringLiteral("--conversation"), b},
            "a restore resumes each agent's pinned conversation");
    for (const auto& id : {first, second})
        require(workspace.closeAgent(id, true), "close a restored agent");
    require(waitFor([&workspace] { return closedFully(workspace); }, 10000),
            "restored agents close");
}

// Two Codex agents in one folder before either's first turn ends: a thread
// either could have begun is shown for neither, until the agent's own
// process is seen holding it.
void codexAgentsNeverTakeEachOthersThread() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-pins-XXXXXX"));
    require(directory.isValid(), "Codex pin directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    const ScopedHome home{root.filePath(QStringLiteral("home")).toUtf8()};
    const ScopedCodexHome codex_home{root.filePath(QStringLiteral("codex")).toUtf8()};
    const auto work = root.filePath(QStringLiteral("work"));
    require(QDir().mkpath(work), "Codex agent folder");
    const auto today =
        root.filePath(QStringLiteral("codex/sessions/") +
                      QDateTime::currentDateTime().date().toString(QStringLiteral("yyyy/MM/dd")));
    const auto now = QDateTime::currentDateTimeUtc();
    const auto rollout = [&](const QString& thread, int seconds) {
        const auto path = QDir(today).filePath(QStringLiteral("rollout-2026-11-10T10-00-00-") +
                                               thread + QStringLiteral(".jsonl"));
        writeLines(path, {R"({"type":"session_meta","payload":{"id":")" + thread.toUtf8() +
                          R"(","timestamp":")" +
                          now.addSecs(seconds).toString(Qt::ISODateWithMs).toUtf8() +
                          R"(","cwd":")" + work.toUtf8() + R"(","source":"cli"}})"});
        return path;
    };
    FakeServices services(root.path());
    const auto first = uuid();
    const auto second = uuid();
    QJsonArray agents;
    for (const auto& id : {first, second}) {
        const auto endpoint = root.filePath(id + QStringLiteral(".sock"));
        QFile made(endpoint);
        require(made.open(QIODevice::WriteOnly), "make a Codex endpoint");
        services.start(id, endpoint);
        auto record = agentRecord(work, id, "general");
        record.insert(QStringLiteral("endpoint"), endpoint);
        record.insert(QStringLiteral("harness"), QStringLiteral("codex"));
        agents.append(record);
    }
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    writeRegistry(options.storagePath,
                  {{"version", 2},
                   {"activeCategory", "general"},
                   {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                   {"agents", agents}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "Codex agents restored");
    // The second agent's thread starts first; either agent could own it.
    const auto theirs = rollout(uuid(), 1);
    require(workspace.conversationFile(first).isEmpty() &&
                workspace.conversationFile(second).isEmpty(),
            "a thread either agent could have begun is neither's");
    services.hold(second, theirs);
    require(waitFor([&] { return workspace.conversationFile(second) == theirs; }, 5000),
            "the thread its own process writes is the second agent's");
    require(workspace.conversationFile(first).isEmpty(), "and never the first agent's");
    const auto mine = rollout(uuid(), 2);
    require(workspace.conversationFile(first) == mine,
            "with the other's thread known, the first agent's is its own");
}

// A local Claude Code agent's saved transcript: looked for first in the
// project folder Claude Code names after the agent's directory (each
// character other than an ASCII letter or digit made '-'), else in any
// project folder; the path found is kept while the file exists; and one
// not saved yet is searched for everywhere at most every 30 seconds, while
// the named folder is still looked in at once.
void claudeTranscriptsAreFoundAndKept() {
    QTemporaryDir directory;
    require(directory.isValid(), "Claude transcript directory");
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const ScopedClaudeHome claude_home{QDir(root).filePath(QStringLiteral("claude")).toUtf8()};
    const ScopedHome home{QDir(root).filePath(QStringLiteral("home")).toUtf8()};
    const auto work = QDir(root).filePath(QStringLiteral("my_project.v2"));
    require(QDir().mkpath(work), "agent folder");
    QString folder = work;
    for (auto& c : folder)
        if (!(c.isLetterOrNumber() && c.unicode() < 128))
            c = QLatin1Char('-');
    require(folder.endsWith(QStringLiteral("-my-project-v2")), "the folder name maps every mark");
    const auto projects = QDir(root).filePath(QStringLiteral("claude/projects"));
    const auto saved = [&projects](const QString& project, const QString& conversation) {
        const auto path = QDir(projects).filePath(project + QLatin1Char('/') + conversation +
                                                  QStringLiteral(".jsonl"));
        writeLines(path, {R"({"type":"user","uuid":"u1","message":{"content":"hi"}})"});
        return path;
    };
    QHash<QString, QString> ids, conversations;
    QJsonArray agents;
    for (const auto* role : {"named", "elsewhere", "unsaved"}) {
        const auto id = uuid();
        const auto conversation = uuid();
        ids.insert(QString::fromLatin1(role), id);
        conversations.insert(QString::fromLatin1(role), conversation);
        auto record = agentRecord(root, id, "general");
        record.insert(QStringLiteral("harness"), QStringLiteral("claude"));
        record.insert(QStringLiteral("directory"), work);
        record.insert(QStringLiteral("conversation"), conversation);
        agents.append(record);
    }
    // The same conversation in both folders: the named one wins.
    const auto named = saved(folder, conversations.value(QStringLiteral("named")));
    (void)saved(QStringLiteral("-other-project"), conversations.value(QStringLiteral("named")));
    const auto elsewhere =
        saved(QStringLiteral("-moved-project"), conversations.value(QStringLiteral("elsewhere")));
    WorkspaceOptions options;
    options.storagePath = QDir(root).filePath(QStringLiteral("ws.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", agents}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "Claude agents restored");
    const auto file = [&workspace, &ids](const char* role) {
        return workspace.conversationFile(ids.value(QString::fromLatin1(role)));
    };
    require(file("named") == named, "the folder named after the agent's directory first");
    require(file("elsewhere") == elsewhere, "else any project folder");
    // Kept while it exists: a copy in the named folder made later is not
    // looked for, and the kept path is checked to exist.
    const auto moved = saved(folder, conversations.value(QStringLiteral("elsewhere")));
    require(file("elsewhere") == elsewhere, "the path found is kept");
    require(QFile::remove(elsewhere), "remove the kept transcript");
    require(file("elsewhere") == moved, "a kept path whose file is gone is looked for again");

    // Not saved yet: empty, and the search of every folder is not repeated
    // within 30 seconds; the named folder is.
    require(file("unsaved").isEmpty(), "nothing before Claude Code saved the conversation");
    const auto other =
        saved(QStringLiteral("-another-project"), conversations.value(QStringLiteral("unsaved")));
    require(file("unsaved").isEmpty(),
            "every project folder is searched again only after 30 seconds");
    const auto own = saved(folder, conversations.value(QStringLiteral("unsaved")));
    require(file("unsaved") == own && own != other,
            "the named folder is looked in each time it is asked");
}

// The visual view's read-ahead (setVisualKeep, warmThreads): the budget
// reaches the thread model, the threads of this Mac's agents whose
// conversations lapis already knows are read most recently written first
// while the budget has room, and an agent whose conversation is not known
// yet is not placed by it (its pin waits for the pin timer).
void visualThreadsAreReadAhead() {
    QTemporaryDir directory;
    require(directory.isValid(), "read-ahead directory");
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const ScopedClaudeHome claude_home{QDir(root).filePath(QStringLiteral("claude")).toUtf8()};
    const ScopedCodexHome codex_home{QDir(root).filePath(QStringLiteral("codex")).toUtf8()};
    const ScopedHome home{QDir(root).filePath(QStringLiteral("home")).toUtf8()};
    const auto work = QDir(root).filePath(QStringLiteral("work"));
    require(QDir().mkpath(work), "agent folder");
    QString folder = work;
    for (auto& c : folder)
        if (!(c.isLetterOrNumber() && c.unicode() < 128))
            c = QLatin1Char('-');
    const auto projects = QDir(root).filePath(QStringLiteral("claude/projects"));
    const auto endpoint = [&root](const QString& id) {
        return QDir(root).filePath(id + QStringLiteral(".sock"));
    };
    QHash<QString, QString> ids, paths;
    QJsonArray agents;
    const auto now = QDateTime::currentDateTime();
    // Claude Code agents: "newer" written last, "older" before it, and
    // "unsaved" with no transcript yet.
    for (const auto* role : {"older", "newer", "unsaved"}) {
        const auto id = uuid();
        const auto conversation = uuid();
        ids.insert(QString::fromLatin1(role), id);
        auto record = agentRecord(root, id, "general");
        record.insert(QStringLiteral("harness"), QStringLiteral("claude"));
        record.insert(QStringLiteral("directory"), work);
        record.insert(QStringLiteral("conversation"), conversation);
        agents.append(record);
        if (QLatin1String(role) == QLatin1String("unsaved"))
            continue;
        const auto path = QDir(projects).filePath(folder + QLatin1Char('/') + conversation +
                                                  QStringLiteral(".jsonl"));
        writeLines(path, {R"({"type":"user","uuid":"u1","message":{"content":"hi"}})",
                          R"({"type":"assistant","uuid":"a1","message":{"content":[)"
                          R"({"type":"text","text":"hello"}]}})"});
        QFile touched(path);
        require(touched.open(QIODevice::ReadWrite) &&
                    touched.setFileTime(QLatin1String(role) == QLatin1String("newer")
                                            ? now.addSecs(-10)
                                            : now.addSecs(-3600),
                                        QFileDevice::FileModificationTime),
                "date a transcript");
        paths.insert(QString::fromLatin1(role), path);
    }
    // A Codex agent whose thread nothing names yet: conversationFile would
    // place it (the thread it started in its folder since its service
    // started); reading ahead must not.
    const auto codex = uuid();
    {
        QFile made(endpoint(codex));
        require(made.open(QIODevice::WriteOnly), "make an endpoint");
        auto record = agentRecord(root, codex, "general");
        record.insert(QStringLiteral("harness"), QStringLiteral("codex"));
        record.insert(QStringLiteral("directory"), work);
        agents.append(record);
    }
    const auto thread = uuid();
    const auto rollout =
        QDir(QDir(root).filePath(QStringLiteral("codex/sessions")))
            .filePath(QDateTime::currentDateTime().date().toString(QStringLiteral("yyyy/MM/dd")) +
                      QStringLiteral("/rollout-2026-11-10T10-00-00-") + thread +
                      QStringLiteral(".jsonl"));
    writeLines(rollout,
               {R"({"type":"session_meta","payload":{"id":")" + thread.toUtf8() +
                R"(","timestamp":")" +
                QDateTime::currentDateTimeUtc().addSecs(1).toString(Qt::ISODateWithMs).toUtf8() +
                R"(","cwd":")" + work.toUtf8() + R"(","source":"cli"}})"});
    WorkspaceOptions options;
    options.storagePath = QDir(root).filePath(QStringLiteral("ws.json"));
    writeRegistry(
        options.storagePath,
        QJsonObject{{"version", 2},
                    {"activeCategory", "general"},
                    {"categories", QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}}}},
                    {"agents", agents}});
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.workspaceError().isEmpty(), "read-ahead agents restored");
    auto* threads = qobject_cast<lapis::desktop::TranscriptModel*>(workspace.transcript());
    require(threads != nullptr, "the workspace owns a thread model");
    const auto settled = [threads] {
        return waitFor([threads] { return !threads->readingAhead(); }, 10000);
    };

    // No memory budget: nothing is read ahead.
    workspace.setVisualKeep(0, 32, false);
    workspace.warmThreads();
    require(settled() && threads->keptCount() == 0, "a zero budget reads nothing ahead");
    // Room for one: the most recently written.
    workspace.setVisualKeep(1024, 1, false);
    workspace.warmThreads();
    require(waitFor([&] { return threads->kept(paths.value(QStringLiteral("newer"))); }, 10000) &&
                settled() && threads->keptCount() == 1 &&
                !threads->kept(paths.value(QStringLiteral("older"))),
            "the most recently written thread is read first");
    // Room for all: the rest of the known ones, never the unplaced Codex
    // thread.
    workspace.setVisualKeep(1024, 32, false);
    workspace.warmThreads();
    require(waitFor([&] { return threads->kept(paths.value(QStringLiteral("older"))); }, 10000) &&
                settled() && threads->keptCount() == 2 && !threads->kept(rollout),
            "every known thread is read ahead, and an unplaced one is not");
    require(workspace.conversationFile(codex) == rollout,
            "the Codex thread is placed when the view asks for it");
    // Scheduled once a run, a few seconds after it is asked for.
    workspace.setVisualKeep(0, 32, false);
    require(threads->keptCount() == 0, "a smaller budget lets the kept threads go");
    workspace.setVisualKeep(1024, 32, true);
    require(threads->keptCount() == 0, "the scheduled read-ahead waits");
    require(waitFor([&] { return threads->keptCount() == 3; }, 10000) && settled() &&
                threads->kept(rollout),
            "then reads every thread known by then");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    try {
        if (argc > 1) {
            require(argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--case") &&
                        (QString::fromLocal8Bit(argv[2]) == QStringLiteral("adapter-status") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("remote-options") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("remote-hooks") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("local-codex") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("retired-plans") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("startup-defaults") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("launch-policy") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("reload") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("updater") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("updates-off") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("omp-overlay") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("chimes") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("attention") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("sidebar") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("stage-size") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("visual-files") ||
                         QString::fromLocal8Bit(argv[2]) == QStringLiteral("closing")),
                    "Usage: lapis_workspace_tests [--case "
                    "adapter-status|remote-options|remote-hooks|local-codex|retired-plans|reload|"
                    "updater|"
                    "updates-off|startup-defaults|launch-policy|omp-overlay|chimes|attention|"
                    "sidebar|"
                    "stage-size|visual-files|closing]");
            const auto selected = QString::fromLocal8Bit(argv[2]);
            if (selected == QStringLiteral("adapter-status")) {
                claudeAgentsUseServiceAdapter();
            } else if (selected == QStringLiteral("retired-plans")) {
                retiredPlansLeaveSavedAgentsStartable();
            } else if (selected == QStringLiteral("remote-options")) {
                remoteOptionsRespectTheArgumentLimit();
                remoteClaudeReconnectsToItsConversation();
            } else if (selected == QStringLiteral("remote-hooks")) {
                remoteAgentsReportTurnsThroughTheirTerminal();
            } else if (selected == QStringLiteral("local-codex")) {
                localCodexReportsTurnsThroughNotify();
            } else if (selected == QStringLiteral("startup-defaults")) {
                savedGrokDefaultsPreserveLaunchOwnership();
                savedCodexDefaultsKeepTheScreenMode();
            } else if (selected == QStringLiteral("stage-size")) {
                // Windowless: a stand-in CLI under a real session service.
                agentsStartAtTheStageSize();
                restoredAgentsStartAtTheStageSize();
            } else if (selected == QStringLiteral("launch-policy")) {
                modelessAgentsGetTheDefaultMode();
                resumingAConversationStartsItsCli();
                askedEffortReachesTheCli();
            } else if (selected == QStringLiteral("updater")) {
                updaterLifecycle();
                updaterOutputIsDrainedWithABoundedTail();
                failedUpdaterStartClearsTheQueue();
                pinnedCliIsNotUpdated();
                startupUpdatesShareOneInstaller();
            } else if (selected == QStringLiteral("updates-off")) {
                pinnedCliIsNotUpdated();
            } else if (selected == QStringLiteral("visual-files")) {
                visualConversationFiles();
                agyAgentsKeepTheirOwnConversations();
                codexAgentsNeverTakeEachOthersThread();
                claudeTranscriptsAreFoundAndKept();
                visualThreadsAreReadAhead();
            } else if (selected == QStringLiteral("omp-overlay")) {
                ompGetsItsResizeOverlay();
            } else if (selected == QStringLiteral("chimes")) {
                alertsChimeWhileAnAgentWaits();
                chimesPlayChosenFiles();
                attentionLogStaysPrivateAndRotates();
            } else if (selected == QStringLiteral("attention")) {
                truthfulStatus();
                categoryCountsChangeOnlyWhenNeeded();
                tabGoesToTheReadyThenTheOldest();
                unseenFollowsTurnsAndSelection();
                attentionMarksFollowLooking();
                tabSkipsTurnsAlreadySeen();
                attentionMarksSurviveARestart();
                attentionReplayMarksEachTurnOnce();
            } else if (selected == QStringLiteral("closing")) {
                closeEndsTheAgent("exec sleep 600", 0);
                closeOnHistoryPageEndsTheAgent();
                closingIsInstant();
                failedCloseReturnsTheAgent();
                relaunchMidCloseFinishesTheClose();
            } else if (selected == QStringLiteral("sidebar")) {
                nextAgentWalksTheSidebar();
                categoryColorsPersist();
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
        submittedAttentionKeepsItsDisplayToken();
        categoryCountsChangeOnlyWhenNeeded();
        nextAgentWalksTheSidebar();
        categoryColorsPersist();
        persistence();
        restoreAgentIdentity();
        visualConversationFiles();
        agyAgentsKeepTheirOwnConversations();
        codexAgentsNeverTakeEachOthersThread();
        claudeTranscriptsAreFoundAndKept();
        visualThreadsAreReadAhead();
        truthfulStatus();
        placeholderMatchesTerminalDefaults();
        discardOutsideActiveCategorySelectsNeighbor();
        failedWritesPreserveState();
        malformedAgentRegistry();
        unknownRegistryVersionsAreRejected();
        unseenFollowsTurnsAndSelection();
        attentionMarksFollowLooking();
        tabSkipsTurnsAlreadySeen();
        attentionMarksSurviveARestart();
        attentionReplayMarksEachTurnOnce();
        tabGoesToTheReadyThenTheOldest();
        tabAttributionFollowsTheNextHandMove();
        oneBadAgentDoesNotEmptyTheWorkspace();
        firstTabChoiceExcludesTheInitialFocus();
        closingRemovesAPendingChoiceCandidate();
        closingFocusedAgentCancelsPendingChoice();
        failedTabSelectionRecordsNothing();
        modelessAgentsGetTheDefaultMode();
        claudeAgentsUseServiceAdapter();
        agentArgumentsPersist();
        savedTilesLoadAsTheSelectedAgent();
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
        pinnedCliIsNotUpdated();
        savedArgumentCapKeepsRegistryLoadable();
        managedResumeFollowsRecovery();
        printedCheckpointsResumeButNeverOverrideTheObserver();
        restartReportsValidationFailures();
        const bool had_codex_home = qEnvironmentVariableIsSet("CODEX_HOME");
        const auto original_codex_home = qgetenv("CODEX_HOME");
        codexResumeArguments();
        savedGrokDefaultsPreserveLaunchOwnership();
        require(qEnvironmentVariableIsSet("CODEX_HOME") == had_codex_home &&
                    qgetenv("CODEX_HOME") == original_codex_home,
                "Codex fixtures restore the caller's exact environment");
        closeEndsTheAgent("exec sleep 600", 0);
        // An agent that ignores SIGHUP is ended by the SIGTERM escalation.
        closeEndsTheAgent("trap '' HUP; exec sleep 600", 1200);
        closeOnHistoryPageEndsTheAgent();
        closingIsInstant();
        failedCloseReturnsTheAgent();
        relaunchMidCloseFinishesTheClose();
        joinedViewStaysInSync();
        explicitLaunchesUseUpdaterPolicy();
        harnessesUpdateBeforeNewAgents();
        windowWaitsForTheRestoreHelper();
        phoneStartsAnAgentInItsCategory();
        remoteOptionsRespectTheArgumentLimit();
        remoteClaudeReconnectsToItsConversation();
        staleDescriptorReattachesTheRunningService();
        remoteAgentsReportTurnsThroughTheirTerminal();
        reloadStartsAgentsAgain();
        agentsStartAtTheStageSize();
        restoredAgentsStartAtTheStageSize();
        reloadFailuresRemainRetryable();
        batchReloadRetainsEarlierFailures();
        startupUpdatesShareOneInstaller();
        resumingAConversationStartsItsCli();
        askedEffortReachesTheCli();
        terminalsRunPlainShells();
        wheelReachesAFullScreenProgram();
        ompGetsItsResizeOverlay();
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
