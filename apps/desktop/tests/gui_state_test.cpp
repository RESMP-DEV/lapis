// What a restarted window brings back (GuiState): the file itself (owner-only,
// versioned, atomic, bounded, written publish-to-publish), and each owner's
// restore: unseen marks, a turn that ended while no window watched, what was
// seen of each agent, and closed agents.
#include "agent_checkpoint.hpp"
#include "alerts.hpp"
#include "gui_state.hpp"
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

#include <functional>
#include <iostream>
#include <stdexcept>

using lapis::desktop::GuiState;
using lapis::desktop::SeenScreens;
using lapis::desktop::SessionPreview;
using lapis::desktop::Workspace;
using lapis::desktop::WorkspaceMode;
using lapis::desktop::WorkspaceOptions;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()>& done, int ms = 5000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}
QJsonObject readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read the state file");
    return QJsonDocument::fromJson(file.readAll()).object();
}
void writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write a fixture file");
    require(file.write(bytes) == bytes.size(), "write the fixture bytes");
}
// A turn the observer saw start and end.
void finishTurn(SessionPreview& item) {
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    state.activity = lapis::session::attention::Activity::working;
    item.applyAttention(state);
    state.activity = lapis::session::attention::Activity::turn_completed;
    item.applyAttention(state);
}
void setActivity(SessionPreview& item, lapis::session::attention::Activity activity) {
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    state.activity = activity;
    item.applyAttention(state);
}

void savesAndRestoresSections() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const auto path = QDir(directory.path()).filePath(QStringLiteral("runtime/gui_state.json"));
    QJsonObject first{{QStringLiteral("x"), 1}};
    {
        GuiState state(path);
        require(!state.load(), "no file yet");
        require(state.section(QStringLiteral("first")).isUndefined(), "nothing loaded");
        state.addSection(QStringLiteral("first"), [&first] { return first; });
        state.setValue(QStringLiteral("lastHarness"), QStringLiteral("claude"));
        state.setValue(QStringLiteral("lastModels"),
                       QVariantMap{{QStringLiteral("claude"), QStringLiteral("opus")}});
        state.setValue(QString(65, QLatin1Char('k')), 1);
        state.setValue(QStringLiteral("big"), QString(5000, QLatin1Char('v')));
        first.insert(QStringLiteral("x"), 2);
    } // the destructor writes what changed
    const auto info = QFileInfo(path);
    require(info.exists(), "written at exit");
    require(info.permissions() ==
                (QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser),
            "owner-only");
    const auto root = readFile(path);
    require(root.value(QStringLiteral("version")).toInt() == GuiState::kVersion, "versioned");
    require(QDir(info.absolutePath()).entryList(QDir::Files).size() == 1,
            "written atomically, no temporary left");
    GuiState again(path);
    require(again.load(), "loads its own file");
    require(again.section(QStringLiteral("first")).toObject().value(QStringLiteral("x")).toInt() ==
                2,
            "the newest state of each section");
    require(again.value(QStringLiteral("lastHarness")).toString() == QLatin1String("claude") &&
                again.value(QStringLiteral("lastModels")).toMap().value(QStringLiteral("claude")) ==
                    QStringLiteral("opus"),
            "the window's choices");
    require(!again.value(QString(65, QLatin1Char('k'))).isValid() &&
                !again.value(QStringLiteral("big")).isValid(),
            "overlong keys and values are not kept");
}

void corruptOrOtherFilesAreIgnored() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const auto path = QDir(directory.path()).filePath(QStringLiteral("gui_state.json"));
    for (const QByteArray& bytes : {QByteArray("{not json"), QByteArray("[]"),
                                    QByteArray(R"({"version": 99, "sections": {"a": {"x": 1}}})"),
                                    QByteArray(R"({"sections": {"a": {"x": 1}}})")}) {
        writeFile(path, bytes);
        GuiState state(path);
        require(!state.load() && state.section(QStringLiteral("a")).isUndefined(),
                "a corrupt or other-version file is ignored");
    }
    writeFile(path, QByteArray(GuiState::kMaxBytes + 10, ' '));
    {
        GuiState state(path);
        require(!state.load(), "an oversized file is ignored");
    }
    const auto target = QDir(directory.path()).filePath(QStringLiteral("elsewhere.json"));
    writeFile(target, R"({"version": 1, "sections": {"a": {"x": 1}}})");
    QFile::remove(path);
    require(QFile::link(target, path), "make a symlink");
    GuiState state(path);
    require(!state.load(), "a symlink is not followed");
}

