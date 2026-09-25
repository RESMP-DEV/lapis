#ifndef LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#define LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#include <QtGlobal>
class QQuickWindow;
namespace lapis::desktop {
#ifdef Q_OS_MACOS
[[nodiscard]] bool system_reduced_motion();
void style_window_chrome(QQuickWindow& window);
#else
// Linux desktop settings integration remains unqualified; manual preview works.
inline bool system_reduced_motion() { return false; }
inline void style_window_chrome(QQuickWindow&) {}
#endif
} // namespace lapis::desktop
#endif
