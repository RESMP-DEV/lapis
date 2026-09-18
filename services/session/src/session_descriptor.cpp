#include "session_descriptor.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/unique_fd.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace lapis::session {
namespace {
constexpr qsizetype descriptor_size = 73;
constexpr qsizetype fingerprint_size = 32;

void check_descriptor(bool valid) {
    if (!valid)
        throw std::runtime_error("Invalid session descriptor");
}
[[noreturn]] void throw_system(const char* message) {
    throw std::runtime_error(std::string(message) + ": " +
                             std::error_code(errno, std::generic_category()).message());
}
QByteArray read_exact(int descriptor) {
    QByteArray result(descriptor_size, Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < descriptor_size) {
        const ssize_t count = ::read(descriptor, result.data() + offset,
                                     static_cast<std::size_t>(descriptor_size - offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw_system("Cannot read session descriptor");
        }
        check_descriptor(count != 0);
        offset += count;
    }
    return result;
}
void write_exact(int descriptor, const QByteArray& bytes) {
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.constData() + offset,
                                      static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw_system("Cannot write session descriptor");
        }
        check_descriptor(count != 0);
        offset += count;
    }
}
QByteArray encode_descriptor(const QByteArray& fingerprint, const wire::SessionIdentity& identity) {
    check_descriptor(fingerprint.size() == fingerprint_size && wire::valid_identity(identity));
    QByteArray result;
    result += QByteArrayLiteral("LAPIS-S1\n");
    result += identity.session_id;
    result += identity.epoch;
    result += fingerprint;
    check_descriptor(result.size() == descriptor_size);
    return result;
}
void validate_open_descriptor(int descriptor) {
    struct stat info{};
    if (::fstat(descriptor, &info) != 0)
        throw_system("Cannot inspect session descriptor");
    check_descriptor(S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && info.st_nlink == 1 &&
                     (info.st_mode & 07777U) == 0600U && info.st_size == descriptor_size);
}
QString descriptor_path(const QString& endpoint) {
    return posix::prepare_endpoint(endpoint) + QLatin1String(".session");
}
void validate_existing_descriptor(const QString& path) {
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0) {
        if (errno == ENOENT)
            return;
        throw_system("Cannot inspect existing session descriptor");
    }
    check_descriptor(S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && info.st_nlink == 1 &&
                     (info.st_mode & 07777U) == 0600U);
}
} // namespace

std::optional<wire::SessionIdentity> read_descriptor(const QString& endpoint,
                                                     const QByteArray& fingerprint) {
    check_descriptor(fingerprint.size() == fingerprint_size);
    const QString path = descriptor_path(endpoint);
    const int descriptor =
        ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        if (errno == ENOENT)
            return std::nullopt;
        throw_system("Cannot open session descriptor");
    }
    const posix::UniqueFd owned{descriptor};
    validate_open_descriptor(descriptor);
    const QByteArray bytes = read_exact(descriptor);
    check_descriptor(bytes.startsWith(QByteArrayLiteral("LAPIS-S1\n")));
    wire::SessionIdentity identity;
    identity.session_id = bytes.sliced(9, 16);
    identity.epoch = bytes.sliced(25, 16);
    check_descriptor(wire::valid_identity(identity) &&
                     QByteArrayView(bytes.sliced(41, fingerprint_size)) ==
                         QByteArrayView(fingerprint));
    return identity;
}

void write_descriptor(const QString& endpoint, const QByteArray& fingerprint,
                      const wire::SessionIdentity& identity) {
    const QString path = descriptor_path(endpoint);
    validate_existing_descriptor(path);
    const QByteArray bytes = encode_descriptor(fingerprint, identity);
    const QFileInfo destination(path);
    const QString base = destination.fileName() + QLatin1Char('.');
    for (int attempt = 0; attempt < 64; ++attempt) {
        const QString temporary = destination.absoluteDir().filePath(
            base + QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex()) +
            QStringLiteral(".tmp"));
        const int descriptor =
            ::open(QFile::encodeName(temporary).constData(),
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST)
                continue;
            throw_system("Cannot create temporary session descriptor");
        }
        const posix::UniqueFd owned{descriptor};
        try {
            if (::fchmod(descriptor, 0600) != 0)
                throw_system("Cannot set session descriptor permissions");
            write_exact(descriptor, bytes);
            validate_open_descriptor(descriptor);
            if (::fsync(descriptor) != 0)
                throw_system("Cannot sync temporary session descriptor");
            validate_existing_descriptor(path);
            if (::rename(QFile::encodeName(temporary).constData(),
                         QFile::encodeName(path).constData()) != 0)
                throw_system("Cannot replace session descriptor");
            return;
        } catch (...) {
            const int saved_errno = errno;
            static_cast<void>(::unlink(QFile::encodeName(temporary).constData()));
            errno = saved_errno;
            throw;
        }
    }
    throw std::runtime_error("Cannot allocate a temporary session descriptor name");
}
} // namespace lapis::session