void writesAreCoalesced() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const auto path = QDir(directory.path()).filePath(QStringLiteral("gui_state.json"));
    GuiState state(path);
    state.setIntervalForTesting(300);
    int value = 0;
    state.addSection(QStringLiteral("n"), [&value] { return QJsonValue(value); });
    const auto saved = [&path] {
        return readFile(path)
            .value(QStringLiteral("sections"))
            .toObject()
            .value(QStringLiteral("n"))
            .toInt(-1);
    };
    QElapsedTimer clock;
    clock.start();
    value = 1;
    state.touch();
    require(waitFor([&] { return state.writesForTesting() == 1; }, 2000) && clock.elapsed() < 150,
            "the first change after a quiet spell is written at once");
    require(saved() == 1, "with its state");
    for (value = 2; value <= 20; ++value) {
        state.touch();
        QCoreApplication::processEvents();
    }
    value = 20;
    QThread::msleep(100);
    QCoreApplication::processEvents();
    require(state.writesForTesting() == 1, "a burst waits for the interval");
    require(waitFor([&] { return state.writesForTesting() == 2; }, 2000), "then one write");
    state.waitForTesting();
    require(saved() == 20, "of the newest state");
    QThread::msleep(400);
    QCoreApplication::processEvents();
    require(state.writesForTesting() == 2, "nothing more without a change");
}

// A preview workspace stands in for a window's: three agents, renderer shown.
void marksComeBack() {
    QJsonObject marks;
    qint64 needed = 0;
    {
        Workspace before(WorkspaceMode::preview);
        require(before.selectSession(QStringLiteral("renderer")), "select renderer");
        auto* agent = before.session(QStringLiteral("agent"));
        finishTurn(*agent);
        require(agent->unseen(), "a turn finished out of view is marked");
        needed = agent->neededAtMs();
        setActivity(*before.session(QStringLiteral("checks")),
                    lapis::session::attention::Activity::working);
        marks = before.saveMarks();
    }
    auto agents = marks.value(QStringLiteral("agents")).toObject();
    require(
        agents.value(QStringLiteral("agent")).toObject().value(QStringLiteral("unseen")).toBool(),
        "the unseen mark is saved");
    require(
        agents.value(QStringLiteral("checks")).toObject().value(QStringLiteral("working")).toBool(),
        "and an agent at work");
    // A mark for an agent that no longer exists, and one for the agent shown.
    agents.insert(QStringLiteral("gone"),
                  QJsonObject{{QStringLiteral("unseen"), true}, {QStringLiteral("neededAtMs"), 5}});
    agents.insert(QStringLiteral("renderer"),
                  QJsonObject{{QStringLiteral("unseen"), true}, {QStringLiteral("neededAtMs"), 5}});
    marks.insert(QStringLiteral("agents"), agents);

    Workspace after(WorkspaceMode::preview);
    require(after.selectSession(QStringLiteral("renderer")), "select renderer");
    int pings = 0;
    int away = 0;
    QObject::connect(&after, &Workspace::turnFinished, [&pings] { ++pings; });
    QObject::connect(&after, &Workspace::finishedWhileAway, [&away] { ++away; });
    after.restoreMarks(marks);
    auto* agent = after.session(QStringLiteral("agent"));
    require(agent->unseen() && agent->neededAtMs() == needed,
            "the mark comes back with when the agent began to need you");
    require(!after.session(QStringLiteral("renderer"))->unseen(),
            "the agent shown is never marked");
    require(after.session(QStringLiteral("gone")) == nullptr && pings == 0 && away == 0,
            "an agent that is gone is skipped, and restoring pings nothing");
    require(after.latestAttention() && after.focusedSession() == agent,
            "the restored mark draws Command-L");
    // The agent at work when the window saved is found finished.
    require(after.selectSession(QStringLiteral("renderer")), "back to renderer");
    auto* checks = after.session(QStringLiteral("checks"));
    setActivity(*checks, lapis::session::attention::Activity::turn_completed);
    require(away == 1 && pings == 0 && checks->unseen(),
            "a turn that ended while no window watched is marked and guessed for, unpinged");
    setActivity(*checks, lapis::session::attention::Activity::working);
    setActivity(*checks, lapis::session::attention::Activity::turn_completed);
    require(away == 1 && pings == 1, "later turns ping as usual");

    // Saved long ago: an idle agent is just idle.
    marks.insert(QStringLiteral("savedAtMs"),
                 QDateTime::currentMSecsSinceEpoch() - qint64{24} * 3600 * 1000);
    Workspace later(WorkspaceMode::preview);
    require(later.selectSession(QStringLiteral("renderer")), "select renderer");
    int later_away = 0;
    QObject::connect(&later, &Workspace::finishedWhileAway, [&later_away] { ++later_away; });
    later.restoreMarks(marks);
    setActivity(*later.session(QStringLiteral("checks")),
                lapis::session::attention::Activity::idle);
    require(later_away == 0 && !later.session(QStringLiteral("checks"))->unseen(),
            "after a long absence nothing is inferred");
    require(later.session(QStringLiteral("agent"))->unseen(), "though unseen marks remain");
}

