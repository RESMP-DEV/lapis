#ifndef LAPIS_SESSION_POSIX_LOCAL_ENDPOINT_HPP
#define LAPIS_SESSION_POSIX_LOCAL_ENDPOINT_HPP
#include <QString>
#include <QtGlobal>
namespace lapis::session::posix {
// Create a private directory if absent; reject shared or foreign-owned parents.
// Existing directories are never chmodded. Returns an absolute socket path.
[[nodiscard]] QString prepare_endpoint(const QString& endpoint);
// Canonicalize an existing private directory and validate its trusted ancestors.
// This contract intentionally has no AF_UNIX length or socket-file constraints.
[[nodiscard]] QString canonical_trusted_directory(const QString& directory);
// A whole screen (about 130 KB) in one write: local sockets default to 8 KB
// buffers on macOS, which split a screen into a round trip per 8 KB through
// both event loops, about 25 ms. Best effort; a refused size keeps the default.
void widen_socket_buffers(qintptr descriptor);
} // namespace lapis::session::posix
#endif
