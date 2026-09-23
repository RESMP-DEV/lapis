#include "workspace.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <memory>
#include <signal.h>
#include <source_location>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
using namespace lapis::desktop;
using namespace lapis::session;
void require(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("Workspace check failed at " + std::to_string(where.line()));
}
template <typename F>
void until(F predicate, const char* message = "condition",
           std::source_location where = std::source_location::current()) {
    bool timeout_configured = false;
    const int configured_seconds =
        qEnvironmentVariable("LAPIS_WORKSPACE_TEST_TIMEOUT_S").toInt(&timeout_configured);
    const auto timeout =
        std::chrono::seconds{timeout_configured && configured_seconds > 0 ? configured_seconds : 8};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Workspace check " + std::string{message} +
                                     " timed out at line " + std::to_string(where.line()));
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
QString screen(const SessionPreview& document) {
    const auto& snapshot = document.snapshot();
    QString text;
    for (std::size_t row = 0; row < snapshot.size.rows; ++row) {
        for (std::size_t col = 0; col < snapshot.size.columns; ++col)
            text += QString::fromUcs4(
                snapshot.text(row * snapshot.size.columns + col).data(),
                static_cast<qsizetype>(snapshot.text(row * snapshot.size.columns + col).size()));
        text += QLatin1Char('\n');
    }
    return text;
}
void send(SessionPreview& document, const QByteArray& command) {
    require(document.inputReady());
    document.sendText(command + '\r');
}
void contains(SessionPreview& document, const QString& marker) {
    try {
        until([&] { return screen(document).contains(marker); }, "controlled marker");
    } catch (const std::exception&) {
        throw std::runtime_error("Missing controlled marker " + marker.toStdString() +
                                 " in screen: " + screen(document).toStdString());
    }
}
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
std::vector<QString> savedIds(const QString& path) {
    std::vector<QString> result;
    const auto entries = QJsonDocument::fromJson(read(path)).object().value("entries").toArray();
    for (const auto& entry : entries)
        result.push_back(entry.toObject().value("sessionId").toString());
    return result;
}
void loadFailureRecovery() {
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-load-retry-XXXXXX")};
    require(directory.isValid());
    const QString manifest = directory.filePath(QStringLiteral("owner/workspace.json"));

    {
        // Exercise actual flock contention, not a mocked load error.
        auto locked_registry = std::make_unique<WorkspaceRegistry>(manifest);
        WorkspaceOptions options;
        options.manifest = manifest;
        Workspace workspace{WorkspaceMode::live, options};
        until([&] { return !workspace.loading(); }, "contended manifest load");
        require(!workspace.canAddSessions() && workspace.canRetrySave() &&
                workspace.sessions().isEmpty() && !workspace.status().isEmpty());
        require(workspace.status().contains(QStringLiteral("already locked")));
        require(!QFile::exists(manifest));

        // Retrying is user-directed and asynchronous; releasing the external
        // lock is the external repair action. The recovered Workspace must be
        // retired before the independent corrupt-load phase takes the same lock.
        locked_registry.reset();
        workspace.retrySave();
        require(workspace.loading() && !workspace.canRetrySave());
        until([&] { return workspace.canAddSessions(); }, "lock-contention load retry");
        require(workspace.sessions().isEmpty() && !workspace.canRetrySave());
    }

    // A corrupt read retains its refusal and must not be overwritten. Repairing
    // the file externally makes the same explicit retry load the valid manifest.
    QFile corrupt(manifest);
    require(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray broken = QByteArrayLiteral("{broken");
    require(corrupt.write(broken) == broken.size());
    corrupt.close();
    require(::chmod(QFile::encodeName(manifest).constData(), 0600) == 0);

    WorkspaceOptions corrupt_options;
    corrupt_options.manifest = manifest;
    Workspace corrupt_workspace{WorkspaceMode::live, corrupt_options};
    until([&] { return !corrupt_workspace.loading(); }, "corrupt manifest load");
    require(!corrupt_workspace.canAddSessions() && corrupt_workspace.canRetrySave() &&
            corrupt_workspace.sessions().isEmpty());
    require(corrupt_workspace.status().contains(QStringLiteral("JSON is corrupt")));
    require(read(manifest) == broken);

    corrupt_workspace.retrySave();
    require(corrupt_workspace.loading() && !corrupt_workspace.canRetrySave());
    until([&] { return !corrupt_workspace.loading() && corrupt_workspace.canRetrySave(); },
          "unchanged corrupt manifest retry");
    require(!corrupt_workspace.canAddSessions() && read(manifest) == broken &&
            corrupt_workspace.status().contains(QStringLiteral("JSON is corrupt")));

    require(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray repaired = QByteArrayLiteral("{\"entries\":[],\"schema\":1}");
    require(corrupt.write(repaired) == repaired.size());
    corrupt.close();
    corrupt_workspace.retrySave();
    until([&] { return corrupt_workspace.canAddSessions(); }, "repaired manifest load retry");
    require(corrupt_workspace.sessions().isEmpty() && !corrupt_workspace.canRetrySave() &&
            read(manifest) == repaired);
}
struct Processes {
    qint64 shell{};
    qint64 service{};
    bool operator==(const Processes&) const = default;
};
Processes probe(SessionPreview& document, const QByteArray& label) {
    send(document, "printf '\\033[2J\\033[H" + label + ":%s:%s:END\\n' \"$$\" \"$PPID\"");
    const QRegularExpression pattern(QString::fromLatin1(label) +
                                     QStringLiteral(":([0-9]+):([0-9]+):END"));
    QRegularExpressionMatch match;
    until(
        [&] {
            match = pattern.match(screen(document));
            return match.hasMatch();
        },
        "controlled process probe");
    return {match.captured(1).toLongLong(), match.captured(2).toLongLong()};
}
bool gone(qint64 pid) { return ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH; }
struct Fixture {
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-workspace-XXXXXX")};
    QString manifest{directory.filePath(QStringLiteral("owner/workspace.json"))};
    std::unique_ptr<Workspace> workspace;
    std::vector<Processes> children;
    Fixture() {
        require(directory.isValid());
        qputenv("LAPIS_HISTORY_ROOT", directory.filePath(QStringLiteral("history")).toUtf8());
        qputenv("LAPIS_HISTORY_SESSION_BYTES", "262144");
        qputenv("LAPIS_HISTORY_GLOBAL_BYTES", "393216");
        open();
    }
    ~Fixture() {
        // Only fixture-owned child PIDs are signalled on failure; successful cases exit in-band.
        for (const auto& child : children)
            if (!gone(child.shell))
                ::kill(static_cast<pid_t>(child.shell), SIGHUP);
        workspace.reset();
        QThreadPool::globalInstance()->waitForDone();
    }
    void open() {
        WorkspaceOptions options;
        options.manifest = manifest;
        workspace = std::make_unique<Workspace>(WorkspaceMode::live, options);
        until([&] { return !workspace->loading(); }, "workspace fixture open");
    }
    void close() {
        workspace.reset();
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    SessionPreview* add() {
        require(workspace->addSession(false, directory.path()));
        auto* document = workspace->sessions().back().value<SessionPreview*>();
        until([&] { return document->inputReady(); }, "new session readiness");
        // Split the marker so echoed input cannot satisfy the readiness check.
        send(*document, "PS1=; PS2=; unset PROMPT_COMMAND; stty -echo; printf "
                        "'\\033[2J\\033[H%s%s\\n' LAPIS_ READY");
        contains(*document, QStringLiteral("LAPIS_READY"));
        children.push_back(probe(*document, QByteArrayLiteral("START")));
        return document;
    }
};
void sharedHistory(Fixture& fixture, SessionPreview& first, SessionPreview& second) {
    const QString root = fixture.directory.filePath(QStringLiteral("history"));
    // Both independent service processes archive concurrently under the same global quota.
    send(first, "i=0; while [ $i -lt 2000 ]; do printf 'QUOTA_A_%04d\\n' $i; i=$((i+1)); done");
    send(second, "i=0; while [ $i -lt 2000 ]; do printf 'QUOTA_B_%04d\\n' $i; i=$((i+1)); done");
    contains(first, QStringLiteral("QUOTA_A_1999"));
    contains(second, QStringLiteral("QUOTA_B_1999"));
    for (auto* document : {&first, &second}) {
        document->olderHistory();
        until([&] { return !document->historyRequestPending(); }, "history page");
        require(document->historyMessage().isEmpty());
        document->returnToLive();
    }
    qint64 bytes{};
    int pages{};
    QDirIterator files(root, {QStringLiteral("*.page")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        bytes += files.fileInfo().size();
        ++pages;
    }
    require(bytes > 0 && bytes <= 393216 && pages < 100);
    const auto lock = QFile::encodeName(root + QStringLiteral("/.lock"));
    require(::chmod(lock.constData(), 0644) == 0);
    // A real shared-archive validation failure must leave both retained live screens usable.
    first.olderHistory();
    second.olderHistory();
    until([&] { return !first.historyRequestPending() && !second.historyRequestPending(); },
          "shared history failure handling");
    require(!first.historyMessage().isEmpty() && !second.historyMessage().isEmpty());
    first.returnToLive();
    second.returnToLive();
    contains(first, QStringLiteral("QUOTA_A_1999"));
    contains(second, QStringLiteral("QUOTA_B_1999"));
    require(first.inputReady() && second.inputReady());
    require(::chmod(lock.constData(), 0600) == 0);
}
void saveFailureRecovery() {
    Fixture fixture;
    auto* first = fixture.add();
    auto* second = fixture.add();
    const auto firstId = first->sessionId();
    const auto secondId = second->sessionId();
    until([&] { return savedIds(fixture.manifest).size() == 2; },
          "initial two-session registry write");
    const auto original = read(fixture.manifest);
    // Fail an actual save after a visible removal, without disturbing live services.
    require(::chmod(QFile::encodeName(fixture.manifest).constData(), 0644) == 0);
    require(fixture.workspace->removeSession(firstId));
    until([&] { return fixture.workspace->canRetrySave(); }, "save-failure retry state");
    require(!fixture.workspace->canAddSessions() && !fixture.workspace->status().isEmpty());
    require(read(fixture.manifest) == original && !gone(fixture.children[0].shell));
    fixture.workspace->retrySave();
    require(!fixture.workspace->canRetrySave());
    until([&] { return fixture.workspace->canRetrySave(); }, "failed-save retry rearm");
    require(read(fixture.manifest) == original);
    // Repair permissions but introduce corrupt content: explicit retry must still refuse it.
    require(::chmod(QFile::encodeName(fixture.manifest).constData(), 0600) == 0);
    QFile file(fixture.manifest);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray corrupt = "{broken";
    require(file.write(corrupt) == corrupt.size());
    file.close();
    fixture.workspace->retrySave();
    until([&] { return fixture.workspace->canRetrySave(); }, "corruption-refusal retry state");
    require(read(fixture.manifest) == corrupt && !fixture.workspace->canAddSessions());
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    require(file.write(original) == original.size());
    file.close();
    fixture.workspace->retrySave();
    until([&] { return fixture.workspace->canAddSessions(); }, "repaired registry persistence");
    require(!fixture.workspace->canRetrySave() && fixture.workspace->status().isEmpty());
    require(savedIds(fixture.manifest) == std::vector<QString>{secondId});
    require(probe(*second, QByteArrayLiteral("AFTER_SAVE_RETRY")) == fixture.children[1]);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    qputenv("SHELL", "/bin/sh");
    try {
        loadFailureRecovery();
        saveFailureRecovery();
        Fixture fixture;
        auto& workspace = *fixture.workspace;
        require(workspace.canAddSessions() && workspace.sessions().isEmpty());
        auto* first = fixture.add();
        workspace.setInteractionBlocked(QStringLiteral("held-key"), true);
        auto* second = fixture.add();
        require(workspace.focusedSession() == first);
        workspace.setInteractionBlocked(QStringLiteral("held-key"), false);
        until([&] { return workspace.focusedSession() == second; }, "deferred manual focus");
        require(fixture.children[0].shell != fixture.children[1].shell);
        require(fixture.children[0].service != fixture.children[1].service);
        const std::vector<QString> ids{first->sessionId(), second->sessionId()};
        require(ids[0] != ids[1]);
        until([&] { return savedIds(fixture.manifest) == ids; });

        require(!workspace.focusAutomatically(QStringLiteral("removed-session")));
        workspace.setInteractionBlocked(QStringLiteral("paste"), true);
        require(!workspace.focusAutomatically(ids[0]));
        workspace.setInteractionBlocked(QStringLiteral("paste"), false);
        QCoreApplication::processEvents();
        require(workspace.focusedSession() == second); // Automatic attempts are never replayed.
        require(workspace.focusAutomatically(ids[0]));
        workspace.setInteractionBlocked(QStringLiteral("composition"), true);
        workspace.setFocusedIndex(1);
        workspace.setInteractionBlocked(QStringLiteral("composition"), false);
        require(!workspace.focusAutomatically(ids[1])); // Deferred manual intent wins first.
        until([&] { return workspace.focusedSession() == second; }, "composition-blocked focus");

        workspace.setInteractionBlocked(QStringLiteral("held-key"), true);
        workspace.setFocusedIndex(0);
        workspace.setFocusedIndex(1); // Selecting the current owner cancels a pending switch.
        workspace.setInteractionBlocked(QStringLiteral("held-key"), false);
        QCoreApplication::processEvents();
        require(workspace.focusedSession() == second);
        first->resizeTerminal({57, 19});
        second->resizeTerminal({43, 23});
        until(
            [&] {
                return first->snapshot().size == TerminalSize{57, 19} &&
                       second->snapshot().size == TerminalSize{43, 23};
            },
            "deferred terminal resize");
        send(*first, "printf 'ONLY_FIRST\\n'");
        send(*second, "printf 'ONLY_SECOND\\n'");
        contains(*first, QStringLiteral("ONLY_FIRST"));
        contains(*second, QStringLiteral("ONLY_SECOND"));
        require(!screen(*first).contains(QStringLiteral("ONLY_SECOND")) &&
                !screen(*second).contains(QStringLiteral("ONLY_FIRST")));
        send(*first,
             "i=0; while [ $i -lt 80 ]; do printf 'HISTORY_A_%02d\\n' $i; i=$((i+1)); done");
        contains(*first, QStringLiteral("HISTORY_A_79"));
        first->olderHistory();
        until([&] { return first->historyActive() && !first->historyRequestPending(); },
              "older history page");
        require(first->historyMessage().isEmpty());
        const auto history = screen(*first);
        require(history.contains(QStringLiteral("HISTORY_A_")) && !first->inputReady());
        require(!workspace.focusAutomatically(ids[0]));
        workspace.setFocusedIndex(0);
        require(!workspace.focusAutomatically(ids[1])); // Reading history retains input ownership.
        workspace.setFocusedIndex(1);
        send(*second, "printf 'BACKGROUND_SECOND\\n'");
        contains(*second, QStringLiteral("BACKGROUND_SECOND"));
        require(screen(*first) == history && workspace.focusedSession() == second);
        first->returnToLive();
        contains(*first, QStringLiteral("HISTORY_A_79"));
        const auto entry = first->reconnectEntry();
        if (!entry)
            throw std::runtime_error("Missing verified workspace entry");
        require(!workspace.addSession(false, fixture.directory.path(), entry.value().endpoint));
        require(workspace.sessions().size() == 2);
        sharedHistory(fixture, *first, *second);
        send(*first, "printf 'BEFORE_REOPEN_A\\n'");
        send(*second, "printf 'BEFORE_REOPEN_B\\n'");
        contains(*first, QStringLiteral("BEFORE_REOPEN_A"));
        contains(*second, QStringLiteral("BEFORE_REOPEN_B"));

        fixture.close();
        require(!gone(fixture.children[0].shell) && !gone(fixture.children[1].shell));
        fixture.open();
        auto& reopened = *fixture.workspace;
        first = reopened.session(ids[0]);
        second = reopened.session(ids[1]);
        require(first && second);
        until([&] { return first->inputReady() && second->inputReady(); },
              "workspace reopen readiness");
        contains(*first, QStringLiteral("BEFORE_REOPEN_A"));
        contains(*second, QStringLiteral("BEFORE_REOPEN_B"));
        require(first->snapshot().size == TerminalSize{57, 19} &&
                second->snapshot().size == TerminalSize{43, 23});
        require(probe(*first, QByteArrayLiteral("REOPEN_A")) == fixture.children[0]);
        require(probe(*second, QByteArrayLiteral("REOPEN_B")) == fixture.children[1]);
        send(*first, "exit");
        until([&] { return first->connectionState() == QStringLiteral("ended"); },
              "explicit session exit");
        until([&] { return gone(fixture.children[0].service); }, "session-service exit");
        send(*second, "printf 'SURVIVOR_READY\\n'");
        contains(*second, QStringLiteral("SURVIVOR_READY"));
        const auto secondEntry = second->reconnectEntry();
        if (!secondEntry)
            throw std::runtime_error("Missing surviving workspace entry");
        // No event-loop turn between the two mutations and destruction: the second
        // save is coalesced behind the first and must survive watcher teardown.
        require(reopened.removeSession(ids[0]));
        require(reopened.removeSession(ids[1]));
        fixture.close();
        require(savedIds(fixture.manifest).empty());
        require(!gone(fixture.children[1].shell));
        fixture.open();
        require(fixture.workspace->sessions().isEmpty());
        require(fixture.workspace->addSession(false, fixture.directory.path(),
                                              secondEntry.value().endpoint));
        second = fixture.workspace->focusedSession();
        until([&] { return second->inputReady(); }, "endpoint adoption readiness");
        require(probe(*second, QByteArrayLiteral("ADOPT_B")) == fixture.children[1]);
        send(*second, "exit");
        until([&] { return second->connectionState() == QStringLiteral("ended"); },
              "adopted session exit");
        until([&] { return gone(fixture.children[1].service); }, "adopted service exit");
        fixture.close();
        const auto valid = read(fixture.manifest);
        require(!valid.isEmpty());
        QFile file(fixture.manifest);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray corrupt = QByteArrayLiteral("{broken");
        require(file.write(corrupt) == corrupt.size());
        file.close();
        fixture.open();
        require(fixture.workspace->sessions().isEmpty() && !fixture.workspace->canAddSessions());
        require(!fixture.workspace->status().isEmpty() && read(fixture.manifest) == corrupt);
        std::cout << "Two services: independent I/O, geometry, history, retained children, "
                     "adoption and failure isolation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
