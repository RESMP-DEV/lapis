#ifndef LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#define LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#include <QByteArray>
#include <QtGlobal>
class QQuickWindow;
namespace lapis::desktop {
#ifdef Q_OS_MACOS
[[nodiscard]] bool system_reduced_motion();
void style_window_chrome(QQuickWindow& window);
// Plays a WAV through the system's output, without waiting for it to end.
void play_sound(const QByteArray& wav);
#else
// Linux desktop settings integration remains unqualified; manual preview works.
inline bool system_reduced_motion() { return false; }
inline void style_window_chrome(QQuickWindow&) {}
// Alert sounds are macOS-only for now.
inline void play_sound(const QByteArray&) {}
#endif
} // namespace lapis::desktop
#endif
