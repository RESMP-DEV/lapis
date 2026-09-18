#include "local_endpoint.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace lapis::session::posix {
QString prepare_endpoint(const QString& endpoint) {
    if (endpoint.isEmpty() || endpoint.contains(QChar::Null))
        throw std::invalid_argument("Socket path is required");
    const QFileInfo file(endpoint);
    const QString parent = file.absolutePath();
    if (!QDir().mkpath(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        throw std::runtime_error("Cannot create socket directory");
    const auto directory = QFileInfo(parent).canonicalFilePath();
    const auto validate_directory = [](const QString& path, bool endpoint_parent) {
        struct stat info{};
        if (::stat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISDIR(info.st_mode))
            throw std::runtime_error("Cannot inspect socket directory");
        const auto mode = static_cast<unsigned int>(info.st_mode);
        if (endpoint_parent) {
            if (info.st_uid != ::getuid() || (mode & 07777U) != 0700U)
                throw std::runtime_error("Socket directory must be owned by you with mode 0700");
        } else if ((info.st_uid != ::getuid() && info.st_uid != 0) ||
                   ((mode & 0022U) != 0 && (mode & S_ISVTX) == 0)) {
            // A trusted sticky ancestor (for example /tmp) cannot be used by
            // another user to rename our owned child. Non-sticky shared paths can.
            throw std::runtime_error("Socket path traverses an untrusted writable directory");
        }
    };
    validate_directory(directory, true);
    for (QString ancestor = QFileInfo(directory).path();; ancestor = QFileInfo(ancestor).path()) {
        validate_directory(ancestor, false);
        if (ancestor == QLatin1String("/"))
            break;
    }
    const QString result = QDir(directory).filePath(file.fileName());
    const sockaddr_un address{};
    if (QFile::encodeName(result).size() >= static_cast<qsizetype>(sizeof(address.sun_path)))
        throw std::invalid_argument("Socket path is too long");
    if (QFileInfo(result).isSymLink())
        throw std::invalid_argument("Socket path cannot be a symlink");
    struct stat existing{};
    if (::lstat(QFile::encodeName(result).constData(), &existing) == 0 &&
        !S_ISSOCK(existing.st_mode))
        throw std::invalid_argument("Socket path already contains a non-socket file");
    return result;
}
} // namespace lapis::session::posix
