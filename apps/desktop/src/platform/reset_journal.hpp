#ifndef LAPIS_DESKTOP_RESET_JOURNAL_HPP
#define LAPIS_DESKTOP_RESET_JOURNAL_HPP

#include <QJsonObject>
#include <QString>

namespace lapis::desktop::platform {
// Writes and synchronizes the private file and its parent directory. Run off
// the GUI thread; failure never authorizes a provider consume operation.
[[nodiscard]] bool write_reset_journal(const QString& path, const QJsonObject& state);
} // namespace lapis::desktop::platform
#endif
