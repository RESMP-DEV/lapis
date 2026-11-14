// Claude Code agents on another machine: their conversation identity (the
// session their hooks report, followed across /clear and resumed on
// restart) and their visual view (the conversation file mirrored over ssh).
// Every "machine" here is a stand-in ssh that runs the command it is given
// on this Mac, in a fixture home; nothing connects anywhere. Windowless.
#include "remote_threads.hpp"
#include "slash_commands.hpp"
#include "transcript.hpp"
#include "workspace.hpp"

#include "agent_checkpoint.hpp"

#include <QDateTime>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
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

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

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
// Each call is numbered under a lock (calls can overlap: an agent's ssh and
// the thread helper's) and its file is renamed into place whole before the
// count names it: an agent's command runs to about 11 kB, and under load the
// test could read it half written.
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
i=0; until mkdir "$d/calls/.lock" 2>/dev/null || [ $i -ge 300 ]; do i=$((i+1)); sleep 0.01; done
n=$(($(cat "$d/calls/count" 2>/dev/null || echo 0) + 1))
printf '%s\n' "$@" > "$d/calls/.$n.$$" && mv -f "$d/calls/.$n.$$" "$d/calls/$n"
echo $n > "$d/calls/.count.$$" && mv -f "$d/calls/.count.$$" "$d/calls/count"
rmdir "$d/calls/.lock" 2>/dev/null
for last; do :; done
case " $* " in *" -t "*) echo "agent started"; exit 0;; esac
for f in "$d"/fail-*; do
  [ -e "$f" ] || continue
  cat "$f" >&2; code=${f##*-}; exit "$code"
done
export HOME="$d/home"
unset CLAUDE_CONFIG_DIR
if [ -e "$d/no-python" ]; then PATH=/nonexistent; export PATH; fi
if [ -e "$d/slow" ]; then cd "$HOME" && /bin/sh -c "$last" | python3 "$d/bin/throttle.py"; exit; fi
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

// Positional contract; callers and tests pin the order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// Stage 1 ---------------------------------------------------------------------

QByteArray record(int n) {
    return n % 2 == 1
               ? QStringLiteral(R"({"type":"user","uuid":"u%1","message":{"role":"user",)"
                                R"("content":"prompt %1"}})")
                     .arg(n)
                     .toUtf8() +
                     '\n'
               : QStringLiteral(R"({"type":"assistant","uuid":"a%1","message":{"role":)"
                                R"("assistant","content":[{"type":"text","text":"reply %1"}]}})")
                     .arg(n)
                     .toUtf8() +
                     '\n';
}
// Positional contract; callers and tests pin the order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QByteArray records(int from, int to) {
    QByteArray out;
    for (int n = from; n <= to; ++n)
        out += record(n);
    return out;
}
void appendFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Append), "append to a fixture file");
    require(file.write(bytes) == bytes.size(), "append the fixture bytes");
}
// The items a whole reading of `path` gives, as the view would read it.
std::size_t itemsIn(const QString& path) {
    lapis::desktop::TranscriptModel reference;
    reference.setPath(path);
    reference.loadNow();
    return reference.items().size();
}

// A remote agent's thread as the window shows it: the view asks for the
// file and shows it, and the model reads what the file gained.
struct View {
    Workspace& workspace;
    QString agent;
    [[nodiscard]] lapis::desktop::TranscriptModel* model() const {
        return qobject_cast<lapis::desktop::TranscriptModel*>(workspace.transcript());
    }
    // Called for its effect too; the path is for the checks that want it.
    // NOLINTNEXTLINE(modernize-use-nodiscard)
    QString show() const {
        const auto path = workspace.conversationFile(agent);
        model()->setShown({{QStringLiteral("agent"), agent},
                           {QStringLiteral("harness"), QStringLiteral("claude")},
                           {QStringLiteral("path"), path}});
        model()->readMore();
        return path;
    }
    void hide() const { model()->setShown({}); }
    [[nodiscard]] std::size_t items() const { return model()->items().size(); }
    [[nodiscard]] QString reason() const { return workspace.visualUnavailable(agent); }
    [[nodiscard]] QVariantMap state() const { return workspace.remoteThreadState(agent); }
};

