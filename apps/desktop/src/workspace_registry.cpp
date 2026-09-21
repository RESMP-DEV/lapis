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
#include <QUuid>

#include <algorithm>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
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

[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }

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
        fail("Workspace endpoint contains a null byte");
    const QString normalized = session::posix::prepare_endpoint(endpoint);
    return normalized;
}

bool valid_agent(session::AgentMode agent) {
    switch (agent) {
    case session::AgentMode::terminal:
    case session::AgentMode::codex:
        return true;
    }
    return false;
}

void validate_entry(const WorkspaceEntry& entry) {
    static_cast<void>(normalized_endpoint(entry.endpoint));
    if (!session::wire::valid_identity(entry.identity))
        fail("Workspace identity is invalid");
    if (entry.fingerprint.size() != 32)
        fail("Workspace fingerprint must contain 32 bytes");
    if (!valid_utf8(entry.title) || !valid_utf8(entry.directory))
        fail("Workspace title or directory is invalid");
    if (!valid_agent(entry.agent))
        fail("Workspace agent mode is unknown");
}

void validate_entries(const std::vector<WorkspaceEntry>& entries) {
    if (entries.size() > WorkspaceRegistry::maximum_entries)
        fail("Workspace registry contains too many entries");
    std::vector<QString> endpoints;
    std::vector<QByteArray> session_ids;
    endpoints.reserve(entries.size());
    session_ids.reserve(entries.size());
    for (const auto& entry : entries) {
        validate_entry(entry);
        endpoints.push_back(normalized_endpoint(entry.endpoint));
        session_ids.push_back(entry.identity.session_id);
    }
    std::sort(endpoints.begin(), endpoints.end());
    if (std::adjacent_find(endpoints.begin(), endpoints.end()) != endpoints.end())
        fail("Workspace endpoints must be unique");
    std::sort(session_ids.begin(), session_ids.end());
    if (std::adjacent_find(session_ids.begin(), session_ids.end()) != session_ids.end())
        fail("Workspace session IDs must be unique");
}

void validate_directory_status(const struct stat& status) {
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::getuid() ||
        (status.st_mode & 07777U) != 0700U)
        fail("Workspace owner directory must be a private regular directory");
}

void ensure_private_owner_directory(const QString& path) {
    const auto native = QFile::encodeName(path);
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST)
        fail("Could not create private workspace directory");
    struct stat status{};
    if (::lstat(native.constData(), &status) != 0)
        fail("Could not inspect workspace owner directory");
    validate_directory_status(status);
}

posix::UniqueFd open_private_regular_file(const QString& path, int flags) {
    const int descriptor = ::open(
        QFile::encodeName(path).constData(),
        static_cast<int>(static_cast<unsigned int>(flags) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK),
        0600);
    posix::UniqueFd result{descriptor};
    struct stat status{};
    if (!result || ::fstat(result.get(), &status) != 0)
        fail("Could not open private workspace regular file");
    if (!S_ISREG(status.st_mode) || status.st_uid != ::getuid() ||
        (status.st_mode & 07777U) != 0600U || status.st_nlink != 1)
        fail("Workspace storage must be a private regular file with one link");
    return result;
}

