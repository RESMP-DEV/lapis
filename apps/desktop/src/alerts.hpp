#ifndef LAPIS_DESKTOP_ALERTS_HPP
#define LAPIS_DESKTOP_ALERTS_HPP
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
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

enum class Chime : std::uint8_t { needsYou, finished };

// A chime as 16-bit mono WAV, synthesized so lapis ships no audio files: two
// glassy taps, rising (E6 then A6) when an agent needs you, falling and
// quieter when a turn has ended.
[[nodiscard]] QByteArray chime_wav(Chime chime);

// What a chime plays: WAV bytes, or those of any sound file the system reads,
// at a volume from 0 to 1.
struct ChimeSound {
    QByteArray bytes;
    float volume{1.0F};
    // Identifies the file and its stamp for the platform cache; empty means
    // the synthesized chime.
    QString cacheKey;
};

// The configured sound files in place of the synthesized chimes: soundFile
// when an agent needs you, finishedFile when a turn ends, else soundFile at
// half volume (the taps' -12 and -18 dBFS). A file is read again only when it
// changes; one that cannot be read, or is over 4 MiB, plays the taps.
class ChimeSounds {
  public:
    static constexpr qint64 kMaxFileBytes = qint64{4} * 1024 * 1024;
    [[nodiscard]] ChimeSound sound(Chime chime, const KeyMap& config);

  private:
    struct File {
        QDateTime modified;
        qint64 size{-1};
        QByteArray bytes;
        QString cacheKey;
    };
    [[nodiscard]] ChimeSound read(const QString& path);
    QHash<QString, File> files_;
};

// When to chime. An agent that needs you (a new request) chimes at once and
// again every few seconds while the request still waits and you are not
// looking at that agent, up to the configured number of times: two taps, a
// pause, two taps, like a Dock icon bouncing. A Codex or Claude turn that ends
// out of view chimes once, quietly. At most one chime plays at a time.
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
    const KeyMap& config_;
    Player play_;
    Looking looking_;
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

  private:
    void notify(const SessionPreview* item, bool needsYou);
    const KeyMap& config_;
    Post post_;
    Background background_;
};
} // namespace lapis::desktop
#endif
