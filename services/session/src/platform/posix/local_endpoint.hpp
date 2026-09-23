#ifndef LAPIS_SESSION_POSIX_LOCAL_ENDPOINT_HPP
#define LAPIS_SESSION_POSIX_LOCAL_ENDPOINT_HPP
#include <QString>
namespace lapis::session::posix {
// Create a private directory if absent; reject shared or foreign-owned parents.
// Existing directories are never chmodded. Returns an absolute socket path.
[[nodiscard]] QString prepare_endpoint(const QString& endpoint);
// Canonicalize an existing private directory and validate its trusted ancestors.
// This contract intentionally has no AF_UNIX length or socket-file constraints.
[[nodiscard]] QString canonical_trusted_directory(const QString& directory);
} // namespace lapis::session::posix
#endif