struct RemoteFixture {
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-remote-thread-XXXXXX")};
    QDir root;
    std::unique_ptr<Machine> machine;
    std::unique_ptr<ScopedPath> path;
    WorkspaceOptions options;
    std::unique_ptr<Workspace> workspace;
    RemoteFixture() {
        require(directory.isValid(), "remote thread directory");
        root = QDir(QFileInfo(directory.path()).canonicalFilePath());
        machine = std::make_unique<Machine>(root);
        path = std::make_unique<ScopedPath>(root.filePath(QStringLiteral("bin")));
        // Slows a helper's output (64 KiB every 20 ms) while `slow` exists.
        writeFile(root.filePath(QStringLiteral("bin/throttle.py")), R"(import os, sys, time
while True:
    data = os.read(0, 65536)
    if not data:
        break
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()
    time.sleep(0.02)
)");
        require(root.mkpath(QStringLiteral("runtime")) && root.mkpath(QStringLiteral("home/proj")),
                "a runtime folder and a project there");
        QFile::setPermissions(root.filePath(QStringLiteral("runtime")),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        options.storagePath = root.filePath(QStringLiteral("runtime/workspace.json"));
        workspace = std::make_unique<Workspace>(WorkspaceMode::live, options);
        auto* threads = workspace->remoteThreads();
        require(threads != nullptr, "a workspace with a registry mirrors remote threads");
        lapis::desktop::RemoteThreads::Timing timing;
        timing.keep = std::chrono::milliseconds(800);
        timing.waits = {std::chrono::milliseconds(100)};
        timing.check = std::chrono::milliseconds(100);
        timing.tail_bytes = qint64{1024} * 1024;
        threads->setTimingForTesting(timing);
    }
    ~RemoteFixture() {
        // Cleanup is best effort; a failure here must not end the run.
        try {
            for (const auto& id : workspace->agentConversations().keys())
                workspace->closeAgent(id, true);
            waitFor([this] { return workspace->agents().isEmpty(); }, 5000);
        } catch (...) {
            std::cerr << "remote-thread: fixture cleanup failed\n";
        }
        workspace.reset();
    }
    RemoteFixture(const RemoteFixture&) = delete;
    RemoteFixture& operator=(const RemoteFixture&) = delete;
    QString agent(const QString& title) {
        require(workspace->createAgent(QStringLiteral("~/proj"), title, QStringLiteral("claude"),
                                       {}, {}, QStringLiteral("buildbox")),
                "a Claude Code agent on another machine");
        auto* item = workspace->focusedAgent();
        require(item != nullptr, "the agent is shown");
        return item->agentId();
    }
    // Where Claude Code there keeps `conversation` for ~/proj.
    [[nodiscard]] QString transcript(const QString& conversation) const {
        auto folder = root.filePath(QStringLiteral("home/proj"));
        for (auto& c : folder)
            if (!(c.isLetterOrNumber() && c.unicode() < 128))
                c = QLatin1Char('-');
        return root.filePath(QStringLiteral("home/.claude/projects/%1/%2.jsonl")
                                 .arg(folder, conversation));
    }
    [[nodiscard]] QString cache(const QString& conversation) const {
        return root.filePath(QStringLiteral("runtime/remote/buildbox/%1.jsonl").arg(conversation));
    }
    void marker(const QString& name, const QByteArray& body = {}) const {
        writeFile(root.filePath(name), body);
    }
    void clear(const QString& name) const { QFile::remove(root.filePath(name)); }
};

// The thread follows the file there: appends add rows, a killed helper
// resumes by offset without repeating any, a truncated file resets the copy,
// and nothing about the view resolves on this Mac.
void remoteThreadFollowsTheFile() {
    RemoteFixture fixture;
    auto& workspace = *fixture.workspace;
    const auto id = fixture.agent(QStringLiteral("far"));
    const auto conversation = workspace.agentConversation(id);
    const auto remote = fixture.transcript(conversation);
    const auto cache = fixture.cache(conversation);
    writeFile(remote, records(1, 4));
    View view{workspace, id};
    require(workspace.visualSupported(id), "a remote Claude Code agent can show as a thread");
    require(waitFor([&] { return view.show() == cache && view.items() == itemsIn(remote); }, 10000),
            "the thread shows the mirrored conversation");
    require(readFile(cache) == readFile(remote), "the mirror is the file");
    require(view.model()->remote() && !view.model()->forkable(),
            "the view knows it is remote; fork and edit are off");
    require(workspace.visualFolder(id) == QStringLiteral("~/proj") &&
                workspace.agentFolder(id) != QStringLiteral("~/proj"),
            "the view's folder is the remote one, never resolved here");

    // Appends arrive as rows.
    appendFile(remote, records(5, 6));
    require(waitFor([&] { return view.show() == cache && view.items() == itemsIn(remote); }, 10000),
            "appending to the file there adds rows");

    // A helper that dies resumes from the cache's size: no repeated rows.
    auto* threads = workspace.remoteThreads();
    threads->killForTesting(id);
    require(waitFor([&] { return !threads->running(id); }, 5000), "the helper ends");
    appendFile(remote, records(7, 8));
    require(waitFor(
                [&] {
                    view.show();
                    return threads->running(id) && readFile(cache) == readFile(remote);
                },
                10000),
            "it reconnects and resumes at the cache's size");
    require(waitFor([&] { view.show(); return view.items() == itemsIn(remote); }, 5000),
            "no row is repeated");
    const auto calls = fixture.machine->calls();
    const auto resumed = fixture.machine->call(calls);
    require(resumed.contains(QStringLiteral("--offset ") +
                             QString::number(QFileInfo(remote).size() - records(7, 8).size())),
            "the reconnect asked for the bytes after the cache");
    require(resumed.startsWith(QStringLiteral("-o\nBatchMode=yes\n-o\nConnectTimeout=10\n-o\n"
                                              "ControlPath=none\n")) &&
                resumed.contains(QStringLiteral("\n-T\n--\nbuildbox\npython3 - follow --cli claude "
                                                "--conversation %1 --folder '~/proj' --offset ")
                                     .arg(conversation)),
            "the helper's ssh never asks for a password and passes one quoted command");

    // Truncated there: the copy resets and reloads.
    writeFile(remote, records(101, 102));
    require(waitFor([&] { view.show(); return readFile(cache) == readFile(remote); }, 10000),
            "a truncated file resets the mirror");
    require(waitFor([&] { view.show(); return view.items() == itemsIn(remote); }, 5000),
            "and the thread reloads");

    // Nothing in a remote thread resolves on this Mac.
    const auto mac_file = fixture.root.filePath(QStringLiteral("home-on-mac.png"));
    writeFile(mac_file, "not a picture");
    auto* model = view.model();
    require(model->media(mac_file).isEmpty() && model->replyMedia(mac_file).isEmpty(),
            "a path a reply names is never looked up here");
    const auto pieces = model->pieces(QStringLiteral("See\n\n") + mac_file + QStringLiteral("\n"));
    require(std::ranges::all_of(pieces,
                                [](const QVariant& piece) {
                                    return piece.toMap().value(QStringLiteral("media")).isNull();
                                }) &&
                !pieces.isEmpty(),
            "it stays text");
    require(!model->openPath(mac_file, QStringLiteral("auto")),
            "a same-named file here never opens");
    require(model->slashCommands(QStringLiteral("claude"), QStringLiteral("~/proj")) ==
                lapis::desktop::slash_commands(QStringLiteral("claude"), {}, {}),
            "slash commands are the CLI's own");
    view.hide();
    require(!model->remote(), "a local thread is not remote");

    // The mirror's files are private, under this test's runtime folder.
    require(QFileInfo(fixture.root.filePath(QStringLiteral("runtime/remote/buildbox")))
                    .permissions() == (QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                       QFile::ReadUser | QFile::WriteUser | QFile::ExeUser),
            "the mirror's folder is 0700");
    require((QFileInfo(cache).permissions() & ~(QFile::ReadOwner | QFile::WriteOwner |
                                                 QFile::ReadUser | QFile::WriteUser)) == 0,
            "the mirror is 0600");

    // Not shown any more: its helper stops a moment later.
    require(waitFor([&] { return !threads->running(id); }, 5000),
            "a thread no longer shown stops its helper");
}

// A /clear there moves the mirror to the new conversation's file.
void remoteThreadFollowsClear() {
    RemoteFixture fixture;
    auto& workspace = *fixture.workspace;
    const auto id = fixture.agent(QStringLiteral("cleared"));
    const auto first = workspace.agentConversation(id);
    writeFile(fixture.transcript(first), records(1, 2));
    View view{workspace, id};
    require(waitFor([&] { return view.show() == fixture.cache(first); }, 10000),
            "the first conversation shows");
    auto* agent = workspace.agent(id);
    require(waitFor([agent] { return agent->connectionState() == QStringLiteral("ended") ||
                                     agent->connectionState() == QStringLiteral("disconnected"); },
                    10000),
            "the stand-in agent ended");
    const auto endpoint = endpointOf(fixture.options.storagePath, id);
    const auto cleared = QStringLiteral("c1ea0000-0000-4000-8000-000000000001");
    writeFile(fixture.transcript(cleared), records(1, 6));
    lapis::session::write_resume_record(
        endpoint, {QStringLiteral("claude"), cleared, lapis::session::ResumeSource::relayed});
    // The view's file stays as it was; the mirror moves on its own and says so.
    QString changed;
    QObject::connect(&workspace, &Workspace::remoteThreadChanged,
                     [&changed](const QString& agent) { changed = agent; });
    require(waitFor([&] { return changed == id; }, 5000), "the mirror reports the move");
    require(waitFor([&] { return view.show() == fixture.cache(cleared) &&
                                 view.items() == itemsIn(fixture.transcript(cleared)); },
                    10000),
            "the thread moves to the new conversation");
}

// A file over 4 MB shows its end first, which follows what the CLI appends,
// while the whole mirror fills in; the thread then moves to the whole file.
void remoteThreadShowsALargeFileTailFirst() {
    RemoteFixture fixture;
    auto& workspace = *fixture.workspace;
    const auto id = fixture.agent(QStringLiteral("large"));
    const auto conversation = workspace.agentConversation(id);
    const auto remote = fixture.transcript(conversation);
    QByteArray big;
    for (int n = 1; big.size() < qsizetype{6} * 1024 * 1024; ++n)
        big += record(n).replace("prompt", QByteArray(400, 'x') + " prompt");
    writeFile(remote, big);
    fixture.marker(QStringLiteral("slow"));
    View view{workspace, id};
    const auto tail = fixture.root.filePath(
        QStringLiteral("runtime/remote/buildbox/%1.tail.jsonl").arg(conversation));
    require(waitFor([&] { return view.show() == tail; }, 10000), "the end shows first");
    // It may still be arriving: what is there starts at a line in the last MiB.
    const auto shown = readFile(tail);
    const auto start = big.indexOf(shown, big.size() - qint64{1024} * 1024);
    require(!shown.isEmpty() && start > 0 && big.at(start - 1) == '\n' &&
                QFileInfo(tail).size() <= qint64{1024} * 1024,
            "the end is at most the asked size, cut at a line start");
    const auto appended = records(9001, 9002);
    appendFile(remote, appended);
    require(waitFor([&] { return view.show() == tail && readFile(tail).endsWith(appended); },
                    10000),
            "the end follows what the CLI appends while the rest arrives");
    fixture.clear(QStringLiteral("slow"));
    require(waitFor([&] { return view.show() == fixture.cache(conversation); }, 30000),
            "then the thread moves to the whole mirror");
    require(readFile(fixture.cache(conversation)) == readFile(remote) &&
                waitFor([&] { view.show(); return view.items() == itemsIn(remote); }, 10000),
            "which is the whole file");
    require(waitFor([&] { return !QFileInfo::exists(tail); }, 15000), "the end's copy goes");
}

// Failures with nothing cached leave the terminal (visualSupported false)
// and say why; a cached thread stays and says it may be behind.
void remoteThreadFailuresSayWhy() {
    RemoteFixture fixture;
    auto& workspace = *fixture.workspace;
    const auto id = fixture.agent(QStringLiteral("failing"));
    const auto conversation = workspace.agentConversation(id);
    View view{workspace, id};

    fixture.marker(QStringLiteral("no-python"));
    view.show();
    require(waitFor([&] { view.show(); return !workspace.visualSupported(id); }, 10000) &&
                view.reason() == QStringLiteral("python3 is not installed on buildbox"),
            "no python3 there: the terminal shows, and why");
    fixture.clear(QStringLiteral("no-python"));

    fixture.marker(QStringLiteral("fail-255"), "Permission denied (publickey,password).\n");
    require(waitFor([&] { view.show(); return view.reason().contains(QStringLiteral(
                                                  "needs a password")); },
                    10000) &&
                !workspace.visualSupported(id),
            "ssh that would ask for a password fails, and says so");
    fixture.clear(QStringLiteral("fail-255"));

    fixture.marker(QStringLiteral("fail-3"), "something broke\n");
    require(waitFor([&] { view.show(); return view.reason().contains(QStringLiteral("exit 3")); },
                    10000),
            "another exit is reported with its code");
    fixture.clear(QStringLiteral("fail-3"));

    // Recovered: the missing file is waited for, and the thread may show.
    require(waitFor([&] { view.show(); return view.state().value(QStringLiteral("phase")) ==
                                                QStringLiteral("waiting"); },
                    10000) &&
                workspace.visualSupported(id) &&
                view.reason() == QStringLiteral("The conversation file is not on buildbox yet") &&
                view.state()
                    .value(QStringLiteral("status"))
                    .toString()
                    .contains(QStringLiteral("buildbox")),
            "a conversation not saved there yet is waited for, and the empty thread says so");
    writeFile(fixture.transcript(conversation), records(1, 2));
    require(waitFor([&] { return view.show() == fixture.cache(conversation); }, 10000),
            "once saved there it shows");

    // Lost with a cached copy: the thread stays and says it may be behind.
    fixture.marker(QStringLiteral("fail-255"), "Connection closed by remote host\n");
    workspace.remoteThreads()->killForTesting(id);
    const auto behind = [&] {
        view.show();
        return view.state().value(QStringLiteral("behind")).toBool();
    };
    require(waitFor(behind, 10000) &&
                workspace.visualSupported(id) &&
                view.reason().startsWith(QStringLiteral("Lost the connection to buildbox")),
            "a lost connection keeps the cached thread, marked behind");
    fixture.clear(QStringLiteral("fail-255"));
    require(waitFor([&] { view.show(); return view.state().value(QStringLiteral("phase")) ==
                                                QStringLiteral("following"); },
                    10000),
            "and it reconnects");
}

// Attachments go to the agent's machine first; the message names the copy
// there. Nothing else is written there.
void remoteAttachmentsUploadFirst() {
    RemoteFixture fixture;
    auto& workspace = *fixture.workspace;
    const auto id = fixture.agent(QStringLiteral("attach"));
    const auto local = fixture.root.filePath(QStringLiteral("mac/Screen Shot.png"));
    writeFile(local, QByteArray(qsizetype{300} * 1024, 'p'));
    QString token_done;
    QString remote_path;
    QString failure;
    double progress = 0;
    QObject::connect(&workspace, &Workspace::attachmentUploaded,
                     [&](const QString& token, const QString& remote, const QString& error) {
                         token_done = token;
                         remote_path = remote;
                         failure = error;
                     });
    QObject::connect(&workspace, &Workspace::attachmentProgress,
                     [&progress](const QString&, double fraction) { progress = fraction; });
    const auto started = workspace.uploadAttachment(id, local);
    const auto token = started.value(QStringLiteral("token")).toString();
    require(!token.isEmpty(), "the upload starts");
    require(waitFor([&] { return token_done == token; }, 10000) && failure.isEmpty(),
            "the upload ends");
    const auto folder = fixture.root.filePath(QStringLiteral("home/.cache/lapis/attachments"));
    require(remote_path.startsWith(folder + QLatin1Char('/')) &&
                remote_path.endsWith(QStringLiteral(".png")) &&
                !remote_path.contains(QStringLiteral("Screen Shot")),
            "it lands in the machine's attachments folder, under an unguessable name");
    require(readFile(remote_path) == readFile(local) && progress == 1.0, "whole, with progress");
    require((QFileInfo(remote_path).permissions() & (QFile::ReadGroup | QFile::ReadOther)) == 0 &&
                (QFileInfo(folder).permissions() & (QFile::ReadGroup | QFile::ReadOther)) == 0,
            "private there");
    require(QDir(folder).entryList(QDir::Files).size() == 1, "no partial copy is left");

    const auto big = fixture.root.filePath(QStringLiteral("mac/big.bin"));
    writeFile(big, QByteArray(qsizetype{21} * 1024 * 1024, 'b'));
    require(workspace.uploadAttachment(id, big)
                .value(QStringLiteral("error"))
                .toString()
                .contains(QStringLiteral("20 MB")),
            "files over 20 MB are refused");

    fixture.marker(QStringLiteral("fail-255"), "Connection timed out\n");
    const auto again =
        workspace.uploadAttachment(id, local).value(QStringLiteral("token")).toString();
    require(!again.isEmpty() && waitFor([&] { return token_done == again; }, 10000) &&
                remote_path.isEmpty() && !failure.isEmpty(),
            "a failed upload says why");
    fixture.clear(QStringLiteral("fail-255"));
}

// The composer (Transcript.qml) for an agent on another machine: an
// attachment is copied there first, the message waits for the copy, and the
// mention it sends names the copy there, never the file on this Mac; a
// failed copy keeps the message in the bar.
void composerMentionsOnlyRemoteCopies() {
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(import QtQuick
Item {
    width: 900; height: 700
    property var sent: []
    property var asked: []
    property bool remote: true
    property alias view: view
    Transcript {
        id: view
        anchors.fill: parent
        canSend: true
        agent: remote ? "far" : "here"
        submit: text => { sent = sent.concat([text]); return true }
        upload: remote ? (path => { asked = asked.concat([path]); return { token: "t" + asked.length } })
                       : null
        uploadMachine: remote ? "buildbox" : ""
    }
}
)",
                      QUrl::fromLocalFile(
                          QStringLiteral(LAPIS_QML_DIR "/RemoteComposerCheck.qml")));
    std::unique_ptr<QObject> root(component.create());
    if (!root)
        std::cerr << component.errorString().toStdString() << '\n';
    require(root != nullptr, "the composer loads");
    auto* view = root->property("view").value<QObject*>();
    auto* input = view->findChild<QObject*>(QStringLiteral("visualComposer"));
    require(view != nullptr && input != nullptr, "the composer's input");
    const auto call = [view](const char* method, const QVariantList& arguments = {}) {
        QVariant result;
        switch (arguments.size()) {
        case 0:
            QMetaObject::invokeMethod(view, method, Q_RETURN_ARG(QVariant, result));
            break;
        case 1:
            QMetaObject::invokeMethod(view, method, Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, arguments[0]));
            break;
        default:
            QMetaObject::invokeMethod(view, method, Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, arguments[0]), Q_ARG(QVariant, arguments[1]),
                                      Q_ARG(QVariant, arguments[2]));
        }
        return result;
    };
    const auto sent = [&root] { return root->property("sent").toStringList(); };
    const auto mac = QStringLiteral("/Users/someone/Desktop/shot.png");
    const auto there = QStringLiteral("/home/someone/.cache/lapis/attachments/0a1b.png");
    call("attach", {mac});
    require(root->property("asked").toStringList() == QStringList{mac}, "attaching starts a copy");
    input->setProperty("text", QStringLiteral("look at this"));
    call("send");
    const auto typed = [input] { return input->property("text").toString(); };
    require(sent().isEmpty() && typed() == QStringLiteral("look at this"),
            "the message waits for the copy, in the bar");
    call("uploadFinished", {QStringLiteral("t1"), there, QString()});
    require(view->property("attachments").toStringList() == QStringList{there},
            "the attachment is now the copy there");
    call("send");
    require(sent().size() == 1 && sent().first().contains(QStringLiteral("@") + there) &&
                !sent().first().contains(mac) &&
                !sent().first().contains(QStringLiteral("/Users/")),
            "the mention names the copy there, never the file here");

    // A failed copy keeps the message, and says why.
    const auto second = QStringLiteral("/Users/someone/Desktop/notes.txt");
    call("attach", {second});
    input->setProperty("text", QStringLiteral("and this"));
    call("uploadFinished", {QStringLiteral("t2"), QString(), QStringLiteral("ssh failed")});
    call("send");
    require(sent().size() == 1 && typed() == QStringLiteral("and this") &&
                view->property("notice").toString().contains(QStringLiteral("buildbox")),
            "a failed copy keeps the message and says why");
    call("detach", {second});
    call("send");
    require(sent().size() == 2 && !sent().last().contains(second), "without it, it sends");

    // The bar is shared between agents: a file attached for an agent here
    // never goes to the remote one, nor a copy there to an agent here.
    root->setProperty("remote", false);
    const auto here = QStringLiteral("/Users/someone/Desktop/local.txt");
    call("attach", {here});
    root->setProperty("remote", true);
    input->setProperty("text", QStringLiteral("for the remote one"));
    call("send");
    require(sent().size() == 2, "a file here is never mentioned to the remote agent");
    call("detach", {here});
    call("attach", {mac});
    call("uploadFinished", {QStringLiteral("t3"), there, QString()});
    root->setProperty("remote", false);
    input->setProperty("text", QStringLiteral("for the local one"));
    call("send");
    require(sent().size() == 2, "a copy there is never mentioned to an agent here");
}

