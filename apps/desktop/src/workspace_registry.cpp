#include "workspace_registry.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/unique_fd.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringView>
#include <QUuid>

#include <algorithm>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace lapis::desktop {
namespace posix = session::posix;
namespace {
constexpr auto schema_key = "schema";
constexpr auto entries_key = "entries";
constexpr auto endpoint_key = "endpoint";
constexpr auto session_id_key = "sessionId";
constexpr auto epoch_key = "epoch";
constexpr auto fingerprint_key = "fingerprint";
constexpr auto title_key = "title";
constexpr auto directory_key = "directory";
constexpr auto agent_key = "agent";
constexpr qsizetype max_json_bytes = qsizetype{64} * 1024;
constexpr qsizetype max_text_bytes = 4096;

[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }

[[noreturn]] void fail_path(const QString& path, const QString& message) {
    fail(path + QStringLiteral(": ") + message);
}

[[noreturn]] void fail_errno(QStringView message, const QString& path = {}) {
    // Build the text only after saving errno; QString and std::generic_category
    // can both make system calls or otherwise alter the thread-local value.
    const int error = errno;
    QString detail = message.toString();
    if (!path.isEmpty())
        detail += QStringLiteral(": ") + path;
    throw std::system_error(error, std::generic_category(), detail.toStdString());
}

bool valid_utf8(const QString& value) {
    if (value.toUtf8().size() > max_text_bytes || value.contains(QChar::Null))
        return false;
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character.isHighSurrogate()) {
            if (index + 1 >= value.size() || !value.at(index + 1).isLowSurrogate())
                return false;
            ++index;
        } else if (character.isLowSurrogate()) {
            return false;
        }
    }
    return true;
}

QString normalized_endpoint(const QString& endpoint) {
    if (endpoint.contains(QChar::Null))
        fail(QStringLiteral("Workspace endpoint contains a null byte"));
    const QString normalized = session::posix::prepare_endpoint(endpoint);
    return normalized;
}

bool valid_agent(session::AgentMode agent) {
    switch (agent) {
    case session::AgentMode::terminal:
    case session::AgentMode::codex:
    case session::AgentMode::claude:
        return true;
    }
    return false;
}

std::vector<WorkspaceEntry> validate_entries(const std::vector<WorkspaceEntry>& entries) {
    if (entries.size() > WorkspaceRegistry::maximum_entries)
        fail(QStringLiteral("Workspace registry contains too many entries"));
    std::vector<QString> endpoints;
    std::vector<QByteArray> session_ids;
    endpoints.reserve(entries.size());
    session_ids.reserve(entries.size());
    auto normalized = entries;
    for (auto& entry : normalized) {
        entry.endpoint = normalized_endpoint(entry.endpoint);
        endpoints.push_back(entry.endpoint);
        if (!session::wire::valid_identity(entry.identity))
            fail(QStringLiteral("Workspace identity is invalid"));
        if (entry.fingerprint.size() != 32)
            fail(QStringLiteral("Workspace fingerprint must contain 32 bytes"));
        if (!valid_utf8(entry.title) || !valid_utf8(entry.directory))
            fail(QStringLiteral("Workspace title or directory is invalid"));
        if (!valid_agent(entry.agent))
            fail(QStringLiteral("Workspace agent mode is unknown"));
        session_ids.push_back(entry.identity.session_id);
    }
    std::sort(endpoints.begin(), endpoints.end());
    if (std::adjacent_find(endpoints.begin(), endpoints.end()) != endpoints.end())
        fail(QStringLiteral("Workspace endpoints must be unique"));
    std::sort(session_ids.begin(), session_ids.end());
    if (std::adjacent_find(session_ids.begin(), session_ids.end()) != session_ids.end())
        fail(QStringLiteral("Workspace session IDs must be unique"));
    return normalized;
}

void validate_directory_status(const struct stat& status, const QString& path) {
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::getuid() ||
        (status.st_mode & 07777U) != 0700U)
        fail(QStringLiteral("%1 must be a private owner directory "
                            "(mode=%2, uid=%3)")
                 .arg(path, QString::number(status.st_mode & 07777U, 8),
                      QString::number(status.st_uid)));
}

