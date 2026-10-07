// AgentStatePublisher writes what the window knows about each agent to
// runtime/agent_state.json for other local apps (Ultra Tab): owner-only,
// replaced atomically, rate-limited publish-to-publish, and only on change.
#include "agent_state.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
    require(clock.elapsed() < 300, "the first state is not held for the interval");
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

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    try {
        publishesEachAgentPrivately();
    } catch (const std::exception& error) {
        std::cerr << "agent state test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "agent state tests passed\n";
    return 0;
}
