#ifndef LAPIS_DESKTOP_RESET_JOURNAL_HPP
#define LAPIS_DESKTOP_RESET_JOURNAL_HPP

#include <QJsonObject>
#include <QString>
#include <cstdint>

namespace lapis::desktop::platform {
inline constexpr int reset_journal_version = 2;
inline constexpr qint64 reset_journal_max_bytes = 65536;
inline constexpr int reset_journal_max_files = 256;
inline constexpr int reset_journal_max_attempts = 128;
enum class JournalWrite : std::uint8_t { saved, busy, full, failed };
// The caller holds the target's process lock. New-file admission is serialized
// across hosts, reclaiming only expired settled journals. Synchronizes the
// private file and parent directory; run off the GUI thread.
[[nodiscard]] JournalWrite write_reset_journal(const QString& path, const QJsonObject& state);
} // namespace lapis::desktop::platform
#endif
