#ifndef LAPIS_CLAUDE_HOOK_RELAY_HPP
#define LAPIS_CLAUDE_HOOK_RELAY_HPP

#include <QString>
#include <QStringView>
#include <array>

namespace lapis::claude {

inline constexpr qsizetype relay_frame_limit = qsizetype{16} * 1024;

// The relay and observer form one authenticated metadata wire contract. Keep the
// frame bound and forwarded identity fields together so neither side can drift.
inline constexpr std::array<QStringView, 8> relay_identity_fields{
    QStringView{u"hook_event_name"}, QStringView{u"session_id"},  QStringView{u"prompt_id"},
    QStringView{u"tool_name"},       QStringView{u"tool_use_id"}, QStringView{u"notification_type"},
    QStringView{u"source"},          QStringView{u"reason"},
};

int run_hook_relay(const QString& socket, const QString& nonce) noexcept;

} // namespace lapis::claude

#endif
