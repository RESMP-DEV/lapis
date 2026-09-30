#ifndef LAPIS_DESKTOP_CHIME_SOUNDS_HPP
#define LAPIS_DESKTOP_CHIME_SOUNDS_HPP
#include <QByteArray>
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>

namespace lapis::desktop {
class KeyMap;
enum class Chime : std::uint8_t { needsYou, finished };

// A chime as 16-bit mono WAV, synthesized so lapis ships no audio files: two
// glassy taps, rising (E6 then A6) when an agent needs you, falling and
// quieter when a turn has ended.
[[nodiscard]] QByteArray chime_wav(Chime chime);

// What a chime attempts to play: synthesized WAV bytes or configured file bytes,
// at a volume from 0 to 1.
struct ChimeSound {
    QByteArray bytes;
    float volume{1.0F};
    // Identifies the file and its stamp for the platform cache; empty means
    // the synthesized chime.
    QString cacheKey;
    QString path;
};

// GUI-owned sound selection with at most two background file checks. The GUI
// uses the latest completed bytes or the synthesized fallback without waiting.
class ChimeSounds {
  public:
    using Player = std::function<bool(const ChimeSound&)>;
    static constexpr qint64 kMaxFileBytes = qint64{4} * 1024 * 1024;
    ChimeSounds();
    ~ChimeSounds();
    ChimeSounds(const ChimeSounds&) = delete;
    ChimeSounds& operator=(const ChimeSounds&) = delete;
    void configure(KeyMap& config);
    [[nodiscard]] ChimeSound sound(Chime chime, KeyMap& config);
    void play(Chime chime, KeyMap& config, const Player& player);

  private:
    struct State;
    static void pump(const std::shared_ptr<State>& state);
    // Called with State::mutex held; no GUI object is read on a worker thread.
    static void reportLocked(const std::shared_ptr<State>& state);
    QObject dispatcher_;
    std::shared_ptr<State> state_;
};

} // namespace lapis::desktop
#endif
