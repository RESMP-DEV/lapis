// Claude Code agents on another machine: their conversation identity (the
// session their hooks report, followed across /clear and resumed on
// restart) and their visual view (the conversation file mirrored over ssh).
// Every "machine" here is a stand-in ssh that runs the command it is given
// on this Mac, in a fixture home; nothing connects anywhere. Windowless.
#include "transcript.hpp"
#include "workspace.hpp"

#include "agent_checkpoint.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::Workspace;
using lapis::desktop::WorkspaceMode;
using lapis::desktop::WorkspaceOptions;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
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

void writeFile(const QString& path, const QByteArray& body, bool executable = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write a fixture file");
    require(file.write(body) == body.size(), "write the fixture body");
    file.close();
    if (executable)
        require(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
                "make the fixture executable");
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// PATH with the fixture's bin first, restored on scope exit.
class ScopedPath {
  public:
    explicit ScopedPath(const QString& bin) : previous_(qgetenv("PATH")) {
        qputenv("PATH", QFile::encodeName(bin) + ':' + previous_);
    }
    ~ScopedPath() { qputenv("PATH", previous_); }
    ScopedPath(const ScopedPath&) = delete;
    ScopedPath& operator=(const ScopedPath&) = delete;

  private:
    QByteArray previous_;
};

// A stand-in for every machine: an agent's own ssh (`-t`) records its
// command and ends; any other ssh (the thread helper, an upload) runs its
// last argument here with HOME set to the fixture's remote home, as that
// machine's shell would. `fail-<code>` beside it makes the next calls fail
// that way, printing its contents to stderr.
struct Machine {
    QDir root;
    QString ssh;
    QString home;
    explicit Machine(const QDir& at) : root(at) {
        ssh = root.filePath(QStringLiteral("bin/ssh"));
        home = root.filePath(QStringLiteral("home"));
        require(root.mkpath(QStringLiteral("home")) && root.mkpath(QStringLiteral("calls")),
                "a home on the stand-in machine");
        writeFile(ssh, R"(#!/bin/sh
d=$(cd "$(dirname "$0")/.." && pwd)
n=$(($(cat "$d/calls/count" 2>/dev/null || echo 0) + 1)); echo $n > "$d/calls/count"
printf '%s\n' "$@" > "$d/calls/$n"
for last; do :; done
case " $* " in *" -t "*) echo "agent started"; exit 0;; esac
for f in "$d"/fail-*; do
  [ -e "$f" ] || continue
  cat "$f" >&2; code=${f##*-}; exit "$code"
done
export HOME="$d/home"
unset CLAUDE_CONFIG_DIR
if [ -e "$d/no-python" ]; then PATH=/nonexistent; export PATH; fi
cd "$HOME" && exec /bin/sh -c "$last"
)",
                  true);
    }
    [[nodiscard]] int calls() const {
        return readFile(root.filePath(QStringLiteral("calls/count"))).trimmed().toInt();
    }
    [[nodiscard]] QString call(int number) const {
        return QString::fromUtf8(readFile(root.filePath(QStringLiteral("calls/%1").arg(number))));
    }
    // The last call made with `-t` (an agent's own command), or empty.
    [[nodiscard]] QString lastAgentCall() const {
        for (int n = calls(); n > 0; --n)
            if (const auto text = call(n); text.contains(QStringLiteral("\n-t\n")))
                return text;
        return {};
    }
};

QString conversationIn(const QString& command) {
    static const QRegularExpression id(QStringLiteral(R"( && s=([0-9a-f-]{36}) && )"));
    return id.match(command).captured(1);
}

QString endpointOf(const QString& storage, const QString& agent) {
    const auto agents =
        QJsonDocument::fromJson(readFile(storage)).object()[QStringLiteral("agents")].toArray();
    for (const auto& value : agents)
        if (value.toObject()[QStringLiteral("id")].toString() == agent)
            return value.toObject()[QStringLiteral("endpoint")].toString();
    return {};
}

