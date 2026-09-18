#include "session_descriptor.hpp"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("Descriptor expectation failed");
}
template <typename Operation> void rejects(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected);
}
struct Stat {
    struct stat data{};
    Stat(const QString& path) {
        if (::stat(QFile::encodeName(path).constData(), &data) != 0)
            throw std::runtime_error("Cannot stat descriptor");
    }
};
void write_file(const QString& path, const QByteArray& bytes, mode_t mode = 0600) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot create descriptor fixture");
    if (file.write(bytes) != bytes.size() || !file.flush())
        throw std::runtime_error("Cannot write descriptor fixture");
    file.close();
    if (::chmod(QFile::encodeName(path).constData(), mode) != 0)
        throw std::runtime_error("Cannot set descriptor fixture mode");
}
QByteArray descriptor_bytes(const QByteArray& fingerprint, const QByteArray& session,
                            const QByteArray& epoch) {
    return QByteArrayLiteral("LAPIS-S1\n") + session + epoch + fingerprint;
}
} // namespace

int main() {
    using namespace lapis::session;
    try {
        QTemporaryDir directory;
        require(directory.isValid());
        const QString endpoint = directory.filePath(QStringLiteral("session.sock"));
        const QString path = endpoint + QStringLiteral(".session");
        const QByteArray fingerprint(32, 'F');
        const QByteArray session(16, 'S');
        const QByteArray epoch(16, 'E');
        const wire::SessionIdentity identity{session, epoch};
        const QByteArray bytes = descriptor_bytes(fingerprint, session, epoch);

        require(!read_descriptor(endpoint, fingerprint).has_value());
        write_descriptor(endpoint, fingerprint, identity);
        const auto read = read_descriptor(endpoint, fingerprint);
        require(read.has_value() && *read == identity);
        const Stat info(path);
        require(info.data.st_uid == ::geteuid() && info.data.st_nlink == 1 &&
                (info.data.st_mode & 07777U) == 0600U && info.data.st_size == bytes.size());
        require(QDir(directory.path())
                    .entryList(QStringList() << QStringLiteral("session.sock.session.*.tmp"),
                               QDir::Files | QDir::NoDotAndDotDot)
                    .isEmpty());

        rejects([&] { static_cast<void>(read_descriptor(endpoint, QByteArray(32, 'X'))); });
        write_file(path, bytes + 'x');
        rejects([&] { static_cast<void>(read_descriptor(endpoint, fingerprint)); });
        write_descriptor(endpoint, fingerprint, identity);
        require(read_descriptor(endpoint, fingerprint) == identity);

        auto corrupt = bytes;
        corrupt[9] = '\0';
        for (int index = 10; index < 25; ++index)
            corrupt[index] = '\0';
        write_file(path, corrupt);
        rejects([&] { static_cast<void>(read_descriptor(endpoint, fingerprint)); });
        write_descriptor(endpoint, fingerprint, identity);
        require(read_descriptor(endpoint, fingerprint) == identity);

        auto old_version = bytes;
        old_version.replace(7, 1, QByteArrayLiteral("0"));
        write_file(path, old_version);
        rejects([&] { static_cast<void>(read_descriptor(endpoint, fingerprint)); });
        write_descriptor(endpoint, fingerprint, identity);

        for (const mode_t mode : {mode_t{0000}, mode_t{0400}, mode_t{0644}}) {
            if (::chmod(QFile::encodeName(path).constData(), mode) != 0)
                throw std::runtime_error("Cannot change descriptor mode");
            rejects([&] { static_cast<void>(read_descriptor(endpoint, fingerprint)); });
            rejects([&] { write_descriptor(endpoint, fingerprint, identity); });
        }
        QFile::remove(path);
        const QString target = directory.filePath(QStringLiteral("target"));
        QFile target_file(target);
        require(target_file.open(QIODevice::WriteOnly));
        target_file.write(bytes);
        target_file.close();
        require(QFile::link(target, path));
        rejects([&] { static_cast<void>(read_descriptor(endpoint, fingerprint)); });
        rejects([&] { write_descriptor(endpoint, fingerprint, identity); });
        QFile::remove(path);

        write_file(target, bytes);
        require(::link(QFile::encodeName(target).constData(),
                       QFile::encodeName(path).constData()) == 0);
        rejects([&] { static_cast<void>(read_descriptor(endpoint, fingerprint)); });
        rejects([&] { write_descriptor(endpoint, fingerprint, identity); });
        require(QFile::remove(path));
        require(::mkfifo(QFile::encodeName(path).constData(), 0600) == 0);
        rejects([&] { static_cast<void>(read_descriptor(endpoint, fingerprint)); });
        rejects([&] { write_descriptor(endpoint, fingerprint, identity); });
        require(QFile::remove(path));

        write_descriptor(endpoint, fingerprint, identity);
        const auto before = read_descriptor(endpoint, fingerprint);
        write_descriptor(endpoint, fingerprint, identity);
        require(read_descriptor(endpoint, fingerprint) == before);
        write_descriptor(endpoint, fingerprint, {wire::new_id(), epoch});
        require(read_descriptor(endpoint, fingerprint) != before);

        rejects([&] { write_descriptor(endpoint, QByteArray(31, 'F'), identity); });
        rejects([&] { write_descriptor(endpoint, fingerprint, {}); });
        rejects([&] { static_cast<void>(read_descriptor(endpoint, QByteArray(31, 'F'))); });

        std::cout << "Session descriptor read, replace, rejection and permissions passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
