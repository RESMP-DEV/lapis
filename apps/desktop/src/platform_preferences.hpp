#ifndef LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#define LAPIS_DESKTOP_PLATFORM_PREFERENCES_HPP
#include <QtGlobal>
namespace lapis::desktop {
#ifdef Q_OS_MACOS
[[nodiscard]] bool system_reduced_motion();
#else
// Linux desktop settings integration remains unqualified; manual preview works.
inline bool system_reduced_motion() { return false; }
#endif
} // namespace lapis::desktop
#endif