void ensure_private_owner_directory(const QString& path) {
    const auto native = QFile::encodeName(path);
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST)
        fail_errno(QStringLiteral("Could not create private workspace directory"), path);
    struct stat status{};
    if (::lstat(native.constData(), &status) != 0)
        fail_errno(QStringLiteral("Could not inspect workspace owner directory"), path);
    validate_directory_status(status, path);
}

posix::UniqueFd open_private_regular_file(const QString& path, int flags) {
    const int descriptor = ::open(
        QFile::encodeName(path).constData(),
        static_cast<int>(static_cast<unsigned int>(flags) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
        0600);
    posix::UniqueFd result{descriptor};
    struct stat status{};
    if (!result)
        fail_errno(QStringLiteral("Could not open workspace file"), path);
    if (::fstat(result.get(), &status) != 0)
        fail_errno(QStringLiteral("Could not inspect workspace file"), path);
    if (!S_ISREG(status.st_mode) || status.st_uid != ::getuid() ||
        (status.st_mode & 07777U) != 0600U || status.st_nlink != 1)
        fail(QStringLiteral("%1 must be a private 0600 regular file with one link "
                            "(mode=%2, uid=%3, nlink=%4)")
                 .arg(path, QString::number(status.st_mode & 07777U, 8),
                      QString::number(status.st_uid), QString::number(status.st_nlink)));
    return result;
}

void fsync_directory(const QString& path) {
    posix::UniqueFd directory{::open(QFile::encodeName(path).constData(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat status{};
    if (!directory)
        fail_errno(QStringLiteral("Could not open workspace owner directory"), path);
    if (::fstat(directory.get(), &status) != 0)
        fail_errno(QStringLiteral("Could not inspect workspace owner directory"), path);
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::getuid() ||
        (status.st_mode & 07777U) != 0700U)
        fail(QStringLiteral("%1 must be a private owner directory (mode=%2, uid=%3)")
                 .arg(path, QString::number(status.st_mode & 07777U, 8),
                      QString::number(status.st_uid)));
    if (::fsync(directory.get()) != 0)
        fail_errno(QStringLiteral("Could not synchronize workspace owner directory"), path);
}

QByteArray encode_entries(const std::vector<WorkspaceEntry>& entries) {
    QJsonArray array;
    for (const auto& entry : entries) {
        QJsonObject object;
        object.insert(QLatin1String(endpoint_key), entry.endpoint);
        object.insert(QLatin1String(session_id_key),
                      QString::fromLatin1(entry.identity.session_id.toHex()));
        object.insert(QLatin1String(epoch_key), QString::fromLatin1(entry.identity.epoch.toHex()));
        object.insert(QLatin1String(fingerprint_key),
                      QString::fromLatin1(entry.fingerprint.toHex()));
        object.insert(QLatin1String(title_key), entry.title);
        object.insert(QLatin1String(directory_key), entry.directory);
        object.insert(QLatin1String(agent_key),
                      entry.agent == session::AgentMode::terminal ? QStringLiteral("terminal")
                      : entry.agent == session::AgentMode::codex  ? QStringLiteral("codex")
                                                                  : QStringLiteral("claude"));
        array.append(std::move(object));
    }
    QJsonObject document;
    document.insert(QLatin1String(schema_key), 1);
    document.insert(QLatin1String(entries_key), std::move(array));
    const QByteArray bytes = QJsonDocument{document}.toJson(QJsonDocument::Compact);
    if (bytes.size() > max_json_bytes)
        fail(QStringLiteral("Workspace registry document exceeds 64 KiB"));
    return bytes;
}

QByteArray read_all(const QString& path) {
    auto file = open_private_regular_file(path, O_RDONLY);
    struct stat status{};
    if (::fstat(file.get(), &status) != 0)
        fail_errno(QStringLiteral("Could not inspect workspace registry"), path);
    if (status.st_size < 0 || status.st_size > max_json_bytes || status.st_nlink != 1)
        fail(QStringLiteral("Workspace registry exceeds 64 KiB or is hard-linked "
                            "(size=%1, nlink=%2)")
                 .arg(QString::number(status.st_size), QString::number(status.st_nlink)));
    QByteArray bytes;
    bytes.resize(static_cast<qsizetype>(status.st_size));
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(file.get(), bytes.data() + offset,
                                     static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            fail_errno(QStringLiteral("Could not read workspace registry"), path);
        }
        if (count == 0)
            fail_path(path, QStringLiteral("Workspace registry changed while it was being read"));
        offset += count;
    }
    if (::fstat(file.get(), &status) != 0)
        fail_errno(QStringLiteral("Could not inspect workspace registry"), path);
    if (status.st_size != bytes.size() || status.st_nlink != 1)
        fail(QStringLiteral("Workspace registry changed while it was being read "
                            "(size=%1, nlink=%2)")
                 .arg(QString::number(status.st_size), QString::number(status.st_nlink)));
    return bytes;
}

std::vector<WorkspaceEntry> decode_entries(const QByteArray& bytes, const QString& path = {}) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail_path(path, QStringLiteral("Workspace registry JSON is corrupt"));
    const QJsonObject root = document.object();
    if (root.size() != 2 || !root.contains(QLatin1String(schema_key)) ||
        !root.contains(QLatin1String(entries_key)))
        fail_path(path, QStringLiteral("Workspace registry contains unknown fields"));
    if (root.value(QLatin1String(schema_key)) != QJsonValue{1})
        fail_path(path, QStringLiteral("Workspace registry schema version is unsupported"));
    const QJsonValue entries_value = root.value(QLatin1String(entries_key));
    if (!entries_value.isArray())
        fail_path(path, QStringLiteral("Workspace registry entries are corrupt"));
    const QJsonArray array = entries_value.toArray();
    if (static_cast<std::size_t>(array.size()) > WorkspaceRegistry::maximum_entries)
        fail_path(path, QStringLiteral("Workspace registry contains too many entries"));
    std::vector<WorkspaceEntry> entries;
    entries.reserve(static_cast<std::size_t>(array.size()));
    for (const auto& value : array) {
        if (!value.isObject())
            fail_path(path, QStringLiteral("Workspace registry entry is corrupt"));
        const QJsonObject object = value.toObject();
        if (object.size() != 7)
            fail_path(path, QStringLiteral(
                                "Workspace registry entry contains unknown or missing fields"));
        WorkspaceEntry entry;
        const auto string_field = [&object, &path](const char* key) {
            const QJsonValue value = object.value(QLatin1String(key));
            if (!value.isString())
                fail_path(path,
                          QStringLiteral("Workspace registry string field is missing or corrupt"));
            return value.toString();
        };
        entry.endpoint = string_field(endpoint_key);
        const auto hex_field = [&](const char* key) {
            const auto encoded = string_field(key);
            const auto decoded = QByteArray::fromHex(encoded.toLatin1());
            if (QString::fromLatin1(decoded.toHex()) != encoded)
                fail_path(path, QStringLiteral(
                                    "Workspace identity fields require canonical lowercase hex"));
            return decoded;
        };
        entry.identity = {hex_field(session_id_key), hex_field(epoch_key)};
        entry.fingerprint = hex_field(fingerprint_key);
        entry.title = string_field(title_key);
        entry.directory = string_field(directory_key);
        const QString agent = string_field(agent_key);
        if (agent == QLatin1String("terminal"))
            entry.agent = session::AgentMode::terminal;
        else if (agent == QLatin1String("codex"))
            entry.agent = session::AgentMode::codex;
        else if (agent == QLatin1String("claude"))
            entry.agent = session::AgentMode::claude;
        else
            fail_path(path, QStringLiteral("Workspace agent mode is unknown"));
        entries.push_back(std::move(entry));
    }
    return validate_entries(entries);
}

class TemporaryFile {
  public:
    explicit TemporaryFile(const QString& path)
        : path_(path), file_(open_private_regular_file(path, O_WRONLY | O_CREAT | O_EXCL)) {}
    ~TemporaryFile() { static_cast<void>(::unlink(QFile::encodeName(path_).constData())); }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] int get() const { return file_.get(); }

    void release() { path_.clear(); }

  private:
    QString path_;
    posix::UniqueFd file_;
};

