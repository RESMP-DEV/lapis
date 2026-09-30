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
inline constexpr std::array<QStringView, 8> relay_identity_fields{
    QStringView{u"hook_event_name"}, QStringView{u"notification_type"}, QStringView{u"prompt_id"},
    QStringView{u"reason"},          QStringView{u"session_id"},        QStringView{u"source"},
    QStringView{u"tool_name"},       QStringView{u"tool_use_id"},
};
inline constexpr std::array<QStringView, 1> relay_derived_fields{
    QStringView{u"in_flight"},
};

int run_hook_relay(const QString& socket, const QString& nonce) noexcept;

} // namespace lapis::claude

#endif
