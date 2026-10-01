#ifndef LAPIS_DESKTOP_ALERTS_HPP
#define LAPIS_DESKTOP_ALERTS_HPP
#include "chime_sounds.hpp"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <vector>

namespace lapis::desktop {
class KeyMap;
class SessionPreview;
class Workspace;

// What the person last saw of each agent: while the window is active, the
// screen of the agent shown is sampled every second. A finished turn whose
// screen is still what was seen there says nothing new, so it neither chimes
// nor notifies. Claude Code's input box and status line (below its last
// box-drawing rule) are left out, since a status line can tick on its own.
class SeenScreens final : public QObject {
    Q_OBJECT
  public:
    using Looking = std::function<bool(const SessionPreview*)>;
    static constexpr int kSampleMs = 1000;
    SeenScreens(Workspace& workspace, Looking looking, QObject* parent = nullptr);
    // Whether `item` shows what the person last saw of it.
    [[nodiscard]] bool unchanged(const SessionPreview* item) const;
    // Records what is on `item`'s screen now as seen.
    void see(const SessionPreview* item);
    [[nodiscard]] static size_t fingerprint(const SessionPreview* item);

  private:
    void sample();
    Workspace& workspace_;
    Looking looking_;
    QTimer timer_;
    QHash<const SessionPreview*, size_t> seen_;
};

// One line per ping decision in an owner-only log, so a missed or extra ping
// can be traced: the agent, the moment, and what was done and why.
using AttentionLog = std::function<void(const QJsonObject&)>;

// The writer for that log: one JSON line per call, owner-only at `path`.
// It rotates beside its predecessor before a line would cross 2 MiB, opens
// the fresh file with a marker line, and stops the write with a warning when
// the log cannot be made private or the old file cannot move aside.
[[nodiscard]] AttentionLog attention_log(const QString& path);

// Production requests and completed turns share one finished cue. The legacy
// explicit needsYou signal retains its repeat behavior for existing callers;
// it is not re-enabled by custom sound files.
class Alerts final : public QObject {
    Q_OBJECT
  public:
    using Player = std::function<void(Chime)>;
    // Whether the person is looking at this agent right now.
    using Looking = std::function<bool(const SessionPreview*)>;
    static constexpr int kRepeatMs = 4000;
    static constexpr int kQuietMs = 1500;

    Alerts(Workspace& workspace, const KeyMap& config, Player play, Looking looking,
           QObject* parent = nullptr);
    // A finished turn showing what was already seen stays quiet; every
    // decision is logged.
    void setSeen(SeenScreens* seen) { seen_ = seen; }
    void setLog(AttentionLog log) { log_ = std::move(log); }

    // Plays a chime now, for the settings' Play buttons.
    Q_INVOKABLE void preview(bool needsYou);
    struct Timing {
        int repeatMs;
        int quietMs;
    };
    void setTimingForTesting(Timing timing) {
        repeat_.setInterval(timing.repeatMs);
        quiet_ms_ = timing.quietMs;
    }

  private:
    void needsYou(SessionPreview* item);
    void finished(SessionPreview* item);
    void tick();
    bool ring(Chime chime);
    struct Waiting {
        QPointer<SessionPreview> item;
        int played{};
    };
    void record(const SessionPreview* item, const char* event, const char* decision) const;
    const KeyMap& config_;
    Player play_;
    Looking looking_;
    SeenScreens* seen_{};
    AttentionLog log_;
    std::vector<Waiting> waiting_;
    QTimer repeat_;
    QElapsedTimer last_;
    int quiet_ms_{kQuietMs};
};
// A system notification when an agent needs you or finishes a turn while
// lapis is in the background (the chime's moments, when you are elsewhere).
// Clicking one shows that agent; `post` gets the agent's id for that.
class Notifier final : public QObject {
    Q_OBJECT
  public:
    using Post = std::function<void(const QString& id, const QString& title, const QString& body)>;
    using Background = std::function<bool()>;
    Notifier(Workspace& workspace, const KeyMap& config, Post post, Background background,
             QObject* parent = nullptr);
    void setSeen(const SeenScreens* seen) { seen_ = seen; }
    void setLog(AttentionLog log) { log_ = std::move(log); }

  private:
    void notify(const SessionPreview* item, bool needsYou);
    const SeenScreens* seen_{};
    AttentionLog log_;
    const KeyMap& config_;
    Post post_;
    Background background_;
};
} // namespace lapis::desktop
#endif