// In a live workspace marks belong to a conversation: after /clear (a new
// conversation) the old marks are dropped.
void marksFollowTheConversation() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const auto canonical = QFileInfo(directory.path()).canonicalFilePath();
    const QString shown = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString kept = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString cleared = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonArray agents;
    for (const auto& id : {shown, kept, cleared})
        agents.append(
            QJsonObject{{"id", id},
                        {"title", id.left(8)},
                        {"category", "general"},
                        {"endpoint", QDir(canonical).filePath(id + QStringLiteral(".sock"))},
                        {"program", "/usr/bin/true"},
                        {"harness", "codex"},
                        {"directory", canonical}});
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    writeFile(
        options.storagePath,
        QJsonDocument(QJsonObject{{"version", 2},
                                  {"activeCategory", "general"},
                                  {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                        {"name", "General"},
                                                                        {"selected", shown}}}},
                                  {"agents", agents}})
            .toJson());
    const auto converse = [&canonical](const QString& id, const QString& conversation) {
        lapis::session::write_resume_record(
            QDir(canonical).filePath(id + QStringLiteral(".sock")),
            {QStringLiteral("codex"), conversation, lapis::session::ResumeSource::observer});
    };
    const auto first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    converse(kept, first);
    converse(cleared, first);
    QJsonObject marks;
    {
        Workspace before(WorkspaceMode::live, options);
        require(before.workspaceError().isEmpty(), "load the registry");
        require(before.focusedSession()->sessionId() == shown, "the first agent is shown");
        before.session(kept)->restoreUnseen(true, 100);
        before.session(cleared)->restoreUnseen(true, 100);
        marks = before.saveMarks();
    }
    require(marks.value(QStringLiteral("agents"))
                    .toObject()
                    .value(kept)
                    .toObject()
                    .value(QStringLiteral("conversation"))
                    .toString() == first,
            "a mark records its conversation");
    converse(cleared, QUuid::createUuid().toString(QUuid::WithoutBraces));
    Workspace after(WorkspaceMode::live, options);
    require(after.workspaceError().isEmpty(), "reload the registry");
    after.restoreMarks(marks);
    require(after.session(kept)->unseen(), "same conversation: the mark comes back");
    require(!after.session(cleared)->unseen() && after.session(cleared)->neededAtMs() == 0,
            "another conversation: nothing comes back");

    // Closed agents come back for Command-Shift-T, valid ones only.
    const QJsonArray closed{
        QJsonObject{{"category", "general"},
                    {"title", "closed"},
                    {"harness", "codex"},
                    {"program", "/usr/bin/true"},
                    {"arguments", QJsonArray{"resume", first}},
                    {"directory", canonical},
                    {"mode", "codex"},
                    {"managedResume", QJsonObject{{"index", 0}, {"identity", first}}}},
        QJsonObject{{"title", "relative"},
                    {"harness", "codex"},
                    {"program", "true"},
                    {"directory", canonical}},
        QJsonObject{{"title", "unknown"},
                    {"harness", "no-such-cli"},
                    {"program", "/usr/bin/true"},
                    {"directory", canonical}},
        QJsonObject{{"title", "arguments"},
                    {"harness", "codex"},
                    {"program", "/usr/bin/true"},
                    {"arguments", "not a list"},
                    {"directory", canonical}}};
    require(!after.canReopenAgent(), "nothing closed yet");
    after.restoreClosed(closed);
    require(after.canReopenAgent(), "a closed agent can be reopened after a restart");
    const auto saved = after.saveClosed();
    require(saved.size() == 1 &&
                saved.first().toObject().value(QStringLiteral("title")) ==
                    QLatin1String("closed") &&
                saved.first()
                        .toObject()
                        .value(QStringLiteral("managedResume"))
                        .toObject()
                        .value(QStringLiteral("index"))
                        .toInt() == 0,
            "only the valid entry, with its resume plan");
}

void seenScreensComeBack() {
    QJsonObject saved;
    {
        Workspace before(WorkspaceMode::preview);
        SeenScreens seen(before, [](const SessionPreview*) { return false; });
        int changes = 0;
        QObject::connect(&seen, &SeenScreens::changed, [&changes] { ++changes; });
        seen.see(before.session(QStringLiteral("agent")));
        seen.see(before.session(QStringLiteral("agent")));
        require(changes == 1, "seeing the same screen again changes nothing");
        saved = seen.saveState();
    }
    saved.insert(QStringLiteral("gone"), QStringLiteral("abc"));
    Workspace after(WorkspaceMode::preview);
    SeenScreens seen(after, [](const SessionPreview*) { return false; });
    seen.restoreState(saved);
    require(seen.unchanged(after.session(QStringLiteral("agent"))),
            "what was seen before the restart still counts as seen");
    require(!seen.unchanged(after.session(QStringLiteral("checks"))),
            "an agent never seen stays unseen");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    try {
        savesAndRestoresSections();
        corruptOrOtherFilesAreIgnored();
        writesAreCoalesced();
        marksComeBack();
        marksFollowTheConversation();
        seenScreensComeBack();
    } catch (const std::exception& error) {
        std::cerr << "gui_state_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gui_state_test: PASS\n";
    return 0;
}
