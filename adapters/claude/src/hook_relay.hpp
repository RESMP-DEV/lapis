#ifndef LAPIS_CLAUDE_HOOK_RELAY_HPP
#define LAPIS_CLAUDE_HOOK_RELAY_HPP

#include <QString>

namespace lapis::claude {

int run_hook_relay(const QString& socket, const QString& nonce) noexcept;

} // namespace lapis::claude

#endif
