#ifndef LAPIS_CLAUDE_HOOK_RELAY_HPP
#define LAPIS_CLAUDE_HOOK_RELAY_HPP

#include <QByteArray>
#include <QJsonObject>
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

// The Claude Code hook events lapis registers, for a local or remote relay.
inline constexpr std::array<QStringView, 9> hook_event_names{
    QStringView{u"SessionStart"},       QStringView{u"UserPromptSubmit"},
    QStringView{u"PermissionRequest"},  QStringView{u"Notification"},
    QStringView{u"PreToolUse"},         QStringView{u"PostToolUse"},
    QStringView{u"PostToolUseFailure"}, QStringView{u"Stop"},
    QStringView{u"SessionEnd"}};

// Claude Code settings (for --settings) that run `command` for every event
// above. The command must never print a decision or exit 2.
[[nodiscard]] QByteArray hook_settings(const QString& command);

// The bounded metadata a relay forwards for one hook input: the identity
// fields above plus the derived background-work count. Both the local relay
// process and the session service (for hooks relayed from another machine
// through the terminal) use it, so the derivation has one definition.
[[nodiscard]] QJsonObject relay_event(const QJsonObject& source);

int run_hook_relay(const QString& socket, const QString& nonce) noexcept;

} // namespace lapis::claude

#endif
