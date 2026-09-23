#ifndef LAPIS_SESSION_LAUNCH_SPEC_HPP
#define LAPIS_SESSION_LAUNCH_SPEC_HPP

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <lapis/session/terminal.hpp>

namespace lapis::session {
// Internal launch contract v1. Arguments are passed literally, never interpreted
// by a shell. Initial geometry is not part of attachment identity.
enum class AgentMode : std::uint8_t { terminal, codex, claude };
struct LaunchSpec {
    QString program;
    QStringList arguments;
    QString directory;
    TerminalSize size{100, 30};
    AgentMode agent{AgentMode::terminal};
};

// Resolve the executable and working directory; reject invalid/bounded inputs.
// Throws std::invalid_argument on invalid launch requests.
[[nodiscard]] LaunchSpec validate_launch(LaunchSpec launch);
// The account's configured login shell, used when SHELL is not set in the
// environment. Falls back to /bin/sh only when no account shell can be read.
[[nodiscard]] QString login_shell();
[[nodiscard]] LaunchSpec shell_launch(const QString& directory);
// Call only with a validated launch. 32 bytes, tied to program/argv/cwd, not PID.
[[nodiscard]] QByteArray launch_fingerprint(const LaunchSpec& launch);
} // namespace lapis::session
#endif
