#ifndef LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#define LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#include <QByteArray>
#include <QString>
#include <QtGlobal>
class QQuickWindow;
namespace lapis::desktop {
struct ChimeSound;
#ifdef Q_OS_MACOS
[[nodiscard]] bool system_reduced_motion();
void style_window_chrome(QQuickWindow& window);
// Plays a sound (WAV, or any file format NSSound reads) through the system's
// output at a volume from 0 to 1, without waiting for it to end. A non-empty
// cache key bounds one sound per file version; false means it did not decode.
[[nodiscard]] bool play_sound(const ChimeSound& sound);
#else
// Linux desktop settings integration remains unqualified; manual preview works.
inline bool system_reduced_motion() { return false; }
inline void style_window_chrome(QQuickWindow&) {}
// Alert sounds are macOS-only for now.
inline bool play_sound(const ChimeSound&) { return false; }
#endif
} // namespace lapis::desktop
#endif
