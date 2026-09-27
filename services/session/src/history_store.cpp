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
#include <fcntl.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace lapis::session {
namespace {
// One encoded page before compression, as the wire bounds it.
constexpr qint64 max_page_bytes = qint64{8} * 1024 * 1024;
constexpr quint64 max_segment_bytes = quint64{16} * 1024 * 1024;
constexpr quint64 max_global_bytes = quint64{64} * 1024 * 1024 * 1024;
constexpr qsizetype header_bytes = 72;
constexpr auto magic = "LPJRNL01";
constexpr quint32 max_sessions = 1024;
constexpr quint32 max_files = 65536;

// A file under the root, for the quotas.
struct Entry {
    QString path;
    QString session;
    quint64 segment{}; // its first page ID; zero for a page from before segments
    quint64 bytes{};
    qint64 modified{};
    bool newest{}; // its session's open segment, which only that session evicts
};
struct Header {
    quint64 id{};
    quint64 first{};
    quint64 rows{};
    quint64 length{};
    QByteArray digest;
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
std::optional<quint64> numbered(const QString& name, QLatin1StringView suffix) {
    if (name.size() != 20 + suffix.size() || !name.endsWith(suffix))
        return std::nullopt;
    bool valid{};
    const auto id = name.first(20).toULongLong(&valid);
    if (!valid || id == 0 ||
        QStringLiteral("%1").arg(id, 20, 10, QLatin1Char('0')) + suffix != name)
        return std::nullopt;
    return id;
}
std::optional<quint64> segment_id(const QString& name) {
    return numbered(name, QLatin1StringView(".seg"));
}
bool legacy_page(const QString& name) {
    return numbered(name, QLatin1StringView(".page")).has_value();
}
// Every segment and legacy page under the root, stat only.
std::vector<Entry> scan(const QString& root) {
    std::vector<Entry> entries;
    QDirIterator sessions(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    quint32 directories{};
    quint32 files{};
    while (sessions.hasNext()) {
        const auto directory = sessions.next();
        if (++directories > max_sessions)
            fail("Too many history session directories");
        const auto session = sessions.fileName();
        if (!session_name(session))
            continue;
        private_directory(directory);
        const auto first_of_session = entries.size();
        QDirIterator pages(directory,
                           QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
        while (pages.hasNext()) {
            const auto path = pages.next();
            if (++files > max_files)
                fail("History directory scan limit exceeded");
            const auto name = pages.fileName();
            const auto segment = segment_id(name);
            if (!segment && !legacy_page(name))
                continue;
            struct stat status{};
            if (::lstat(QFile::encodeName(path).constData(), &status) != 0)
                continue; // evicted meanwhile
            if (!S_ISREG(status.st_mode) || status.st_uid != ::getuid() ||
                (status.st_mode & 0077U) != 0)
                fail("History file is not a private regular file");
            entries.push_back({path, session, segment.value_or(0),
                               static_cast<quint64>(status.st_size),
                               pages.fileInfo().lastModified().toMSecsSinceEpoch(), false});
        }
        const auto own = std::span(entries).subspan(first_of_session);
        const auto open =
            std::max_element(own.begin(), own.end(),
                             [](const auto& a, const auto& b) { return a.segment < b.segment; });
        if (open != own.end() && open->segment != 0)
            open->newest = true;
    }
    return entries;
}
HistoryStats totals(const std::vector<Entry>& entries, const QString& session) {
    HistoryStats result;
    for (const auto& entry : entries) {
        result.global_bytes += entry.bytes;
        if (entry.session == session)
            result.session_bytes += entry.bytes;
    }
    return result;
}
QByteArray encode_header(const Header& header) {
    QByteArray bytes(magic, 8);
    QDataStream out(&bytes, QIODevice::Append);
    out << header.id << header.first << static_cast<quint32>(header.rows) << quint32{0}
        << header.length;
    bytes += header.digest;
    if (bytes.size() != header_bytes)
        fail("History record header size mismatch");
    return bytes;
}
std::optional<Header> decode_header(const QByteArray& bytes) {
    if (bytes.size() != header_bytes || !bytes.startsWith(QByteArray(magic, 8)))
        return std::nullopt;
    QDataStream in(bytes.sliced(8, 32));
    Header header;
    quint32 rows{};
    quint32 reserved{};
    in >> header.id >> header.first >> rows >> reserved >> header.length;
    header.rows = rows;
    header.digest = bytes.sliced(40);
    if (in.status() != QDataStream::Ok || header.id == 0 || rows == 0 || reserved != 0 ||
        header.length == 0 || header.length > static_cast<quint64>(max_page_bytes))
        return std::nullopt;
    return header;
}
QByteArray read_at(int fd, quint64 offset, qint64 length) {
    QByteArray bytes(length, Qt::Uninitialized);
    qint64 done = 0;
    while (done < length) {
        const auto count = ::pread(fd, bytes.data() + done, static_cast<std::size_t>(length - done),
                                   static_cast<off_t>(offset + static_cast<quint64>(done)));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            fail("Truncated or corrupt history record");
        done += count;
    }
    return bytes;
}
bool write_all(int fd, const QByteArray& bytes) {
    qsizetype done = 0;
    while (done < bytes.size()) {
        const auto count =
            ::write(fd, bytes.constData() + done, static_cast<std::size_t>(bytes.size() - done));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        done += count;
    }
    return true;
}
quint64 read_counter(const QString& path) {
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
        return 0;
    const auto bytes = read_private(path, 40);
    if (bytes.size() != 40 ||
        QCryptographicHash::hash(bytes.first(8), QCryptographicHash::Sha256) != bytes.sliced(8))
        fail("Corrupt history sequence counter");
    QDataStream input(bytes.first(8));
    quint64 value{};
    input >> value;
    return value;
}
void write_counter(const QString& path, quint64 value) {
    QByteArray bytes;
    QDataStream output(&bytes, QIODevice::WriteOnly);
    output << value;
    bytes += QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    atomic_write(path, bytes);
}
} // namespace

HistoryStore::HistoryStore(const QString& root, QStringView session_id, const HistoryLimits& limits)
    : root_(QDir::cleanPath(root)), session_id_(session_id.toString()),
      directory_(root_ + QLatin1Char('/') + session_id_), limits_(limits) {
    if (!QDir::isAbsolutePath(root_) || !session_name(session_id_) || limits_.session_bytes == 0 ||
        limits_.global_bytes == 0 || limits_.session_bytes > limits_.global_bytes ||
        limits_.global_bytes > max_global_bytes)
        throw std::invalid_argument("Invalid history directory, identity or budget");
    if (!QDir().mkpath(QFileInfo(root_).absolutePath()))
        fail("Could not create history parent directory");
    private_directory(root_);
    auto lock = lock_root(root_);
    private_directory(directory_);
    load();
}
QString HistoryStore::segmentPath(quint64 first_id) const {
    return directory_ + QStringLiteral("/%1.seg").arg(first_id, 20, 10, QLatin1Char('0'));
}
// The index, read from this session's segments. A record cut short by an
// interrupted write is the tail of its segment, and is cut off.
void HistoryStore::load() {
    remove_pending(directory_);
    records_.clear();
    segments_.clear();
    std::vector<quint64> firsts;
    QDirIterator files(directory_,
                       QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
    while (files.hasNext()) {
        files.next();
        if (const auto first = segment_id(files.fileName()))
            firsts.push_back(*first);
    }
    std::sort(firsts.begin(), firsts.end());
    for (const auto first : firsts) {
        const auto path = segmentPath(first);
        auto fd = private_file(path, O_RDWR);
        struct stat status{};
        if (::fstat(fd.get(), &status) != 0 || status.st_size < 0)
            fail("Could not inspect history segment");
        const auto size = static_cast<quint64>(status.st_size);
        quint64 offset = 0;
        while (offset + header_bytes <= size) {
            const auto header = decode_header(read_at(fd.get(), offset, header_bytes));
            if (!header || offset + header_bytes + header->length > size ||
                (!records_.empty() && header->id <= records_.back().id))
                break;
            records_.push_back(
                {first, offset, header->length, header->id, header->first, header->rows});
            offset += header_bytes + header->length;
        }
        if (offset < size && ::ftruncate(fd.get(), static_cast<off_t>(offset)) != 0)
            fail("Could not recover interrupted history write");
        if (offset == 0) {
            if (!QFile::remove(path))
                fail("Could not recover interrupted history write");
            continue;
        }
        segments_.push_back(first);
    }
    const auto counted = read_counter(directory_ + QStringLiteral("/counter"));
    const auto last = records_.empty() ? quint64{0} : records_.back().id;
    if (std::max(counted, last) == std::numeric_limits<quint64>::max())
        fail("History sequence exhausted");
    next_id_ = std::max(counted, last) + 1;
}
quint64 HistoryStore::append(const TerminalSnapshot& page) {
    const auto encoded = wire::encode_snapshot(page);
    if (encoded.size() > max_page_bytes || page.size.rows == 0)
        fail("History page exceeds record size limit");
    const auto payload = qCompress(encoded, 6);
    if (payload.size() > max_page_bytes)
        fail("History page exceeds record size limit");
    const Header header{next_id_,
                        records_.empty() ? 0 : records_.back().first + records_.back().rows,
                        page.size.rows, static_cast<quint64>(payload.size()),
                        QCryptographicHash::hash(payload, QCryptographicHash::Sha256)};
    const auto record = encode_header(header) + payload;
    const auto size = static_cast<quint64>(record.size());
    if (size > limits_.session_bytes || size > limits_.global_bytes)
        fail("History page exceeds archive budget");
    auto lock = lock_root(root_);
    // Several segments make up even a small budget, so the oldest can go.
    const auto segment_limit = std::clamp(limits_.session_bytes / 4, quint64{1}, max_segment_bytes);
    auto segment = segments_.empty() ? header.id : segments_.back();
    quint64 offset = 0;
    if (!segments_.empty()) {
        const QFileInfo current(segmentPath(segment));
        offset = current.exists() ? static_cast<quint64>(current.size()) : 0;
        if (!current.exists() || offset + size > segment_limit) {
            segment = header.id;
            offset = 0;
        }
    }
    const bool fresh = segments_.empty() || segment != segments_.back();
    const auto path = segmentPath(segment);
    auto fd = private_file(path, fresh ? O_WRONLY | O_CREAT | O_EXCL : O_WRONLY | O_APPEND);
    if (!write_all(fd.get(), record) || ::fsync(fd.get()) != 0) {
        // A failed write keeps what was committed: undo this one only.
        if (fresh)
            static_cast<void>(QFile::remove(path));
        else
            static_cast<void>(::ftruncate(fd.get(), static_cast<off_t>(offset)));
        fail("Could not commit history record; storage may be full or unavailable");
    }
    if (fresh)
        segments_.push_back(segment);
    records_.push_back({segment, offset, header.length, header.id, header.first, header.rows});
    ++next_id_;
    enforceBudgets();
    return header.id;
}
// Oldest first, session budget before global: a session over its own budget
// gives up its own segments; over the global budget, any session's closed
// segments go, the least recently written first.
void HistoryStore::enforceBudgets() {
    auto entries = scan(root_);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.modified < b.modified || (a.modified == b.modified && a.path < b.path);
    });
    auto usage = totals(entries, session_id_);
    for (const auto& entry : entries) {
        const bool session_over = usage.session_bytes > limits_.session_bytes;
        const bool global_over = usage.global_bytes > limits_.global_bytes;
        if (!session_over && !global_over)
            break;
        const bool own = entry.session == session_id_;
        if (entry.newest || (!global_over && !own))
            continue;
        if (!QFile::remove(entry.path) && QFileInfo::exists(entry.path))
            fail("Could not evict history record");
        usage.global_bytes -= entry.bytes;
        if (own)
            usage.session_bytes -= entry.bytes;
        if (own && entry.segment != 0) {
            std::erase(segments_, entry.segment);
            std::erase_if(records_, [&entry](const Record& record) {
                return record.segment == entry.segment;
            });
        }
    }
}
HistoryPage HistoryStore::read(const Record& record) {
    auto fd = private_file(segmentPath(record.segment), O_RDONLY);
    const auto bytes =
        read_at(fd.get(), record.offset, header_bytes + static_cast<qint64>(record.length));
    const auto header = decode_header(bytes.first(header_bytes));
    const auto payload = bytes.sliced(header_bytes);
    if (!header || header->id != record.id || header->length != record.length ||
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256) != header->digest)
        fail("Truncated or corrupt history record");
    const auto encoded = qUncompress(payload);
    if (encoded.isEmpty() || encoded.size() > max_page_bytes)
        fail("Truncated or corrupt history record");
    auto snapshot = wire::decode_snapshot(encoded);
    const auto base = records_.front().first;
    const auto end = records_.back().first + records_.back().rows;
    snapshot.history = {static_cast<std::size_t>(end - base),
                        static_cast<std::size_t>(record.first - base),
                        static_cast<std::size_t>(record.rows), true};
    return {record.id, std::move(snapshot)};
}
template <typename Select> std::optional<HistoryPage> HistoryStore::readSelected(Select select) {
    // Held through selection and read: another session's budget needs this
    // lock, and an already-open descriptor survives unlink. A non-cooperating
    // unlink can still win the check/open race, so one missing-segment retry
    // gives the same recovery without reporting archive failure.
    auto lock = lock_root(root_);
    auto selected = select();
    auto recover = [&]() {
        // Another session's budget took it: learn what remains.
        load();
        selected = select();
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (selected && !QFileInfo::exists(segmentPath(selected->segment))) {
            if (attempt != 0)
                return std::nullopt;
            recover();
            continue;
        }
        if (!selected)
            return std::nullopt;
        try {
            return read(*selected);
        } catch (const std::runtime_error&) {
            if (attempt != 0 || QFileInfo::exists(segmentPath(selected->segment)))
                throw;
            recover();
        }
    }
    return std::nullopt;
}
std::optional<HistoryPage> HistoryStore::older(quint64 before) {
    return readSelected([this, before]() -> std::optional<Record> {
        const auto found =
            std::find_if(records_.rbegin(), records_.rend(),
                         [before](const Record& record) { return !before || record.id < before; });
        return found == records_.rend() ? std::nullopt : std::optional{*found};
    });
}
std::optional<HistoryPage> HistoryStore::newer(quint64 after) {
    return readSelected([this, after]() -> std::optional<Record> {
        const auto found =
            std::find_if(records_.begin(), records_.end(),
                         [after](const Record& record) { return record.id > after; });
        return found == records_.end() ? std::nullopt : std::optional{*found};
    });
}
std::optional<HistoryPage> HistoryStore::at(quint64 row) {
    return readSelected([this, row]() -> std::optional<Record> {
        if (records_.empty())
            return std::nullopt;
        const auto base = records_.front().first;
        const auto end = records_.back().first + records_.back().rows;
        if (row >= end - base)
            return records_.back();
        const auto after = std::upper_bound(
            records_.begin(), records_.end(), base + row,
            [](quint64 target, const Record& record) { return target < record.first; });
        return *std::prev(after);
    });
}
void HistoryStore::clear() {
    auto lock = lock_root(root_);
    QDirIterator files(directory_,
                       QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
    while (files.hasNext()) {
        const auto path = files.next();
        if (segment_id(files.fileName()) || legacy_page(files.fileName())) {
            auto fd = private_file(path, O_RDONLY);
            static_cast<void>(fd);
            if (!QFile::remove(path))
                fail("Could not evict history record");
        }
    }
    // Later pages keep later IDs.
    write_counter(directory_ + QStringLiteral("/counter"), next_id_ - 1);
    records_.clear();
    segments_.clear();
}
HistoryStats HistoryStore::stats() {
    auto lock = lock_root(root_);
    // Another session's budget may have evicted closed segments since this
    // index was loaded; report that archive, not stale in-memory records.
    load();
    auto result = totals(scan(root_), session_id_);
    result.pages = records_.size();
    return result;
}
} // namespace lapis::session
