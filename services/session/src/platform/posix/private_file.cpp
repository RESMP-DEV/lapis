#include "platform/posix/private_file.hpp"

#include "platform/posix/unique_fd.hpp"

#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lapis::session::posix {
namespace {
bool fail(QString* error, const QString& message) {
    if (error != nullptr)
        *error = message;
    return false;
}

// The file a chain of symbolic links ends at, resolved as QSaveFile does so
// the link itself is kept; a chain that does not end replaces the path.
QString final_path(const QString& path) {
    QFileInfo info(path);
    int depth = 128;
    while (--depth > 0 && info.isSymLink())
        info.setFile(info.symLinkTarget());
    return depth > 0 ? info.filePath() : path;
}

bool write_all(int descriptor, const QByteArray& bytes) {
    const char* next = bytes.constData();
    auto left = static_cast<std::size_t>(bytes.size());
    while (left > 0) {
        const auto written = ::write(descriptor, next, left);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        next += written;
        left -= static_cast<std::size_t>(written);
    }
    return true;
}
} // namespace

bool write_private_file(const QString& path, const QByteArray& bytes, QString* error) {
    const auto target = final_path(path);
    const auto name = QFile::encodeName(target);
    if (::access(name.constData(), F_OK) == 0 && ::access(name.constData(), W_OK) != 0)
        return fail(error, QStringLiteral("Existing file %1 is not writable").arg(target));
    // mkostemp creates the file 0600; nothing else ever sees it wider.
    QByteArray temporary = name + ".XXXXXX";
    UniqueFd descriptor(::mkostemp(temporary.data(), O_CLOEXEC));
    if (!descriptor)
        return fail(error, QStringLiteral("Cannot create a temporary file for %1: %2")
                               .arg(target, qt_error_string(errno)));
    const auto discard = [&](const char* step) {
        const int code = errno;
        ::unlink(temporary.constData());
        return fail(error, QStringLiteral("Cannot %1 %2: %3")
                               .arg(QLatin1String(step), target, qt_error_string(code)));
    };
    // A umask can only narrow mkostemp's mode; make it exactly owner read/write.
    if (::fchmod(descriptor.get(), S_IRUSR | S_IWUSR) != 0)
        return discard("restrict");
    if (!write_all(descriptor.get(), bytes))
        return discard("write");
    if (::fsync(descriptor.get()) != 0)
        return discard("sync");
    if (::close(descriptor.release()) != 0)
        return discard("close");
    if (::rename(temporary.constData(), name.constData()) != 0)
        return discard("replace");
    return true;
}

} // namespace lapis::session::posix
