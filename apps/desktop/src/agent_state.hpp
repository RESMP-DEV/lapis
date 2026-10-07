#ifndef LAPIS_DESKTOP_AGENT_STATE_HPP
#define LAPIS_DESKTOP_AGENT_STATE_HPP
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>

namespace lapis::desktop {
class NextPrompt;
class Workspace;

// Publishes what the window knows about each agent and nothing else reads
// from disk, for other local apps that show the same agents (Ultra Tab,
// apps/ultratab): `runtime/agent_state.json` beside the workspace registry.
// Read-only toward lapis: nothing in lapis reads the file back.
//
// Version 1: {"version": 1, "pid": <this window>, "publishedAtMs": ...,
// "agents": [{"id", "status" (SessionPreview::statusKind), "unseen",
// "requests" (pending count), "request" (the first one's reason),
// "neededAtMs", "turnAtMs" (the last finished turn or request, this window's
// clock; 0 before one), "offer": {"key", "text", "seen", "said"} when a
// next-prompt guess is shown}]}, in workspace order.
//
// Owner-only (0600), replaced atomically by one ordered writer thread, only
// when the content changes. Rate-limited publish-to-publish: the first change
// after `interval_ms` of quiet is written on the next event loop turn; changes
// sooner are coalesced and the newest state is written at the deadline.
class AgentStatePublisher final : public QObject {
    Q_OBJECT
  public:
    // `next` may be null (suggestions unavailable in this window); both are
    // borrowed and must outlive the publisher.
    AgentStatePublisher(Workspace& workspace, NextPrompt* next, QString path, int interval_ms = 250,
                        QObject* parent = nullptr);
    ~AgentStatePublisher() override;
    AgentStatePublisher(const AgentStatePublisher&) = delete;
    AgentStatePublisher& operator=(const AgentStatePublisher&) = delete;

    static constexpr int version = 1;
    // The state as it would be written now, without the publishedAtMs that
    // only a changed state gets when it is written.
    [[nodiscard]] QByteArray document() const;
    // Writes are asynchronous; tests wait for them here.
    void waitForWrites();
    [[nodiscard]] const QString& path() const { return path_; }
    // Documents handed to the writer so far (unchanged states are skipped).
    [[nodiscard]] int writes() const { return writes_; }

  private:
    [[nodiscard]] QJsonObject state() const;
    void changed();
    void publish();
    void watchSessions();
    Workspace& workspace_;
    QPointer<NextPrompt> next_;
    QString path_;
    int interval_ms_;
    QTimer timer_;
    QElapsedTimer since_publish_;
    QByteArray last_;
    QHash<QString, qint64> turn_at_ms_;
    int writes_{};
    // One thread so replacements land in order.
    QThreadPool writer_;
};
} // namespace lapis::desktop
#endif
