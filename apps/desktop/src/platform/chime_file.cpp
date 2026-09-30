#include "chime_file.hpp"
#include "platform/posix/unique_fd.hpp"
#include <QDataStream>
#include <QFile>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>

namespace lapis::desktop::platform {
namespace {
QByteArray fileStamp(const struct stat& info) {
    QByteArray stamp;
    QDataStream out(&stamp, QIODevice::WriteOnly);
    out << quint64(info.st_dev) << quint64(info.st_ino) << qint64(info.st_size);
#ifdef Q_OS_MACOS
    out << qint64(info.st_mtimespec.tv_sec) << qint64(info.st_mtimespec.tv_nsec)
        << qint64(info.st_ctimespec.tv_sec) << qint64(info.st_ctimespec.tv_nsec);
#else
    out << qint64(info.st_mtim.tv_sec) << qint64(info.st_mtim.tv_nsec)
        << qint64(info.st_ctim.tv_sec) << qint64(info.st_ctim.tv_nsec);
#endif
    return stamp;
}
ChimeFile failed(const QString& message) { return {{}, {}, message}; }
} // namespace
ChimeFile read_chime_file(const QString& path, const ChimeFile& previous, qint64 limit) {
    const auto encoded = QFile::encodeName(path);
    session::posix::UniqueFd fd(::open(encoded.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!fd)
        return failed(errno == ENOENT ? QStringLiteral("not found")
                                      : QStringLiteral("not readable"));
    struct stat before{};
    if (::fstat(fd.get(), &before) != 0)
        return failed(QStringLiteral("could not inspect file"));
    if (!S_ISREG(before.st_mode))
        return failed(QStringLiteral("not a regular file"));
    if (before.st_size <= 0)
        return failed(QStringLiteral("empty file"));
    if (limit <= 0 || before.st_size > limit)
        return failed(QStringLiteral("over 4 MiB"));
    const auto stamp = fileStamp(before);
    if (!previous.bytes.isEmpty() && previous.stamp == stamp)
        return previous;
    QFile file;
    if (!file.open(fd.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        return failed(QStringLiteral("not readable"));
    const auto bytes = file.read(static_cast<qint64>(before.st_size) + 1);
    struct stat after{};
    if (bytes.size() != before.st_size || ::fstat(fd.get(), &after) != 0 ||
        fileStamp(after) != stamp)
        return failed(QStringLiteral("changed during read; retrying"));
    return {bytes, stamp, {}};
}
} // namespace lapis::desktop::platform