// The mirrors' disk budget removes the least recently shown conversations,
// never the one followed.
void remoteCacheKeepsItsBudget() {
    RemoteFixture fixture;
    auto& workspace = *fixture.workspace;
    const auto id = fixture.agent(QStringLiteral("budget"));
    const auto conversation = workspace.agentConversation(id);
    writeFile(fixture.transcript(conversation), records(1, 4));
    View view{workspace, id};
    require(waitFor([&] { return view.show() == fixture.cache(conversation); }, 10000),
            "the followed thread is cached");
    const auto folder = fixture.root.filePath(QStringLiteral("runtime/remote/buildbox"));
    const auto old_one = folder + QStringLiteral("/0ld00000-0000-4000-8000-000000000001.jsonl");
    const auto newer = folder + QStringLiteral("/0ld00000-0000-4000-8000-000000000002.jsonl");
    writeFile(old_one, QByteArray(qsizetype{700} * 1024, 'o'));
    writeFile(newer, QByteArray(qsizetype{700} * 1024, 'n'));
    QFile(old_one).setFileTime(QDateTime::currentDateTime().addDays(-2),
                               QFileDevice::FileModificationTime);
    QFile(newer).setFileTime(QDateTime::currentDateTime().addDays(-1),
                             QFileDevice::FileModificationTime);
    workspace.setRemoteCacheMB(1);
    require(!QFileInfo::exists(old_one) && QFileInfo::exists(newer) &&
                QFileInfo::exists(fixture.cache(conversation)),
            "the least recently shown goes first, until the cache fits");
    workspace.setRemoteCacheMB(0);
    require(!QFileInfo::exists(newer) && QFileInfo::exists(fixture.cache(conversation)),
            "the followed conversation is never removed");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    try {
        const auto only = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
        const std::vector<std::pair<const char*, void (*)()>> cases{
            {"identity", remoteClaudeFollowsItsRelayedSession},
            {"follow", remoteThreadFollowsTheFile},
            {"clear", remoteThreadFollowsClear},
            {"tail", remoteThreadShowsALargeFileTailFirst},
            {"failures", remoteThreadFailuresSayWhy},
            {"attachments", remoteAttachmentsUploadFirst},
            {"cache", remoteCacheKeepsItsBudget},
            {"composer", composerMentionsOnlyRemoteCopies},
        };
        for (const auto& [name, run] : cases) {
            if (!only.isEmpty() && only != QLatin1String(name))
                continue;
            std::cout << "remote-thread " << name << '\n';
            run();
        }
        std::cout << "remote-thread: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "remote-thread: " << error.what() << '\n';
        return 1;
    }
}
