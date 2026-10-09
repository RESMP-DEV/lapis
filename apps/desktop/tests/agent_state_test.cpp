// AgentStatePublisher writes what the window knows about each agent to
// runtime/agent_state.json for other local apps (Ultra Tab): owner-only,
// replaced atomically, rate-limited publish-to-publish, and only on change.
#include "agent_state.hpp"
#include "open_request.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <iostream>
#include <stdexcept>

using lapis::desktop::AgentStatePublisher;
using lapis::desktop::Workspace;
using lapis::desktop::WorkspaceMode;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()>& done, int ms = 5000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
}
QJsonObject read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject{};
}
QJsonObject agent(const QJsonObject& state, const QString& id) {
    for (const auto& value : state.value(QStringLiteral("agents")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == id)
            return value.toObject();
    return {};
}

void publishesEachAgentPrivately() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    Workspace workspace(WorkspaceMode::preview);
    require(workspace.selectSession(QStringLiteral("renderer")), "show the renderer");
    const auto path = directory.filePath(QStringLiteral("agent_state.json"));
    QElapsedTimer clock;
    clock.start();
    AgentStatePublisher publisher(workspace, nullptr, path, 400);
    require(waitFor([&] { return publisher.writes() == 1; }), "the first state is published");
    require(clock.elapsed() < 380, "the first state is not held for the interval");
    publisher.waitForWrites();
    const auto state = read(path);
    require(state.value(QStringLiteral("version")).toInt() == AgentStatePublisher::version &&
                state.value(QStringLiteral("pid")).toInteger() ==
                    QCoreApplication::applicationPid() &&
                state.value(QStringLiteral("publishedAtMs")).toInteger() > 0,
            "versioned, with the writer and the time");
    require(state.value(QStringLiteral("agents")).toArray().size() == workspace.sessions().size(),
            "every agent, in workspace order");
    const auto quiet = agent(state, QStringLiteral("agent"));
    require(quiet.value(QStringLiteral("requests")).toInt() == 0 &&
                !quiet.contains(QStringLiteral("request")) &&
                !quiet.contains(QStringLiteral("offer")) &&
                !quiet.value(QStringLiteral("status")).toString().isEmpty(),
            "status without requests or an offer");
    require(QFile::permissions(path) ==
                (QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser),
            "owner-only");

    // A burst inside the interval is written once, at the deadline, newest.
    clock.restart();
    require(workspace.replayAttention(QStringLiteral("arrival")), "a request arrives");
    require(waitFor([&] { return publisher.writes() == 2; }), "the request is published");
    require(clock.elapsed() >= 300, "a change inside the interval waits for its deadline");
    publisher.waitForWrites();
    const auto asked = agent(read(path), QStringLiteral("agent"));
    require(asked.value(QStringLiteral("requests")).toInt() == 1 &&
                asked.value(QStringLiteral("request")).toString() ==
                    QLatin1String("Review the next step") &&
                asked.value(QStringLiteral("unseen")).toBool() &&
                asked.value(QStringLiteral("neededAtMs")).toInteger() > 0 &&
                asked.value(QStringLiteral("turnAtMs")).toInteger() > 0,
            "the request, its reason and when it began to wait");

    // A change elsewhere in the workspace that leaves every agent as it was
    // writes nothing.
    require(workspace.addCategory(QStringLiteral("Research")), "a new category");
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 700) // labeled negative interval: past one deadline
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    require(publisher.writes() == 2, "an unchanged state is not rewritten");
}
} // namespace

// Ultra Tab's request to show an agent: each fresh one opens it once; old,
// repeated, malformed and linked requests open nothing.
void followsOpenRequests() {
    QTemporaryDir directory;
    require(directory.isValid(), "request directory");
    const auto path = directory.filePath(QStringLiteral("ultratab_open.json"));
    QStringList opened;
    lapis::desktop::OpenRequests requests(path,
                                          [&opened](const QString& id) { opened.append(id); });
    const auto id = QStringLiteral("00000000-0000-4000-8000-000000000001");
    const auto put = [&path](const QJsonObject& object) {
        QSaveFile file(path);
        require(file.open(QIODevice::WriteOnly), "write a request");
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        require(file.commit(), "commit the request");
    };
    const auto now = [] { return QDateTime::currentMSecsSinceEpoch(); };
    put({{QStringLiteral("agent"), id}, {QStringLiteral("atMs"), static_cast<double>(now())}});
    require(waitFor([&] { return opened.size() == 1; }) && opened.front() == id,
            "a fresh request opens its agent");
    requests.check();
    require(opened.size() == 1, "the same request opens it once");
    put({{QStringLiteral("agent"), id},
         {QStringLiteral("atMs"), static_cast<double>(now() - 60000)}});
    requests.check();
    put({{QStringLiteral("agent"), QStringLiteral("not an id")},
         {QStringLiteral("atMs"), static_cast<double>(now() + 10)}});
    requests.check();
    require(opened.size() == 1, "stale and malformed requests open nothing");

    // A request that only has to be rejected for being linked, not for its
    // age, so the symlink guard itself is what rejects it.
    const auto linked = directory.filePath(QStringLiteral("linked.json"));
    require(QFile::remove(path) || !QFile::exists(path), "clear the request file");
    require(QFile::link(directory.filePath(QStringLiteral("missing.json")), linked),
            "make a symlinked request");
    put({});
    require(QFile::remove(linked), "clear the symlink");
    require(QFile::link(path, linked), "link the fresh request");
    requests.check();
    require(opened.size() == 1, "a linked request opens nothing");
    require(QFile::remove(linked), "remove the symlink");

    // An out-of-range or non-finite timestamp is external input: it must be
    // refused, never cast.
    const QByteArray original =
        QByteArrayLiteral("{\"agent\":\"") + id.toUtf8() + QByteArrayLiteral("\",\"atMs\":1e300}");
    require(QFile::remove(path), "clear the request file");
    QFile huge(path);
    require(huge.open(QIODevice::WriteOnly), "write an out-of-range request");
    huge.write(original);
    huge.close();
    requests.check();
    require(opened.size() == 1, "an out-of-range timestamp opens nothing");
    require(QFile::remove(path), "clear the out-of-range request");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    try {
        publishesEachAgentPrivately();
        followsOpenRequests();
    } catch (const std::exception& error) {
        std::cerr << "agent state test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "agent state tests passed\n";
    return 0;
}
