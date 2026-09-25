#ifndef LAPIS_DESKTOP_ALERTS_HPP
#define LAPIS_DESKTOP_ALERTS_HPP
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
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
} // namespace lapis::desktop
#endif