void validate_existing_storage(const QString& path) {
    if (QFileInfo::exists(path) || QFileInfo{path}.isSymLink())
        static_cast<void>(open_private_regular_file(path, O_RDONLY));
}

void write_exact(int descriptor, const QByteArray& bytes, const QString& path) {
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.constData() + offset,
                                      static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            fail_errno(QStringLiteral("Could not write workspace registry"), path);
        }
        if (count == 0)
            fail_path(path, QStringLiteral("Workspace registry write made no progress"));
        offset += count;
    }
}
} // namespace

class WorkspaceRegistry::Impl final {
  public:
    explicit Impl(const QString& path) {
        if (path.isEmpty() || path.contains(QChar::Null) || QFileInfo(path).isRelative())
            fail(QStringLiteral("Workspace registry path must be absolute"));
        const QFileInfo info{path};
        if (info.fileName().isEmpty() || info.fileName() == QLatin1String(".") ||
            info.fileName() == QLatin1String("..") || info.isSymLink())
            fail_path(path, QStringLiteral("Workspace registry filename is unsafe"));
        owner_directory_ = info.absolutePath();
        ensure_private_owner_directory(owner_directory_);
        owner_directory_ = session::posix::canonical_trusted_directory(owner_directory_);
        storage_path_ = QDir(owner_directory_).filePath(info.fileName());
        validate_existing_storage(storage_path_);
        const auto locked_path = lock_path();
        lock_ = open_private_regular_file(locked_path, O_RDWR | O_CREAT);
        while (::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno == EINTR)
                continue;
            if (errno == EWOULDBLOCK)
                fail(QStringLiteral("Workspace registry is already locked: ") + locked_path);
            fail_errno(QStringLiteral("Could not lock workspace registry"), locked_path);
        }
    }

    [[nodiscard]] std::vector<WorkspaceEntry> read() const {
        if (!QFileInfo::exists(storage_path_) && !QFileInfo{storage_path_}.isSymLink())
            return {};
        return decode_entries(read_all(storage_path_), storage_path_);
    }

    void write(const std::vector<WorkspaceEntry>& entries) {
        const std::vector<WorkspaceEntry> normalized = validate_entries(entries);
        const QByteArray bytes = encode_entries(normalized);
        // Preserve external corruption and dangling symlinks rather than replacing them.
        if (QFileInfo::exists(storage_path_) || QFileInfo{storage_path_}.isSymLink())
            static_cast<void>(decode_entries(read_all(storage_path_), storage_path_));
        const QString temporary_path =
            owner_directory_ + QStringLiteral("/.registry.") +
            QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex()) + QStringLiteral(".tmp");
        TemporaryFile temporary{temporary_path};
        write_exact(temporary.get(), bytes, temporary_path);
        if (::fsync(temporary.get()) != 0)
            fail_errno(QStringLiteral("Could not synchronize temporary workspace registry"),
                       temporary_path);
        if (::rename(QFile::encodeName(temporary_path).constData(),
                     QFile::encodeName(storage_path_).constData()) != 0)
            fail_errno(QStringLiteral("Could not commit workspace registry"), storage_path_);
        temporary.release();
        fsync_directory(owner_directory_);
    }

  private:
    [[nodiscard]] QString lock_path() const { return owner_directory_ + QStringLiteral("/.lock"); }

    QString owner_directory_;
    QString storage_path_;
    posix::UniqueFd lock_;
};

WorkspaceRegistry::WorkspaceRegistry(const QString& path) : impl_(std::make_unique<Impl>(path)) {}
WorkspaceRegistry::~WorkspaceRegistry() = default;

std::vector<WorkspaceEntry> WorkspaceRegistry::read() const { return impl_->read(); }

void WorkspaceRegistry::write(const std::vector<WorkspaceEntry>& entries) { impl_->write(entries); }
} // namespace lapis::desktop
