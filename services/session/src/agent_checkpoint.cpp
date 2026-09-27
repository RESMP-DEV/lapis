#include "agent_checkpoint.hpp"

#include "platform/posix/unique_fd.hpp"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace lapis::session {
namespace {
constexpr QByteArrayView marker{"\x1b]1337;SetUserVar=agent_checkpoint="};
constexpr qsizetype max_sequence = 8192;
constexpr qsizetype max_record = 4096;
// Codex writes base instructions into the first line; tens of KiB in practice.
constexpr qint64 max_rollout_header = qint64{1024} * 1024;
constexpr std::array known_agents{"claude", "codex", "grok",   "opencode",
                                  "omp",    "kimi",  "gemini", "agy"};

// A rollout's first line (session_meta) marks a subagent thread with a
// {"subagent": ...} source and its parent. Unreadable, malformed or oversized
// metadata cannot attest a main thread.
std::optional<bool> subagent_rollout(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    QJsonParseError parse;
    const auto header = QJsonDocument::fromJson(file.readLine(max_rollout_header), &parse).object();
    if (parse.error != QJsonParseError::NoError ||
        header.value(QStringLiteral("type")).toString() != QStringLiteral("session_meta") ||
        !header.value(QStringLiteral("payload")).isObject())
        return std::nullopt;
    const auto meta = header.value(QStringLiteral("payload")).toObject();
    return meta.value(QStringLiteral("source")).toObject().contains(QStringLiteral("subagent")) ||
           !meta.value(QStringLiteral("parent_thread_id")).toString().isEmpty();
}

bool known_agent(const QString& agent) {
    return std::any_of(known_agents.begin(), known_agents.end(),
                       [&](const char* name) { return agent == QLatin1String(name); });
}

// Checkpoints from agents on other hosts (an ssh session inside the agent's
// terminal) cannot be resumed here.
bool local_host(const QString& host) {
    if (host.isEmpty())
        return true;
    const auto strip = [](QString name) {
        name = name.toLower();
        if (name.endsWith(QLatin1String(".local")))
            name.chop(6);
        return name;
    };
    return strip(host) == strip(QSysInfo::machineHostName());
}

std::optional<ResumeRecord> decode(QByteArrayView value) {
    const auto decoded = QByteArray::fromBase64Encoding(value.toByteArray(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > max_record)
        return std::nullopt;
    const auto document = QJsonDocument::fromJson(decoded.decoded);
    if (!document.isObject())
        return std::nullopt;
    const auto object = document.object();
    ResumeRecord record;
    record.agent = object.value(QStringLiteral("agent")).toString().toLower();
    record.session_id = object.value(QStringLiteral("session_id")).toString();
    if ((object.contains(QStringLiteral("version")) &&
         object.value(QStringLiteral("version")).toInt() != 1) ||
        !known_agent(record.agent) || !valid_resume_identity(record.session_id) ||
        !local_host(object.value(QStringLiteral("host")).toString()))
        return std::nullopt;
    return record;
}

QString record_path(const QString& endpoint) { return endpoint + QStringLiteral(".resume"); }

// An existing record must be a private regular file owned by this user.
bool safe_existing(const QString& path) {
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0)
        return errno == ENOENT;
    return S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && info.st_nlink == 1 &&
           (info.st_mode & 07777U) == 0600U && info.st_size <= max_record;
}

// Open the record itself without following a final symlink, then trust only
// the open descriptor: a path checked with lstat can be swapped before QFile
// opens it.
bool safe_open_record(const QString& path, posix::UniqueFd& descriptor) {
    descriptor.reset(::open(QFile::encodeName(path).constData(),
                            O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!descriptor)
        return false;
    struct stat info{};
    if (::fstat(descriptor.get(), &info) != 0)
        return false;
    return S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && info.st_nlink == 1 &&
           (info.st_mode & 07777U) == 0600U && info.st_size <= max_record;
}
} // namespace

bool observer_backed_agent(const QString& agent) {
    return agent == QLatin1String("codex") || agent == QLatin1String("claude");
}

bool valid_resume_identity(const QString& value) {
    return !value.isEmpty() && value.size() <= 512 && !value.startsWith(QLatin1Char('-')) &&
           std::none_of(value.begin(), value.end(), [](QChar character) {
               return character.isSpace() || !character.isPrint();
           });
}

QString checkpoint_agent_for_launch(const LaunchSpec& launch) {
    switch (launch.agent) {
    case AgentMode::codex:
        return QStringLiteral("codex");
    case AgentMode::claude:
        return QStringLiteral("claude");
    case AgentMode::terminal:
        break;
    }
    return QFileInfo(launch.program).fileName().toLower();
}

std::optional<ResumeRecord> CheckpointScanner::scan(QByteArrayView output) {
    // Copy only when a sequence may span reads; most output has no carry.
    QByteArray joined;
    QByteArrayView data = output;
    if (!carry_.isEmpty()) {
        joined = carry_ + output.toByteArray();
        data = joined;
    }
    std::optional<ResumeRecord> newest;
    qsizetype from = 0;
    for (;;) {
        const auto start = data.indexOf(marker, from);
        if (start < 0) {
            // Keep only what could be the start of a marker.
            const auto keep = std::min(data.size() - from, marker.size() - 1);
            carry_ = data.last(keep).toByteArray();
            return newest;
        }
        const auto value_start = start + marker.size();
        const auto bell = data.indexOf('\x07', value_start);
        const auto terminator = data.indexOf(QByteArrayView("\x1b\\"), value_start);
        const auto end = bell >= 0 && (terminator < 0 || bell < terminator) ? bell : terminator;
        // A base64 value cannot contain the escape that starts another marker.
        // If one appears first, this sequence has no valid terminator; rescan
        // at that marker instead of gluing the values together and skipping
        // both when decoding fails.
        if (const auto next = data.indexOf(marker, value_start);
            next >= 0 && (end < 0 || next < end)) {
            from = next;
            continue;
        }
        if (end < 0) {
            // Incomplete: keep it unless it has grown beyond any real checkpoint.
            carry_ = data.size() - start > max_sequence ? QByteArray()
                                                        : data.sliced(start).toByteArray();
            return newest;
        }
        if (auto record = decode(data.sliced(value_start, end - value_start)))
            newest = std::move(record);
        from = end + 1;
    }
}

std::vector<QString> codex_threads_from_open_files(const QString& listing) {
    static const QRegularExpression rollout(
        QStringLiteral(R"(^n(\S*/sessions/\S*/(rollout-[^/]*-([0-9a-f]{8}-[0-9a-f]{4}-)"
                       R"([0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}))\.jsonl)$)"),
        QRegularExpression::MultilineOption);
    struct Open {
        QString thread;
        QString name;
        QDateTime written;
    };
    std::vector<Open> open;
    for (auto matches = rollout.globalMatch(listing); matches.hasNext();) {
        const auto match = matches.next();
        const QFileInfo info(match.captured(1));
        if (!info.isFile())
            continue;
        const auto subagent = subagent_rollout(info.filePath());
        if (subagent.value_or(true))
            continue;
        open.push_back({match.captured(3), match.captured(2), info.lastModified()});
    }
    // Rollout names start with their creation time and break ties.
    std::sort(open.begin(), open.end(), [](const Open& left, const Open& right) {
        return left.written != right.written ? left.written > right.written
                                             : left.name > right.name;
    });
    std::vector<QString> threads;
    for (const auto& each : open)
        if (std::find(threads.begin(), threads.end(), each.thread) == threads.end())
            threads.push_back(each.thread);
    return threads;
}

std::optional<QString> codex_thread_from_open_files(const QString& listing) {
    const auto threads = codex_threads_from_open_files(listing);
    if (threads.empty())
        return std::nullopt;
    return threads.front();
}

std::optional<ResumeRecord> read_resume_record(const QString& endpoint) {
    const auto path = record_path(endpoint);
    posix::UniqueFd descriptor;
    if (!safe_open_record(path, descriptor))
        return std::nullopt;
    QFile file;
    if (!file.open(descriptor.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        return std::nullopt;
    const auto bytes = file.read(max_record + 1);
    if (bytes.isEmpty() || bytes.size() > max_record)
        return std::nullopt;
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return std::nullopt;
    const auto object = document.object();
    ResumeRecord record;
    record.agent = object.value(QStringLiteral("agent")).toString().toLower();
    record.session_id = object.value(QStringLiteral("session_id")).toString();
    const auto version = object.value(QStringLiteral("version")).toInt();
    if ((version != 1 && version != 2) || !known_agent(record.agent) ||
        !valid_resume_identity(record.session_id))
        return std::nullopt;
    if (version == 2) {
        const auto source = object.value(QStringLiteral("source")).toString();
        if (source != QStringLiteral("observer") && source != QStringLiteral("terminal"))
            return std::nullopt;
        record.source =
            source == QStringLiteral("observer") ? ResumeSource::observer : ResumeSource::terminal;
    } else {
        // Written before lapis recorded how the identity was learned.
        record.source = ResumeSource::legacy;
    }
    return record;
}

void write_resume_record(const QString& endpoint, const ResumeRecord& record) {
    const auto path = record_path(endpoint);
    if (!known_agent(record.agent) || !valid_resume_identity(record.session_id) ||
        record.source == ResumeSource::legacy)
        throw std::invalid_argument("Invalid resume record");
    if (!safe_existing(path))
        throw std::runtime_error("Unsafe resume record");
    const QJsonObject object{
        {"version", 2},
        {"agent", record.agent},
        {"session_id", record.session_id},
        {"source", record.source == ResumeSource::observer ? "observer" : "terminal"}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 || !file.commit())
        throw std::runtime_error("Cannot write resume record");
}
} // namespace lapis::session
