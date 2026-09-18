#include "local_endpoint.hpp"
#include <QDir>
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
    struct stat info{};
    if (::stat(QFile::encodeName(directory).constData(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != ::getuid() || (static_cast<unsigned int>(info.st_mode) & 0077U) != 0)
        throw std::runtime_error("Socket directory must be owned by you with mode 0700");
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
