#include "workspace.hpp"

#include <QCoreApplication>
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
#include <vector>

namespace {
using namespace lapis::desktop;
using namespace lapis::session;
void require(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("Workspace check failed at " + std::to_string(where.line()));
}
template <typename F>
void until(F predicate, std::source_location where = std::source_location::current()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, where);
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
    until([&] { return screen(document).contains(marker); });
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
    until([&] {
        match = pattern.match(screen(document));
        return match.hasMatch();
    });
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
        until([&] { return !workspace->loading(); });
    }
    void close() {
        workspace.reset();
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    SessionPreview* add() {
        require(workspace->addSession(false, directory.path()));
        auto* document = workspace->sessions().back().value<SessionPreview*>();
        until([&] { return document->inputReady(); });
        // Split the marker so echoed input cannot satisfy the readiness check.
        send(*document, "stty -echo; printf '\\033[2J\\033[H%s%s\\n' LAPIS_ READY");
        contains(*document, QStringLiteral("LAPIS_READY"));
        children.push_back(probe(*document, QByteArrayLiteral("START")));
        return document;
    }
};
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    qputenv("SHELL", "/bin/sh");
    try {
        Fixture fixture;
        auto& workspace = *fixture.workspace;
        require(workspace.canAddSessions() && workspace.sessions().isEmpty());
        auto* first = fixture.add();
        workspace.setInteractionBlocked(QStringLiteral("held-key"), true);
        auto* second = fixture.add();
        require(workspace.focusedSession() == first);
        workspace.setInteractionBlocked(QStringLiteral("held-key"), false);
        until([&] { return workspace.focusedSession() == second; });
        require(fixture.children[0].shell != fixture.children[1].shell);
        require(fixture.children[0].service != fixture.children[1].service);
        const std::vector<QString> ids{first->sessionId(), second->sessionId()};
        require(ids[0] != ids[1]);
        until([&] { return savedIds(fixture.manifest) == ids; });

        workspace.setInteractionBlocked(QStringLiteral("held-key"), true);
        workspace.setFocusedIndex(0);
        workspace.setFocusedIndex(1); // Selecting the current owner cancels a pending switch.
        workspace.setInteractionBlocked(QStringLiteral("held-key"), false);
        QCoreApplication::processEvents();
        require(workspace.focusedSession() == second);
        first->resizeTerminal({57, 19});
        second->resizeTerminal({43, 23});
        until([&] {
            return first->snapshot().size == TerminalSize{57, 19} &&
                   second->snapshot().size == TerminalSize{43, 23};
        });
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
        until([&] { return first->historyActive() && !first->historyRequestPending(); });
        require(first->historyMessage().isEmpty());
        const auto history = screen(*first);
        require(history.contains(QStringLiteral("HISTORY_A_")) && !first->inputReady());
        workspace.setFocusedIndex(0);
        workspace.setFocusedIndex(1);
        send(*second, "printf 'BACKGROUND_SECOND\\n'");
        contains(*second, QStringLiteral("BACKGROUND_SECOND"));
        require(screen(*first) == history && workspace.focusedSession() == second);
        first->returnToLive();
        contains(*first, QStringLiteral("HISTORY_A_79"));
        const auto entry = first->reconnectEntry();
        require(entry.has_value());
        require(!workspace.addSession(false, fixture.directory.path(), entry->endpoint));
        require(workspace.sessions().size() == 2);

        fixture.close();
        require(!gone(fixture.children[0].shell) && !gone(fixture.children[1].shell));
        fixture.open();
        auto& reopened = *fixture.workspace;
        first = reopened.session(ids[0]);
        second = reopened.session(ids[1]);
        require(first && second);
        until([&] { return first->inputReady() && second->inputReady(); });
        contains(*first, QStringLiteral("HISTORY_A_79"));
        contains(*second, QStringLiteral("BACKGROUND_SECOND"));
        require(probe(*first, QByteArrayLiteral("REOPEN_A")) == fixture.children[0]);
        require(probe(*second, QByteArrayLiteral("REOPEN_B")) == fixture.children[1]);
        send(*first, "exit");
        until([&] { return first->connectionState() == QStringLiteral("ended"); });
        until([&] { return gone(fixture.children[0].service); });
        require(reopened.removeSession(ids[0]));
        until([&] { return savedIds(fixture.manifest) == std::vector<QString>{ids[1]}; });
        send(*second, "printf 'SURVIVOR_READY\\n'");
        contains(*second, QStringLiteral("SURVIVOR_READY"));
        const auto secondEntry = second->reconnectEntry();
        require(secondEntry.has_value());
        require(reopened.removeSession(ids[1])); // Detach a live child, then explicitly adopt it.
        until([&] { return savedIds(fixture.manifest).empty(); });
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(!gone(fixture.children[1].shell));
        require(reopened.addSession(false, fixture.directory.path(), secondEntry->endpoint));
        second = reopened.focusedSession();
        until([&] { return second->inputReady(); });
        require(probe(*second, QByteArrayLiteral("ADOPT_B")) == fixture.children[1]);
        send(*second, "exit");
        until([&] { return second->connectionState() == QStringLiteral("ended"); });
        until([&] { return gone(fixture.children[1].service); });
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