// Stage 0. A remote Claude Code agent's conversation is the one its command
// names until its hooks report one; a relayed session (after /clear) then
// wins, a printed checkpoint never does, and a restart resumes the relayed
// session by rewriting the id in the remote command (ssh's own arguments are
// unchanged), still stopping a process left on the previous id.
void remoteClaudeFollowsItsRelayedSession() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-remote-id-XXXXXX"));
    require(directory.isValid(), "remote identity directory");
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    Machine machine(root);
    ScopedPath path(root.filePath(QStringLiteral("bin")));
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("runtime/workspace.json"));
    require(root.mkpath(QStringLiteral("runtime")), "a runtime folder");
    QFile::setPermissions(root.filePath(QStringLiteral("runtime")),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Workspace workspace(WorkspaceMode::live, options);
    require(workspace.createAgent(QStringLiteral("~/proj"), QStringLiteral("far"),
                                  QStringLiteral("claude"), {}, {}, QStringLiteral("buildbox")),
            "a Claude Code agent on another machine");
    auto* far = workspace.focusedAgent();
    require(far != nullptr, "the agent is shown");
    const auto id = far->agentId();
    require(waitFor([&] { return !machine.lastAgentCall().isEmpty(); }, 10000), "ssh starts");
    const auto first = machine.lastAgentCall();
    const auto original = conversationIn(first);
    require(!original.isEmpty(), "the command names a conversation");
    require(workspace.agentConversation(id) == original,
            "before any hook, the agent's conversation is the one its command names");
    // It may end before the window attaches (then it reads disconnected).
    const auto ended = [far] {
        return far->connectionState() == QStringLiteral("ended") ||
               far->connectionState() == QStringLiteral("disconnected");
    };
    require(waitFor(ended, 10000), "the stand-in agent ends");
    const auto endpoint = endpointOf(options.storagePath, id);
    require(!endpoint.isEmpty(), "the agent's endpoint");

    // A printed checkpoint is not trusted for a Claude Code agent.
    lapis::session::write_resume_record(
        endpoint, {QStringLiteral("claude"), QStringLiteral("5e550000-0000-4000-8000-0000000000aa"),
                   lapis::session::ResumeSource::terminal});
    require(workspace.agentConversation(id) == original, "a printed checkpoint does not win");

    // Its hooks reported another session (/clear there).
    const auto cleared = QStringLiteral("5e550000-0000-4000-8000-000000000002");
    lapis::session::write_resume_record(
        endpoint, {QStringLiteral("claude"), cleared, lapis::session::ResumeSource::relayed});
    require(workspace.agentConversation(id) == cleared,
            "the relayed session is the agent's conversation");
    require(workspace.agentConversations().value(id) == cleared, "and in the full list");

    const int before = machine.calls();
    // The service reports the end just before it exits.
    require(waitFor([&] { return workspace.restartAgent(id); }, 10000), "restart the agent");
    require(waitFor([&] { return machine.calls() > before && !machine.lastAgentCall().isEmpty() &&
                                 conversationIn(machine.lastAgentCall()) == cleared; },
                    10000),
            "the restart resumes the relayed session");
    const auto restarted = machine.lastAgentCall();
    require(restarted.contains(QStringLiteral(R"re(p="[-]-(resume|session-id) ($s|%1)")re")
                                   .arg(original)),
            "it still stops a process left on the previous session");
    const auto host = QStringLiteral("\n-t\nbuildbox\n");
    QString expected = first;
    expected.replace(original, cleared);
    expected.replace(QStringLiteral(R"re(p="[-]-(resume|session-id) $s")re"),
                     QStringLiteral(R"re(p="[-]-(resume|session-id) ($s|%1)")re").arg(original));
    require(restarted.left(restarted.indexOf(host)) == first.left(first.indexOf(host)) &&
                restarted == expected,
            "ssh's own arguments are unchanged; only the remote command's id moved");
    require(!restarted.contains(QStringLiteral("\n--resume\n")),
            "no resume option is appended to ssh");
    const auto saved = QJsonDocument::fromJson(readFile(options.storagePath)).object();
    require(QJsonDocument(saved).toJson().contains(cleared.toUtf8()),
            "the registry keeps the moved command");
    require(waitFor(ended, 10000), "the restarted stand-in ends");
    require(workspace.closeAgent(id, true), "close the agent");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    try {
        remoteClaudeFollowsItsRelayedSession();
        std::cout << "remote-thread: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "remote-thread: " << error.what() << '\n';
        return 1;
    }
}