void fsync_directory(const QString& path) {
    posix::UniqueFd directory{::open(QFile::encodeName(path).constData(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat status{};
    if (!directory || ::fstat(directory.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::getuid() || (status.st_mode & 07777U) != 0700U ||
        ::fsync(directory.get()) != 0)
        fail("Could not synchronize workspace owner directory");
}

QByteArray encode_entries(const std::vector<WorkspaceEntry>& entries) {
    QJsonArray array;
    for (const auto& entry : entries) {
        QJsonObject object;
        object.insert(QLatin1String(endpoint_key), normalized_endpoint(entry.endpoint));
        object.insert(QLatin1String(session_id_key),
                      QString::fromLatin1(entry.identity.session_id.toHex()));
        object.insert(QLatin1String(epoch_key), QString::fromLatin1(entry.identity.epoch.toHex()));
        object.insert(QLatin1String(fingerprint_key),
                      QString::fromLatin1(entry.fingerprint.toHex()));
        object.insert(QLatin1String(title_key), entry.title);
        object.insert(QLatin1String(directory_key), entry.directory);
        object.insert(QLatin1String(agent_key), entry.agent == session::AgentMode::terminal
                                                    ? QStringLiteral("terminal")
                                                    : QStringLiteral("codex"));
        array.append(std::move(object));
    }
    QJsonObject document;
    document.insert(QLatin1String(schema_key), 1);
    document.insert(QLatin1String(entries_key), std::move(array));
    const QByteArray bytes = QJsonDocument{document}.toJson(QJsonDocument::Compact);
    if (bytes.size() > max_json_bytes)
        fail("Workspace registry document exceeds 64 KiB");
    return bytes;
}

QByteArray read_all(const QString& path) {
    auto file = open_private_regular_file(path, O_RDONLY);
    struct stat status{};
    if (::fstat(file.get(), &status) != 0 || status.st_size < 0 ||
        status.st_size > max_json_bytes || status.st_nlink != 1)
        fail("Workspace registry exceeds 64 KiB or is hard-linked");
    QByteArray bytes;
    bytes.resize(static_cast<qsizetype>(status.st_size));
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(file.get(), bytes.data() + offset,
                                     static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            fail("Could not read workspace registry");
        }
        if (count == 0)
            fail("Workspace registry changed while it was being read");
        offset += count;
    }
    if (::fstat(file.get(), &status) != 0 || status.st_size != bytes.size() || status.st_nlink != 1)
        fail("Workspace registry changed while it was being read");
    return bytes;
}

std::vector<WorkspaceEntry> decode_entries(const QByteArray& bytes) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail("Workspace registry JSON is corrupt");
    const QJsonObject root = document.object();
    if (root.size() != 2 || !root.contains(QLatin1String(schema_key)) ||
        !root.contains(QLatin1String(entries_key)))
        fail("Workspace registry contains unknown fields");
    if (root.value(QLatin1String(schema_key)) != QJsonValue{1})
        fail("Workspace registry schema version is unsupported");
    const QJsonValue entries_value = root.value(QLatin1String(entries_key));
    if (!entries_value.isArray())
        fail("Workspace registry entries are corrupt");
    const QJsonArray array = entries_value.toArray();
    if (static_cast<std::size_t>(array.size()) > WorkspaceRegistry::maximum_entries)
        fail("Workspace registry contains too many entries");
    std::vector<WorkspaceEntry> entries;
    entries.reserve(static_cast<std::size_t>(array.size()));
    for (const auto& value : array) {
        if (!value.isObject())
            fail("Workspace registry entry is corrupt");
        const QJsonObject object = value.toObject();
        if (object.size() != 7)
            fail("Workspace registry entry contains unknown or missing fields");
        WorkspaceEntry entry;
        const auto string_field = [&object](const char* key) {
            const QJsonValue value = object.value(QLatin1String(key));
            if (!value.isString())
                fail("Workspace registry string field is missing or corrupt");
            return value.toString();
        };
        entry.endpoint = string_field(endpoint_key);
        const auto hex_field = [&](const char* key) {
            const auto encoded = string_field(key);
            const auto decoded = QByteArray::fromHex(encoded.toLatin1());
            if (QString::fromLatin1(decoded.toHex()) != encoded)
                fail("Workspace identity encoding is invalid");
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
        else
            fail("Workspace agent mode is unknown");
        entries.push_back(std::move(entry));
    }
    validate_entries(entries);
    return entries;
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

void write_exact(int descriptor, const QByteArray& bytes) {
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.constData() + offset,
                                      static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            fail("Could not write workspace registry");
        offset += count;
    }
}
} // namespace

class WorkspaceRegistry::Impl final {
  public:
    explicit Impl(const QString& path) {
        if (path.isEmpty() || path.contains(QChar::Null) || QFileInfo(path).isRelative())
            fail("Workspace registry path must be absolute");
        const QFileInfo info{path};
        if (info.fileName().isEmpty() || info.fileName() == QLatin1String(".") ||
            info.fileName() == QLatin1String("..") || info.isSymLink())
            fail("Workspace registry filename is unsafe");
        owner_directory_ = info.absolutePath();
        storage_path_ = QDir(owner_directory_).filePath(info.fileName());
        ensure_private_owner_directory(owner_directory_);
        // Reuse the endpoint ancestor trust boundary for the registry directory.
        owner_directory_ =
            QFileInfo(session::posix::prepare_endpoint(
                          QDir(owner_directory_).filePath(QStringLiteral("registry.guard"))))
                .absolutePath();
        storage_path_ = QDir(owner_directory_).filePath(info.fileName());
        if (QFileInfo::exists(storage_path_) || QFileInfo(storage_path_).isSymLink()) {
            auto existing = open_private_regular_file(storage_path_, O_RDONLY);
            static_cast<void>(existing);
        }
        lock_ = open_private_regular_file(lock_path(), O_RDWR | O_CREAT);
        if (::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0)
            fail("Workspace registry is already locked");
    }

    [[nodiscard]] std::vector<WorkspaceEntry> read() const {
        if (!QFileInfo::exists(storage_path_) && !QFileInfo(storage_path_).isSymLink())
            return {};
        return decode_entries(read_all(storage_path_));
    }

    void write(const std::vector<WorkspaceEntry>& entries) {
        validate_entries(entries);
        const QByteArray bytes = encode_entries(entries);
        if (QFileInfo::exists(storage_path_) || QFileInfo(storage_path_).isSymLink()) {
            auto existing = open_private_regular_file(storage_path_, O_RDONLY);
            static_cast<void>(existing);
        }
        // Preserve external corruption rather than replacing evidence with a fresh snapshot.
        if (QFileInfo::exists(storage_path_))
            static_cast<void>(decode_entries(read_all(storage_path_)));
        const QString temporary_path =
            owner_directory_ + QStringLiteral("/.registry.") +
            QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex()) + QStringLiteral(".tmp");
        TemporaryFile temporary{temporary_path};
        write_exact(temporary.get(), bytes);
        if (::fsync(temporary.get()) != 0)
            fail("Could not synchronize temporary workspace registry");
        if (::rename(QFile::encodeName(temporary_path).constData(),
                     QFile::encodeName(storage_path_).constData()) != 0)
            fail("Could not commit workspace registry");
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
