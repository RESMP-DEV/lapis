#ifndef LAPIS_DESKTOP_PROCESS_FILES_HPP
#define LAPIS_DESKTOP_PROCESS_FILES_HPP
#include <QList>
#include <QStringList>

namespace lapis::desktop::platform {
// Is process `pid` running (it may belong to this user only)?
[[nodiscard]] bool process_alive(qint64 pid);
// `pid` and its descendants, parents first, at most `depth` levels below it
// and `limit` processes in all. Empty when `pid` is not running.
[[nodiscard]] QList<qint64> process_tree(qint64 pid, int depth = 4, int limit = 64);
// The paths of the files and folders process `pid` holds open, as the
// system resolves them. Empty when the process is gone or not this user's,
// and on platforms without a way to list them (only macOS and Linux have
// one).
[[nodiscard]] QStringList open_files(qint64 pid);
} // namespace lapis::desktop::platform
#endif
