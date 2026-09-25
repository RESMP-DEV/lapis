#ifndef LAPIS_DESKTOP_SHELL_ENVIRONMENT_HPP
#define LAPIS_DESKTOP_SHELL_ENVIRONMENT_HPP
#include <QByteArray>
#include <QList>
#include <utility>

namespace lapis::desktop {
// The login shell prints this line before its environment.
inline constexpr char kEnvironmentMarker[] = "lapis-login-environment";

// The name=value pairs `env -0` printed after a line holding only
// kEnvironmentMarker (a shell's startup files may print first). A shell's own
// bookkeeping (PWD, SHLVL, _) and TERM, which each terminal sets for itself,
// are left out.
[[nodiscard]] QList<std::pair<QByteArray, QByteArray>> parse_environment(const QByteArray& output);

// Opened from Finder, the Dock or a LaunchAgent, lapis on macOS is launchd's
// child with launchd's bare environment: a PATH of system folders and none of
// the variables the person's shell exports, so agents would miss their tools
// and keys. Then take the environment of the person's interactive login
// shell, as a terminal would. Started any other way (a terminal, a script, a
// test), lapis keeps the environment it was given.
void adopt_login_environment();
} // namespace lapis::desktop
#endif
