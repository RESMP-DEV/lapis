#ifndef LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#define LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#include <QByteArray>
#include <QtGlobal>
class QQuickWindow;
namespace lapis::desktop {
#ifdef Q_OS_MACOS
[[nodiscard]] bool system_reduced_motion();
void style_window_chrome(QQuickWindow& window);
// Plays a sound (WAV, or any file format the system reads) through the
// system's output at a volume from 0 to 1, without waiting for it to end.
void play_sound(const QByteArray& sound, float volume = 1.0F);
#else
// Linux desktop settings integration remains unqualified; manual preview works.
inline bool system_reduced_motion() { return false; }
inline void style_window_chrome(QQuickWindow&) {}
// Alert sounds are macOS-only for now.
inline void play_sound(const QByteArray&, float = 1.0F) {}
#endif
} // namespace lapis::desktop
#endif
