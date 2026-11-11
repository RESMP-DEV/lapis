#ifndef LAPIS_DESKTOP_ALERTS_HPP
#define LAPIS_DESKTOP_ALERTS_HPP
#include "chime_sounds.hpp"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <functional>

namespace lapis::desktop {
class Config;
class Agent;
class Workspace;

// What the person last saw of each agent: while the window is active and
// someone is at the Mac (`looking`), the screen of the agent shown is sampled
// every second, and that look clears its unseen mark (Workspace::noteLooking),
// so returning to the Mac counts as looking. A finished turn whose
// screen is still what was seen there says nothing new, so it does not
// chime. Claude Code's input box and status line (below its last
// box-drawing rule) are left out, since a status line can tick on its own.
class SeenScreens final : public QObject {
    Q_OBJECT
  public:
    using Looking = std::function<bool(const Agent*)>;
    static constexpr int kSampleMs = 1000;
    SeenScreens(Workspace& workspace, Looking looking, QObject* parent = nullptr);
    // Whether `item` shows what the person last saw of it.
    [[nodiscard]] bool unchanged(const Agent* item) const;
    // Records what is on `item`'s screen now as seen.
    void see(const Agent* item);
    // The same from one launch to the next, so a restarted window still
    // knows what was seen.
    [[nodiscard]] static size_t fingerprint(const Agent* item);
    // What was seen of each agent, by agent id, for GuiState; restoring
    // skips agents that are gone.
    [[nodiscard]] QJsonObject saveState() const;
    void restoreState(const QJsonObject& state);

  signals:
    // What saveState() returns changed.
    void changed();

  private:
    void sample();
    Workspace& workspace_;
    Looking looking_;
    QTimer timer_;
    QHash<const Agent*, size_t> seen_;
};

// One line per chime decision in an owner-only log, so a missed or extra chime
// can be traced: the agent, the moment, and what was done and why.
using AttentionLog = std::function<void(const QJsonObject&)>;

// The writer for that log: one JSON line per call, owner-only at `path`.
// It rotates beside its predecessor before a line would cross 2 MiB, opens
// the fresh file with a marker line, and stops the write with a warning when
// the log cannot be made private or the old file cannot move aside.
[[nodiscard]] AttentionLog attention_log(const QString& path);

// Open requests and completed turns share one finished cue. It plays only when
// the workspace newly marks an agent unseen (Workspace::needsYou "marked"):
// once per turn or request, never while the person looks at it, never for a
// screen already seen, never again for a turn already looked at.
class Alerts final : public QObject {
    Q_OBJECT
  public:
    using Player = std::function<void(Chime)>;
    static constexpr int kQuietMs = 1500;

    Alerts(Workspace& workspace, const Config& config, Player play, QObject* parent = nullptr);
    // The screen the person is looking at as a turn ends is recorded as seen;
    // every decision is logged.
    void setSeen(SeenScreens* seen) { seen_ = seen; }
    void setLog(AttentionLog log) { log_ = std::move(log); }

    // Plays a chime now, for the settings' Play buttons.
    Q_INVOKABLE void preview(bool needsYou);
    // How long after one chime another stays quiet.
    void setQuietForTesting(int ms) { quiet_ms_ = ms; }

  private:
    void finished(Agent* item, const QString& outcome);
    bool ring(Chime chime);
    void record(const Agent* item, const QString& outcome, const char* decision) const;
    const Config& config_;
    Player play_;
    SeenScreens* seen_{};
    AttentionLog log_;
    QElapsedTimer last_;
    int quiet_ms_{kQuietMs};
};
} // namespace lapis::desktop
#endif
