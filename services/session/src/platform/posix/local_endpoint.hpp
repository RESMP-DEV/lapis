#ifndef LAPIS_SESSION_POSIX_LOCAL_ENDPOINT_HPP
#define LAPIS_SESSION_POSIX_LOCAL_ENDPOINT_HPP
#include <QString>
namespace lapis::session::posix {
// Create a private directory if absent; reject shared or foreign-owned parents.
// Existing directories are never chmodded. Returns an absolute socket path.
[[nodiscard]] QString prepare_endpoint(const QString& endpoint);
} // namespace lapis::session::posix
#endif
