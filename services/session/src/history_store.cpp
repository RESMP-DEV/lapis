#include "history_store.hpp"
#include "platform/posix/unique_fd.hpp"
#include "transport/local_protocol.hpp"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace lapis::session {
namespace {
constexpr qint64 max_record_bytes = qint64{8} * 1024 * 1024;
constexpr auto magic = "LPHIST01";
struct Entry {
    QString path;
    QString session;
    quint64 id{};
    quint64 bytes{};
    qint64 modified{};
};
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
bool session_name(const QString& name) {
    return name.size() == 32 && std::all_of(name.begin(), name.end(), [](QChar ch) {
               return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
                      (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'));
           });
}
void private_directory(const QString& path) {
    const auto native = QFile::encodeName(path);
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST)
        fail("Could not create private history directory");
    struct stat status{};
    if (::lstat(native.constData(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::getuid() || (status.st_mode & 0077U) != 0)
        fail("History directory is not private or is a symlink");
}
posix::UniqueFd private_file(const QString& path, int flags) {
    posix::UniqueFd fd(
        ::open(QFile::encodeName(path).constData(),
               static_cast<int>(static_cast<unsigned int>(flags) | O_CLOEXEC | O_NOFOLLOW), 0600));
    struct stat status{};
    if (!fd || ::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::getuid() || (status.st_mode & 0077U) != 0)
        fail("History file is not a private regular file");
    return fd;
}
posix::UniqueFd lock_root(const QString& root) {
    auto fd = private_file(root + QStringLiteral("/.lock"), O_RDWR | O_CREAT);
    // Only called on the dedicated I/O worker in the service. Allow ordinary
    // contention between sessions without turning it into an archive gap.
    for (int attempt = 0; attempt < 250; ++attempt) {
        if (::flock(fd.get(), LOCK_EX | LOCK_NB) == 0)
            return fd;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
            fail("Could not lock history archive");
        QThread::msleep(2);
    }
    fail("History archive remained busy for 500 ms");
}
QByteArray read_private(const QString& path, qint64 maximum) {
    auto fd = private_file(path, O_RDONLY);
    QFile file;
    if (!file.open(fd.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle) ||
        file.size() < 0 || file.size() > maximum)
        fail("History record exceeds its read bound");
    const auto bytes = file.read(maximum + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() != file.size() ||
        bytes.size() > maximum)
        fail("Could not read complete history record");
    return bytes;
}
void remove_pending(const QString& directory) {
    const auto path = directory + QStringLiteral("/.pending");
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
        return;
    auto fd = private_file(path, O_RDONLY);
    static_cast<void>(fd);
    if (!QFile::remove(path))
        fail("Could not recover interrupted history write");
}
struct PendingPath {
    QString path;
    ~PendingPath() { static_cast<void>(QFile::remove(path)); }
};
void atomic_write(const QString& path, const QByteArray& bytes) {
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        auto existing = private_file(path, O_RDONLY);
        static_cast<void>(existing);
    }
    const auto directory = QFileInfo(path).absolutePath();
    remove_pending(directory);
    const PendingPath pending{directory + QStringLiteral("/.pending")};
    auto fd = private_file(pending.path, O_WRONLY | O_CREAT | O_EXCL);
    QFile file;
    if (!file.open(fd.get(), QIODevice::WriteOnly, QFileDevice::DontCloseHandle) ||
        file.write(bytes) != bytes.size() || !file.flush() || ::fsync(fd.get()) != 0 ||
        ::rename(QFile::encodeName(pending.path).constData(),
                 QFile::encodeName(path).constData()) != 0)
        fail("Could not commit history record; storage may be full or unavailable");
}
std::vector<Entry> scan(const QString& root, const HistoryLimits& limits) {
    std::vector<Entry> entries;
    QDirIterator sessions(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    quint32 directories{};
    quint32 files{};
    while (sessions.hasNext()) {
        const auto directory = sessions.next();
        if (++directories > 1024)
            fail("Too many history session directories");
        const auto session = sessions.fileName();
        if (!session_name(session))
            continue;
        private_directory(directory);
        // The root lock excludes active writers: this is an abandoned write.
        remove_pending(directory);
        QDirIterator pages(directory,
                           QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
        while (pages.hasNext()) {
            const auto path = pages.next();
            if (++files > limits.max_pages * 2U + 2048U)
                fail("History directory scan limit exceeded");
            const auto name = pages.fileName();
            if (name.size() != 25 || !name.endsWith(QStringLiteral(".page")))
                continue;
            bool valid{};
            const auto id = name.first(20).toULongLong(&valid);
            if (!valid || id == 0 ||
                QStringLiteral("%1.page").arg(id, 20, 10, QLatin1Char('0')) != name)
                continue;
            auto fd = private_file(path, O_RDONLY);
            struct stat status{};
            if (::fstat(fd.get(), &status) != 0 || status.st_size < 0 ||
                status.st_size > max_record_bytes)
                fail("Invalid history record size");
            entries.push_back({path, session, id, static_cast<quint64>(status.st_size),
                               pages.fileInfo().lastModified().toMSecsSinceEpoch()});
        }
    }
    return entries;
}
HistoryStats totals(const std::vector<Entry>& entries, const QString& session) {
    HistoryStats result;
    for (const auto& entry : entries) {
        result.global_bytes += entry.bytes;
        if (entry.session == session) {
            result.session_bytes += entry.bytes;
            ++result.pages;
        }
    }
    return result;
}
void remove_record(const Entry& entry) {
    auto fd = private_file(entry.path, O_RDONLY);
    static_cast<void>(fd);
    if (!QFile::remove(entry.path))
        fail("Could not evict history record");
}
quint64 reserve_id(const QString& directory, const std::vector<Entry>& entries,
                   const QString& session) {
    const auto path = directory + QStringLiteral("/counter");
    quint64 value{};
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        const auto bytes = read_private(path, 40);
        if (bytes.size() != 40 ||
            QCryptographicHash::hash(bytes.first(8), QCryptographicHash::Sha256) != bytes.sliced(8))
            fail("Corrupt history sequence counter");
        QDataStream input(bytes.first(8));
        input >> value;
    }
    for (const auto& entry : entries)
        if (entry.session == session)
            value = std::max(value, entry.id);
    if (value == std::numeric_limits<quint64>::max())
        fail("History sequence exhausted");
    QByteArray bytes;
    QDataStream output(&bytes, QIODevice::WriteOnly);
    output << ++value;
    bytes += QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    atomic_write(path, bytes);
    return value;
}
HistoryPage read_page(const Entry& entry) {
    const auto bytes = read_private(entry.path, max_record_bytes);
    if (bytes.size() < 48 || bytes.first(8) != QByteArray(magic, 8))
        fail("Invalid history record header");
    QDataStream input(bytes.mid(8, 8));
    quint64 length{};
    input >> length;
    const auto payload = bytes.sliced(48);
    if (length != static_cast<quint64>(payload.size()) ||
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256) != bytes.mid(16, 32))
        fail("Truncated or corrupt history record");
    return {entry.id, wire::decode_snapshot(payload)};
}
} // namespace
HistoryStore::HistoryStore(const QString& root, QStringView session_id, const HistoryLimits& limits)
    : root_(QDir::cleanPath(root)), session_id_(session_id.toString()),
      directory_(root_ + QLatin1Char('/') + session_id_), limits_(limits) {
    if (!QDir::isAbsolutePath(root_) || !session_name(session_id_) || limits_.session_bytes == 0 ||
        limits_.global_bytes == 0 || limits_.session_bytes > limits_.global_bytes ||
        limits_.global_bytes > quint64{4} * 1024 * 1024 * 1024 || limits_.max_pages == 0 ||
        limits_.max_pages > 16384)
        throw std::invalid_argument("Invalid history directory, identity or budget");
    if (!QDir().mkpath(QFileInfo(root_).absolutePath()))
        fail("Could not create history parent directory");
    private_directory(root_);
    auto lock = lock_root(root_);
    private_directory(directory_);
}
quint64 HistoryStore::append(const TerminalSnapshot& page) {
    const auto payload = wire::encode_snapshot(page);
    if (payload.size() > max_record_bytes - 48)
        fail("History page exceeds record size limit");
    QByteArray bytes(magic, 8);
    QDataStream output(&bytes, QIODevice::Append);
    output << static_cast<quint64>(payload.size());
    bytes += QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    bytes += payload;
    const auto size = static_cast<quint64>(bytes.size());
    if (size > limits_.session_bytes || size > limits_.global_bytes)
        fail("History page exceeds archive budget");
    auto lock = lock_root(root_);
    auto entries = scan(root_, limits_);
    const auto id = reserve_id(directory_, entries, session_id_);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.modified < b.modified || (a.modified == b.modified && a.path < b.path);
    });
    const auto destination =
        directory_ + QStringLiteral("/%1.page").arg(id, 20, 10, QLatin1Char('0'));
    // Reserve at most one extra record before eviction. A failed write retains
    // previously committed pages instead of evicting them for an absent page.
    atomic_write(destination, bytes);
    auto usage = totals(entries, session_id_);
    std::size_t count = entries.size();
    try {
        for (const auto& entry : entries) {
            const bool session_over = usage.session_bytes + size > limits_.session_bytes;
            const bool global_over =
                usage.global_bytes + size > limits_.global_bytes || count >= limits_.max_pages;
            if (!session_over && !global_over)
                break;
            if (session_over && entry.session != session_id_ && !global_over)
                continue;
            remove_record(entry);
            usage.global_bytes -= entry.bytes;
            if (entry.session == session_id_)
                usage.session_bytes -= entry.bytes;
            --count;
        }
    } catch (...) {
        static_cast<void>(QFile::remove(destination));
        throw;
    }
    return id;
}
std::optional<HistoryPage> HistoryStore::older(quint64 before) {
    auto lock = lock_root(root_);
    const auto entries = scan(root_, limits_);
    const Entry* selected{};
    for (const auto& entry : entries)
        if (entry.session == session_id_ && (!before || entry.id < before) &&
            (!selected || entry.id > selected->id))
            selected = &entry;
    return selected ? std::optional{read_page(*selected)} : std::nullopt;
}
std::optional<HistoryPage> HistoryStore::newer(quint64 after) {
    auto lock = lock_root(root_);
    const auto entries = scan(root_, limits_);
    const Entry* selected{};
    for (const auto& entry : entries)
        if (entry.session == session_id_ && entry.id > after &&
            (!selected || entry.id < selected->id))
            selected = &entry;
    return selected ? std::optional{read_page(*selected)} : std::nullopt;
}
void HistoryStore::clear() {
    auto lock = lock_root(root_);
    for (const auto& entry : scan(root_, limits_))
        if (entry.session == session_id_)
            remove_record(entry);
}
HistoryStats HistoryStore::stats() {
    auto lock = lock_root(root_);
    return totals(scan(root_, limits_), session_id_);
}
} // namespace lapis::session
