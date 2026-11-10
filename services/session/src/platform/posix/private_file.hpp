#ifndef LAPIS_SESSION_PLATFORM_POSIX_PRIVATE_FILE_HPP
#define LAPIS_SESSION_PLATFORM_POSIX_PRIVATE_FILE_HPP

#include <QByteArray>
#include <QString>

namespace lapis::session::posix {

// Replaces the file at `path` with `bytes`, readable and writable by the
// owner only. The bytes go to a temporary beside the target that is created
// 0600, so no other user can open it whatever the umask, and is synced and
// renamed over the target: a reader sees the old file or the new one. A
// symbolic link is followed to the file it names and an existing target that
// is not writable is refused, as QSaveFile does. The directory must exist. On
// failure the target is untouched, the temporary is removed and `error`, when
// given, says why.
[[nodiscard]] bool write_private_file(const QString& path, const QByteArray& bytes,
                                      QString* error = nullptr);

} // namespace lapis::session::posix

#endif // LAPIS_SESSION_PLATFORM_POSIX_PRIVATE_FILE_HPP
