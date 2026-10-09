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
#include <optional>
#include <vector>

namespace lapis::desktop {
class KeyMap;
class SessionPreview;
class Workspace;

// What the person last saw of each agent: while the window is active and
// someone is at the Mac (`looking`), the screen of the agent shown is sampled
// every second. A finished turn whose
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

// Holds each finished turn until the next-prompt model has judged it
// ("needs", "steer" or "fyi", NextPrompt::judged), at most kWaitMs, then
// passes it on with that judgement; empty when none came. A turn ending on an
// open request needs the person and passes at once, as does every turn while
// alerts.judge is off.
class PingJudge final : public QObject {
    Q_OBJECT
  public:
    static constexpr int kWaitMs = 25000;
    PingJudge(Workspace& workspace, const KeyMap& config, QObject* parent = nullptr);
    void verdict(const QString& id, const QString& attention);
    void setWaitForTesting(int ms) { wait_ms_ = ms; }
    // Whether a judgement will come at all (guessing is on); without one,
    // turns pass at once rather than wait out kWaitMs.
    void setActive(std::function<bool()> active) { active_ = std::move(active); }

  signals:
    void turnJudged(SessionPreview* item, const QString& attention);

  private:
    void hold(SessionPreview* item);
    void release(const QString& id, const QString& attention);
    struct Held {
        QPointer<SessionPreview> item;
        QPointer<QTimer> timer;
    };
    const KeyMap& config_;
    QHash<QString, Held> held_;
    int wait_ms_{kWaitMs};
    std::function<bool()> active_;
};

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

    // Finished turns come judged from here instead: one judged "steer" or
    // "fyi" stays quiet.
    void judgeBy(PingJudge& judge);
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
    QMetaObject::Connection finished_;
};
// A system notification when an agent needs you or finishes a turn while you
// are not watching lapis: it is in the background, or you are away from the
// Mac (no keyboard or mouse input for alerts.awayAfter seconds), which in
// front, showing that very agent, still counts as not seeing it. Clicking one
// shows that agent; `post` gets the agent's id for that. An agent left
// waiting, neither looked at nor started on a new turn, notifies once more
// after alerts.remindAfter minutes, or as soon as you are back if you were
// away then.
class Notifier final : public QObject {
    Q_OBJECT
  public:
    using Post = std::function<void(const QString& id, const QString& title, const QString& body)>;
    using Background = std::function<bool()>;
    // Whether the person is at the Mac now: recent input anywhere.
    using Present = std::function<bool()>;
    // Whether the person is looking at this agent now (present included).
    using Looking = std::function<bool(const SessionPreview*)>;
    static constexpr int kCheckMs = 5000;
    Notifier(Workspace& workspace, const KeyMap& config, Post post, Background background,
             QObject* parent = nullptr);
    // The same record the chime uses: a finished turn whose screen is still
    // what was seen there is answered, even after the person moves on.
    void setSeen(const SeenScreens* seen) { seen_ = seen; }
    void setLog(AttentionLog log) { log_ = std::move(log); }
    // Without these the person always counts as present, so a reminder
    // falling due does not wait for them; it still follows remindAfter.
    void setPresence(Present present, Looking looking);
    // Finished turns come judged from here instead (see Alerts::judgeBy).
    void judgeBy(PingJudge& judge);
    struct Timing {
        int checkMs;
        qint64 remindMs;
    };
    void setTimingForTesting(Timing timing);

  private:
    struct Waiting {
        QPointer<SessionPreview> item;
        qint64 since{};
        bool needsYou{};
    };
    void notify(SessionPreview* item, bool needsYou);
    void wait(SessionPreview* item, bool needsYou);
    void check();
    [[nodiscard]] bool present() const { return !present_ || present_(); }
    [[nodiscard]] qint64 remindMs() const;
    void post(const SessionPreview* item, const QString& body);
    const SeenScreens* seen_{};
    AttentionLog log_;
    const KeyMap& config_;
    Post post_;
    Background background_;
    Present present_;
    Looking looking_;
    std::vector<Waiting> waiting_;
    QTimer check_;
    std::optional<qint64> remind_ms_;
    QMetaObject::Connection finished_;
};
} // namespace lapis::desktop
#endif
