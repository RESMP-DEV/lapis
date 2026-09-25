// Opt-in real retained-process probe. No GUI, agent prompt, trust response or model turn.
#include "workspace.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <csignal>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace lapis::desktop;
struct Process {
    qint64 pid{};
    qint64 parent{};
    QString command;
};
QList<qint64> observed;
QtMessageHandler previous{};
void messages(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    // LiveConnection::acceptHello emits this exact producer contract.
    const auto match =
        QRegularExpression(QStringLiteral("Connected terminal PID ([0-9]+)")).match(message);
    if (match.hasMatch())
        observed.append(match.captured(1).toLongLong());
    if (previous)
        previous(type, context, message);
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
QList<Process> processes() {
    QProcess ps;
    ps.start(QStringLiteral("/bin/ps"),
             {QStringLiteral("-axo"), QStringLiteral("pid=,ppid=,command=")});
    if (!ps.waitForFinished(5000) || ps.exitCode() != 0)
        throw std::runtime_error("Cannot inspect owned processes");
    QList<Process> result;
    const QRegularExpression row(QStringLiteral("^\\s*(\\d+)\\s+(\\d+)\\s+(.*)$"));
    for (const auto& line :
         QString::fromUtf8(ps.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        const auto match = row.match(line);
        if (match.hasMatch())
            result.append({match.captured(1).toLongLong(), match.captured(2).toLongLong(),
                           match.captured(3)});
    }
    return result;
}
void await(const std::function<bool()>& predicate, const char* message, int limit = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < limit) {
        QCoreApplication::processEvents();
        if (predicate())
            return;
        QThread::msleep(10);
    }
    throw std::runtime_error(message);
}
class OwnedProcesses {
  public:
    explicit OwnedProcesses(QString runtime) : runtime_(std::move(runtime)) {}
    ~OwnedProcesses() {
        if (!finished_) {
            try {
                cleanup();
            } catch (const std::exception& error) {
                std::cerr << "CLEANUP FAILED: " << error.what()
                          << " runtime=" << runtime_.toStdString() << '\n';
            }
        }
    }
    void inspect() {
        const auto all = processes();
        const auto service = QStringLiteral(LAPIS_SESSION_SERVICE_PATH);
        for (const auto& process : all)
            if (process.command.startsWith(service + QLatin1Char(' ')) &&
                process.command.contains(runtime_))
                owned_.insert(process.pid, process.command);
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& process : all)
                if (owned_.contains(process.parent) && !owned_.contains(process.pid)) {
                    owned_.insert(process.pid, process.command);
                    changed = true;
                }
        }
    }
    qint64 serviceFor(qint64 child) {
        inspect();
        for (const auto& process : processes())
            if (process.pid == child && owned_.contains(child) && owned_.contains(process.parent))
                return process.parent;
        throw std::runtime_error("Hello PID does not belong to this probe's private service");
    }
    void cleanup() {
        inspect();
        signalOwned(SIGTERM);
        for (int attempt = 0; attempt < 200 && remaining(); ++attempt) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        if (remaining()) {
            signalOwned(SIGKILL);
            for (int attempt = 0; attempt < 200 && remaining(); ++attempt)
                QThread::msleep(10);
        }
        require(!remaining(), "Owned service or descendant remained after cleanup");
        finished_ = true;
    }

  private:
    [[nodiscard]] bool remaining() const {
        for (const auto& process : processes())
            if (owned_.contains(process.pid) && owned_.value(process.pid) == process.command)
                return true;
        return false;
    }
    void signalOwned(int signal) {
        // Match the complete observed command again before every signal. Never
        // signal a saved PID that has been reused for a different command.
        for (const auto& process : processes())
            if (process.pid != ::getpid() && owned_.value(process.pid) == process.command &&
                owned_.contains(process.pid))
                static_cast<void>(::kill(static_cast<pid_t>(process.pid), signal));
    }
    QString runtime_;
    QMap<qint64, QString> owned_;
    bool finished_{};
};
qint64 ready(Workspace& workspace, const QString& id, QSet<qint64> excluded = {}) {
    const auto before = observed.size();
    await(
        [&] {
            auto* item = workspace.session(id);
            if (!item || !item->inputReady() || item->serviceSessionId().isEmpty())
                return false;
            return std::any_of(observed.begin() + before, observed.end(),
                               [&](qint64 pid) { return !excluded.contains(pid); });
        },
        "Agent did not reach restored-screen input readiness");
    const auto found = std::find_if(observed.begin() + before, observed.end(),
                                    [&](qint64 pid) { return !excluded.contains(pid); });
    return *found;
}
void run_harness_probe(const QString& harness, QTemporaryDir& runtime, OwnedProcesses& owned,
                       QFile& output) {
    WorkspaceOptions options;
    options.storagePath = runtime.filePath(QStringLiteral("workspace.json"));
    QString id, identity;
    qint64 pid{};
    qsizetype visible_cells{};
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.createAgent(runtime.path(), QStringLiteral("Probe"), harness),
                "Native harness launch rejected");
        id = workspace.focusedSession()->sessionId();
        pid = ready(workspace, id);
        static_cast<void>(owned.serviceFor(pid));
        auto* item = workspace.session(id);
        await(
            [&] {
                visible_cells = 0;
                const auto& snapshot = item->snapshot();
                if (item->connectionState() == QStringLiteral("ended"))
                    throw std::runtime_error("Native harness exited during startup");
                for (std::size_t index = 0; index < snapshot.cells.size(); ++index) {
                    const auto text = snapshot.text(index);
                    if (!text.empty() && text.front() > U' ')
                        ++visible_cells;
                }
                return item->inputReady() && visible_cells > 20;
            },
            "Native harness did not render its startup screen");
        identity = item->serviceSessionId();
        require(item->harnessId() == harness, "Native harness label mismatch");
    }
    {
        Workspace restored(WorkspaceMode::live, options);
        const auto restored_pid = ready(restored, id);
        require(restored_pid == pid, "Reconnect replaced the native process");
        require(restored.session(id)->harnessId() == harness &&
                    restored.session(id)->serviceSessionId() == identity,
                "Reconnect lost native harness identity");
    }
    owned.cleanup();
    const auto receipt =
        QJsonDocument(QJsonObject{{"passed", true},
                                  {"harness", harness},
                                  {"modelTurns", 0},
                                  {"nativeTerminalPid", pid},
                                  {"startupNonblankCells", visible_cells},
                                  {"samePidAfterReconnect", true},
                                  {"cleanupComplete", true},
                                  {"scope", "native startup screen and session lifecycle; "
                                            "no auth/trust response or model turn"}})
            .toJson();
    require(output.open(QIODevice::WriteOnly) && output.write(receipt) == receipt.size(),
            "Cannot write native harness receipt");
    std::cout << receipt.constData();
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    previous = qInstallMessageHandler(messages);
    QTemporaryDir runtime(QStringLiteral("/tmp/lapis-live-XXXXXX"));
    if (!runtime.isValid())
        return 1;
    OwnedProcesses owned(runtime.path());
    try {
        if (app.arguments().size() == 4 && app.arguments().at(1) == QStringLiteral("--harness")) {
            QFile output(app.arguments().at(3));
            run_harness_probe(app.arguments().at(2), runtime, owned, output);
            qInstallMessageHandler(previous);
            return 0;
        }
        const auto original_home =
            qEnvironmentVariable("CODEX_HOME", QDir::homePath() + QStringLiteral("/.codex"));
        const auto isolated_home = runtime.filePath(QStringLiteral("codex-home"));
        require(QDir().mkdir(isolated_home, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
                "Create private Codex home");
        const auto auth = QDir(original_home).filePath(QStringLiteral("auth.json"));
        if (QFileInfo::exists(auth))
            require(QFile::link(auth, QDir(isolated_home).filePath(QStringLiteral("auth.json"))),
                    "Link authentication without reading credential contents");
        require(qputenv("CODEX_HOME", isolated_home.toUtf8()), "Set probe-only Codex home");
        WorkspaceOptions options;
        options.storagePath = runtime.filePath(QStringLiteral("workspace.json"));
        QString first, second, category, identity1, identity2;
        qint64 pid1{}, pid2{}, service1{}, service2{};
        {
            Workspace workspace(WorkspaceMode::live, options);
            require(workspace.workspaceError().isEmpty(), "Private workspace failed to initialize");
            require(workspace.createAgent(runtime.path(), QStringLiteral("First")),
                    "First managed Codex launch rejected");
            first = workspace.focusedSession()->sessionId();
            pid1 = ready(workspace, first);
            service1 = owned.serviceFor(pid1);
            identity1 = workspace.session(first)->serviceSessionId();
            require(workspace.addCategory(QStringLiteral("Research")), "Create second category");
            category = workspace.activeCategoryId();
            require(workspace.createAgent(runtime.path(), QStringLiteral("Second")),
                    "Second managed Codex launch rejected");
            second = workspace.focusedSession()->sessionId();
            pid2 = ready(workspace, second, {pid1});
            service2 = owned.serviceFor(pid2);
            identity2 = workspace.session(second)->serviceSessionId();
            require(pid1 != pid2 && service1 != service2,
                    "Independent terminal and service processes required");
            auto* retained = workspace.session(first);
            require(workspace.moveSession(first, category), "Move retained agent");
            require(workspace.moveSessionBy(first, 1), "Reorder retained agent");
            require(workspace.session(first) == retained, "Moving must preserve retained object");
            require(workspace.selectSession(first), "Remember category selection");
        }
        require(owned.serviceFor(pid1) == service1 && owned.serviceFor(pid2) == service2,
                "Closing workspace must preserve both children");
        const auto reconnect_start = observed.size();
        {
            Workspace restored(WorkspaceMode::live, options);
            require(restored.workspaceError().isEmpty(),
                    "Registry lock must release on destruction");
            require(restored.session(first) && restored.session(second),
                    "Both registry identities must restore");
            await(
                [&] {
                    return restored.session(first)->inputReady() &&
                           restored.session(second)->inputReady() &&
                           observed.size() >= reconnect_start + 2;
                },
                "Both retained agents must reconnect");
            const QSet<qint64> reconnects(observed.begin() + reconnect_start, observed.end());
            require(reconnects == QSet<qint64>{pid1, pid2},
                    "Reconnect must report the original terminal PIDs");
            require(restored.session(first)->serviceSessionId() == identity1 &&
                        restored.session(second)->serviceSessionId() == identity2,
                    "Service identities survive reconnect");
            require(restored.activeCategoryId() == category &&
                        restored.focusedSession()->sessionId() == first &&
                        restored.categorySessions().size() == 2,
                    "Category membership and selected tab survive restart");
            require(restored.categorySessions().front().value<SessionPreview*>()->sessionId() ==
                        second,
                    "Tab order survives restart");
        }
        owned.cleanup();
        const auto receipt =
            QJsonDocument(QJsonObject{{"passed", true},
                                      {"modelTurns", 0},
                                      {"terminalPids", QJsonArray{pid1, pid2}},
                                      {"servicePids", QJsonArray{service1, service2}},
                                      {"samePidsAfterReconnect", true},
                                      {"cleanupComplete", true}})
                .toJson();
        if (app.arguments().size() == 2) {
            QFile output(app.arguments()[1]);
            require(output.open(QIODevice::WriteOnly) && output.write(receipt) == receipt.size(),
                    "Cannot write receipt");
        }
        std::cout << receipt.constData();
        qInstallMessageHandler(previous);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            owned.cleanup();
        } catch (const std::exception& cleanup) {
            runtime.setAutoRemove(false);
            std::cerr << "Cleanup requires attention: " << cleanup.what()
                      << " runtime=" << runtime.path().toStdString() << '\n';
        }
        qInstallMessageHandler(previous);
        return 1;
    }
}
