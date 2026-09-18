#ifndef LAPIS_SESSION_DESCRIPTOR_HPP
#define LAPIS_SESSION_DESCRIPTOR_HPP
#include "transport/local_protocol.hpp"
#include <optional>
namespace lapis::session {
// Bounded owner-only hint at endpoint + ".session". Not proof of liveness.
// Absence returns nullopt; malformed, unsafe or mismatched files throw.
[[nodiscard]] std::optional<wire::SessionIdentity> read_descriptor(const QString& endpoint,
                                                                   const QByteArray& fingerprint);
// Atomic 0600 replacement in an already validated private endpoint parent.
// Refuse links/nonregular/foreign or insecure existing files, even during discovery.
void write_descriptor(const QString& endpoint, const QByteArray& fingerprint,
                      const wire::SessionIdentity& identity);
} // namespace lapis::session
#endif
