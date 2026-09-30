#ifndef LAPIS_DESKTOP_CHIME_FILE_HPP
#define LAPIS_DESKTOP_CHIME_FILE_HPP
#include <QByteArray>
#include <QString>

namespace lapis::desktop::platform {
struct ChimeFile {
    QByteArray bytes;
    QByteArray stamp;
    QString error;
};
// Background-only regular-file read. Nonblocking open plus descriptor validation
// rejects a path replaced by a pipe/device without consuming a worker forever.
[[nodiscard]] ChimeFile read_chime_file(const QString& path, const ChimeFile& previous,
                                        qint64 limit);
} // namespace lapis::desktop::platform
#endif
