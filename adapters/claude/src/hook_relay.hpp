#ifndef LAPIS_CLAUDE_HOOK_RELAY_HPP
#define LAPIS_CLAUDE_HOOK_RELAY_HPP

#include <QString>
#include <QStringView>
#include <array>

namespace lapis::claude {

inline constexpr int relay_deadline_ms = 1000;
inline constexpr qsizetype relay_frame_limit = qsizetype{16} * 1024;

// Shared metadata contract. Source fields may be copied from Claude; derived
// fields are accepted by the authenticated observer but computed only by the
// relay. Possession of the private socket nonce remains the trust boundary;
// field placement is not authentication. Both lists retain the existing event
// wire shape. `legacy` means omitted lists; `unknown` means schema drift.
inline constexpr QStringView relay_in_flight_field{u"in_flight"};
inline constexpr std::array<QStringView, 8> relay_identity_fields{
    QStringView{u"hook_event_name"}, QStringView{u"notification_type"}, QStringView{u"prompt_id"},
    QStringView{u"reason"},          QStringView{u"session_id"},        QStringView{u"source"},
    QStringView{u"tool_name"},       QStringView{u"tool_use_id"},
};
inline constexpr std::array<QStringView, 1> relay_derived_fields{relay_in_flight_field};

// The hook command names the frame shape its service reads, so a relay never
// sends a field the listening service predates. The hook runs whatever binary
// is installed at the command's path when Claude stops, which after an update
// is newer than a service that kept running; an older service treats an
// unknown field as a malformed hook and stops observing for good. A command
// without a contract (from such a service) gets identity fields only, the
// legacy shape; contract 2 adds the derived fields above.
inline constexpr QStringView relay_contract{u"2"};

int run_hook_relay(const QString& socket, const QString& nonce,
                   const QString& contract = {}) noexcept;

} // namespace lapis::claude

#endif
