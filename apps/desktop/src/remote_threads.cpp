#include "remote_threads.hpp"

#include "platform/updater_process.hpp"
#include "remote_thread_script.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr auto owner_folder = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
constexpr auto owner_file = QFile::ReadOwner | QFile::WriteOwner;
// The helper sends at most 256 KiB a frame; anything far larger is not it.
constexpr quint32 max_frame = 8U * 1024 * 1024;
constexpr qsizetype max_errors = 4096;
constexpr qint64 head_bytes = 4096;
constexpr int upload_timeout_ms = 120'000;

bool uuid(const QString& text) {
    return QUuid::fromString(text).toString(QUuid::WithoutBraces) == text && text == text.toLower();
}
// A machine name as a folder name: its own characters (validMachine allows
// letters, digits, '.', '_', ':' and '-'), ':' made '_'.
QString machineFolder(const QString& machine) {
    static const QRegularExpression name(QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$)"));
    if (!name.match(machine).hasMatch())
        return {};
    auto folder = machine;
    folder.replace(QLatin1Char(':'), QLatin1Char('_'));
    return folder;
}
bool appendPrivate(const QString& path, QByteArrayView bytes) {
    QFile file(path);
    const bool fresh = !file.exists();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    if (fresh)
        file.setPermissions(owner_file);
    return file.write(bytes.data(), bytes.size()) == bytes.size();
}
bool truncatePrivate(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.setPermissions(owner_file);
    return true;
}
QString headHash(const QString& path, qint64 length) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(QCryptographicHash::hash(file.read(std::min(length, head_bytes)),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}
QString sshFailure(int code, const QByteArray& errors, const QString& machine) {
    const auto text = QString::fromUtf8(errors);
    if (code == 255) {
        for (const auto* sign : {"Permission denied", "Host key verification failed", "password",
                                 "passphrase", "publickey"})
            if (text.contains(QLatin1String(sign), Qt::CaseInsensitive))
                return QStringLiteral("ssh to %1 needs a password or a key it cannot use "
                                      "without asking")
                    .arg(machine);
        return QStringLiteral("Lost the connection to %1 (retrying)").arg(machine);
    }
    if (code == 127 || (text.contains(QLatin1String("python3")) &&
                        text.contains(QLatin1String("not found"), Qt::CaseInsensitive)))
        return QStringLiteral("python3 is not installed on %1").arg(machine);
    return {};
}
} // namespace

QStringList remote_helper_options() {
    return {QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
            QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
            QStringLiteral("-o"), QStringLiteral("ControlPath=none"),
            QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=15"),
            QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=4"),
            QStringLiteral("-T")};
}

QString remote_shell_word(const QString& text) {
    static const QRegularExpression plain(QStringLiteral(R"(^[A-Za-z0-9_./=:@%+,-]+$)"));
    if (plain.match(text).hasMatch())
        return text;
    auto quoted = text;
    quoted.replace(QLatin1Char('\''), QStringLiteral(R"('\'')"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

struct RemoteThreads::Mirror {
    enum class Phase : std::uint8_t { idle, connecting, waiting, following, retrying, failed };
    QString agent;
    Target target;
    QString folder; // the cache folder for its machine
    QPointer<UpdaterProcess> process;
    QByteArray buffer;
    QByteArray errors;
    QString error_frame; // the helper's own E reason
    Phase phase{Phase::idle};
    QString reason;
    bool hello{}; // this attempt said hello
    bool stopping{};
    qint64 remote_size{};
    qint64 inode{};
    qint64 size{};   // bytes in the mirror
    bool complete{}; // the mirror once held all the file had
    bool tail_active{};
    qint64 tail_base{};
    qint64 tail_size{};
    qint64 tail_goal{};
    std::size_t attempts{};
    QTimer retry;
    qint64 wanted_ms{};
    QString shown_path; // what file() last returned
    [[nodiscard]] QString full() const {
        return folder + QLatin1Char('/') + target.conversation + QStringLiteral(".jsonl");
    }
    [[nodiscard]] QString tail() const {
        return folder + QLatin1Char('/') + target.conversation + QStringLiteral(".tail.jsonl");
    }
    [[nodiscard]] QString meta() const {
        return folder + QLatin1Char('/') + target.conversation + QStringLiteral(".meta.json");
    }
    [[nodiscard]] bool active() const { return process != nullptr || retry.isActive(); }
};

RemoteThreads::RemoteThreads(QString root, Resolve resolve, QObject* parent)
    : QObject(parent), root_(std::move(root)), resolve_(std::move(resolve)) {
    check_.setInterval(static_cast<int>(timing_.check.count()));
    connect(&check_, &QTimer::timeout, this, &RemoteThreads::check);
}

RemoteThreads::~RemoteThreads() {
    // The helpers end with their process groups; wait briefly for each, so
    // none is destroyed while it runs.
    for (auto& mirror : mirrors_)
        stop(*mirror);
    // Every process child is an UpdaterProcess (start, upload).
    for (auto* process : findChildren<QProcess*>(Qt::FindDirectChildrenOnly)) {
        QObject::disconnect(process, nullptr, this, nullptr);
        static_cast<UpdaterProcess*>(process)->stopGroup();
        if (process->state() != QProcess::NotRunning)
            process->waitForFinished(1000);
    }
}

void RemoteThreads::setTimingForTesting(Timing timing) {
    timing_ = std::move(timing);
    check_.setInterval(static_cast<int>(timing_.check.count()));
}

RemoteThreads::Mirror* RemoteThreads::mirror(const QString& agent) const {
    const auto found = std::ranges::find_if(
        mirrors_, [&agent](const auto& mirror) { return mirror->agent == agent; });
    return found == mirrors_.end() ? nullptr : found->get();
}

QString RemoteThreads::cacheFolder(const QString& machine) const {
    const auto folder = machineFolder(machine);
    if (root_.isEmpty() || folder.isEmpty())
        return {};
    return QDir(root_).filePath(QStringLiteral("remote/") + folder);
}

RemoteThreads::Mirror& RemoteThreads::ensure(const QString& agent, const Target& target) {
    auto* existing = mirror(agent);
    if (existing != nullptr && existing->target == target)
        return *existing;
    if (existing == nullptr) {
        mirrors_.push_back(std::make_unique<Mirror>());
        existing = mirrors_.back().get();
        existing->agent = agent;
        existing->retry.setSingleShot(true);
        connect(&existing->retry, &QTimer::timeout, this, [this, agent] {
            if (auto* again = mirror(agent); again != nullptr && again->process == nullptr)
                start(*again);
        });
    } else {
        // Another conversation (after /clear or /resume) or machine.
        stop(*existing);
    }
    auto& mirror = *existing;
    mirror.target = target;
    mirror.folder = cacheFolder(target.machine);
    mirror.phase = Mirror::Phase::idle;
    mirror.reason.clear();
    mirror.attempts = 0;
    mirror.shown_path.clear();
    open(mirror);
    return mirror;
}

void RemoteThreads::open(Mirror& mirror) {
    QDir().mkpath(mirror.folder);
    QFile::setPermissions(QDir(root_).filePath(QStringLiteral("remote")), owner_folder);
    QFile::setPermissions(mirror.folder, owner_folder);
    mirror.size = QFileInfo(mirror.full()).size();
    mirror.complete = false;
    mirror.inode = 0;
    QFile meta(mirror.meta());
    if (mirror.size > 0 && meta.open(QIODevice::ReadOnly)) {
        const auto saved = QJsonDocument::fromJson(meta.read(4096)).object();
        mirror.inode = saved.value(QStringLiteral("inode")).toInteger();
        mirror.complete = saved.value(QStringLiteral("complete")).toBool() &&
                          saved.value(QStringLiteral("size")).toInteger() == mirror.size;
    }
    // A tail from an earlier run is fetched again with the rest.
    QFile::remove(mirror.tail());
    mirror.tail_active = false;
    mirror.tail_base = mirror.tail_size = mirror.tail_goal = 0;
    // Shown now: the least recently shown conversations leave the cache first.
    saveMeta(mirror);
}

void RemoteThreads::saveMeta(const Mirror& mirror) const {
    if (mirror.folder.isEmpty())
        return;
    QFile meta(mirror.meta());
    if (!meta.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    meta.setPermissions(owner_file);
    meta.write(QJsonDocument(QJsonObject{{"inode", mirror.inode},
                                         {"complete", mirror.complete},
                                         {"size", mirror.size},
                                         {"machine", mirror.target.machine}})
                   .toJson(QJsonDocument::Compact));
}

QString RemoteThreads::path(const Mirror& mirror) const {
    if (mirror.tail_active && mirror.tail_size > 0)
        return mirror.tail();
    if (mirror.complete && mirror.size > 0)
        return mirror.full();
    return {};
}

QString RemoteThreads::file(const QString& agent) {
    if (root_.isEmpty() || !resolve_)
        return {};
    const auto target = resolve_(agent);
    if (!target || !uuid(target->conversation) || cacheFolder(target->machine).isEmpty())
        return {};
    auto& mirror = ensure(agent, *target);
    mirror.wanted_ms = QDateTime::currentMSecsSinceEpoch();
    if (!mirror.active())
        start(mirror);
    if (!check_.isActive())
        check_.start();
    mirror.shown_path = path(mirror);
    return mirror.shown_path;
}

void RemoteThreads::setShown(const QString& agent) {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    // The one left keeps its helper for a while from now.
    if (auto* left = mirror(shown_); left != nullptr)
        left->wanted_ms = now;
    shown_ = agent;
    if (auto* shown = mirror(agent); shown != nullptr)
        shown->wanted_ms = now;
}

QStringList RemoteThreads::helperArguments(const Mirror& mirror) const {
    QStringList words{
        QStringLiteral("python3"),  QStringLiteral("-"),         QStringLiteral("follow"),
        QStringLiteral("--cli"),    QStringLiteral("claude"),    QStringLiteral("--conversation"),
        mirror.target.conversation, QStringLiteral("--folder"),  mirror.target.folder,
        QStringLiteral("--offset"), QString::number(mirror.size)};
    if (mirror.size > 0) {
        words << QStringLiteral("--head") << headHash(mirror.full(), mirror.size);
        if (mirror.inode > 0)
            words << QStringLiteral("--inode") << QString::number(mirror.inode);
    }
    // Nothing whole to show yet: its end first.
    if (!mirror.complete)
        words << QStringLiteral("--tail-bytes") << QString::number(timing_.tail_bytes);
    QStringList quoted;
    for (const auto& word : words)
        quoted << remote_shell_word(word);
    return remote_helper_options() +
           QStringList{QStringLiteral("--"), mirror.target.machine, quoted.join(QLatin1Char(' '))};
}

void RemoteThreads::start(Mirror& mirror) {
    if (mirror.process != nullptr || mirror.target.ssh.isEmpty())
        return;
    auto* process = new UpdaterProcess(this);
    mirror.process = process; // followed now, so trimming keeps it
    trimCache();
    // The mirror may have changed on disk since (removed by hand, say).
    if (const auto on_disk = QFileInfo(mirror.full()).size(); on_disk != mirror.size) {
        mirror.size = on_disk;
        mirror.complete = false;
        mirror.inode = 0;
    }
    mirror.buffer.clear();
    mirror.errors.clear();
    mirror.error_frame.clear();
    mirror.hello = false;
    mirror.stopping = false;
    mirror.tail_active = false;
    mirror.tail_base = mirror.tail_size = mirror.tail_goal = 0;
    if (mirror.phase != Mirror::Phase::retrying && mirror.phase != Mirror::Phase::failed)
        mirror.phase = Mirror::Phase::connecting;
    process->setProgram(mirror.target.ssh);
    process->setArguments(helperArguments(mirror));
    const QString agent = mirror.agent;
    const auto current = [this, agent, process]() -> Mirror* {
        auto* found = this->mirror(agent);
        return found != nullptr && found->process == process ? found : nullptr;
    };
    connect(process, &QProcess::readyReadStandardOutput, this, [this, current] {
        if (auto* found = current())
            receive(*found);
    });
    connect(process, &QProcess::readyReadStandardError, this, [current, process] {
        const auto bytes = process->readAllStandardError();
        if (auto* found = current(); found != nullptr && found->errors.size() < max_errors)
            found->errors += bytes.left(max_errors - found->errors.size());
    });
    connect(process, &QProcess::finished, this,
            [this, current, process](int code, QProcess::ExitStatus status) {
                if (auto* found = current()) {
                    receive(*found);
                    finished(*found, code, status == QProcess::CrashExit);
                }
                process->deleteLater();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, current, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                if (auto* found = current()) {
                    found->error_frame = QStringLiteral("ssh is not available on this Mac");
                    finished(*found, -1, true);
                }
                process->deleteLater();
            });
    process->start();
    process->write(kRemoteThreadScript, sizeof(kRemoteThreadScript) - 1);
    process->closeWriteChannel();
    notify(agent);
}

void RemoteThreads::notify(const QString& agent) {
    // Queued: file() runs inside the view's bindings, which must not be told
    // to evaluate again while they do.
    QMetaObject::invokeMethod(this, [this, agent] { emit changed(agent); }, Qt::QueuedConnection);
}

void RemoteThreads::stop(Mirror& mirror) {
    mirror.retry.stop();
    if (mirror.process != nullptr) {
        auto* process = mirror.process.data();
        mirror.process.clear();
        QObject::disconnect(process, nullptr, this, nullptr);
        process->stopGroup();
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
        if (process->state() == QProcess::NotRunning)
            process->deleteLater();
    }
    mirror.phase = Mirror::Phase::idle;
}

void RemoteThreads::killForTesting(const QString& agent) {
    if (auto* found = mirror(agent); found != nullptr && found->process != nullptr)
        found->process->stopGroup();
}

bool RemoteThreads::running(const QString& agent) const {
    const auto* found = mirror(agent);
    return found != nullptr && found->process != nullptr;
}

void RemoteThreads::receive(Mirror& mirror) {
    if (mirror.process == nullptr)
        return;
    mirror.buffer += mirror.process->readAllStandardOutput();
    const auto before = mirror.shown_path;
    const auto phase = mirror.phase;
    qsizetype at = 0;
    bool ok = true;
    while (mirror.buffer.size() - at >= 5) {
        const auto type = mirror.buffer.at(at);
        const auto length = qFromBigEndian<quint32>(mirror.buffer.constData() + at + 1);
        if (length > max_frame) {
            ok = false;
            break;
        }
        if (mirror.buffer.size() - at - 5 < static_cast<qsizetype>(length))
            break;
        if (!handle(mirror, type, mirror.buffer.mid(at + 5, length))) {
            ok = false;
            break;
        }
        at += 5 + static_cast<qsizetype>(length);
    }
    mirror.buffer.remove(0, at);
    if (!ok) {
        // Not the helper's frames, or out of order: start over from the cache.
        mirror.error_frame = QStringLiteral("The thread from %1 was out of order; reconnecting")
                                 .arg(mirror.target.machine);
        if (mirror.process != nullptr)
            mirror.process->stopGroup();
    }
    mirror.shown_path = path(mirror);
    if (mirror.shown_path != before || mirror.phase != phase)
        emit changed(mirror.agent);
}

// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool RemoteThreads::handle(Mirror& mirror, char type, const QByteArray& payload) {
    const auto chunk = [&payload](qint64& offset) {
        if (payload.size() < 8)
            return false;
        offset = qFromBigEndian<qint64>(payload.constData());
        return offset >= 0;
    };
    switch (type) {
    case 'H': {
        const auto hello = QJsonDocument::fromJson(payload).object();
        mirror.hello = true;
        mirror.attempts = 0;
        mirror.phase = Mirror::Phase::following;
        mirror.reason.clear();
        mirror.remote_size = hello.value(QStringLiteral("size")).toInteger();
        mirror.inode = hello.value(QStringLiteral("inode")).toInteger();
        if (!mirror.complete && mirror.size >= mirror.remote_size) {
            mirror.complete = true;
        }
        saveMeta(mirror);
        return true;
    }
    case 'M':
        mirror.phase = Mirror::Phase::waiting;
        mirror.reason =
            QStringLiteral("The conversation file is not on %1 yet").arg(mirror.target.machine);
        return true;
    case 'R':
        reset(mirror);
        return true;
    case 'T': {
        qint64 offset{};
        if (!chunk(offset))
            return false;
        const auto bytes = QByteArrayView(payload).sliced(8);
        if (!mirror.tail_active) {
            if (!truncatePrivate(mirror.tail()))
                return false;
            mirror.tail_active = true;
            mirror.tail_base = offset;
            mirror.tail_size = 0;
        } else if (offset != mirror.tail_base + mirror.tail_size) {
            return false;
        }
        if (!appendPrivate(mirror.tail(), bytes))
            return false;
        mirror.tail_size += bytes.size();
        mirror.tail_goal = offset + bytes.size();
        return true;
    }
    case 'D': {
        qint64 offset{};
        if (!chunk(offset) || offset != mirror.size)
            return false;
        const auto bytes = QByteArrayView(payload).sliced(8);
        if (!appendPrivate(mirror.full(), bytes))
            return false;
        mirror.size += bytes.size();
        if (mirror.tail_active && mirror.size >= mirror.tail_goal) {
            // The mirror reached the tail's end: the thread moves to it.
            mirror.tail_active = false;
            const auto tail = mirror.tail();
            QTimer::singleShot(10000, this, [this, agent = mirror.agent, tail] {
                const auto* again = this->mirror(agent);
                if (again == nullptr || !again->tail_active || again->tail() != tail)
                    QFile::remove(tail);
            });
        }
        if (mirror.hello && !mirror.complete && mirror.size >= mirror.remote_size) {
            mirror.complete = true;
            saveMeta(mirror);
        }
        return true;
    }
    case 'E':
        mirror.error_frame =
            QJsonDocument::fromJson(payload).object().value(QStringLiteral("reason")).toString();
        return true;
    case 'K':
        return true;
    default:
        return false;
    }
}

void RemoteThreads::reset(Mirror& mirror) {
    truncatePrivate(mirror.full());
    QFile::remove(mirror.tail());
    mirror.size = 0;
    mirror.complete = false;
    // Whole again only at the size the hello after a reset reports.
    mirror.remote_size = std::numeric_limits<qint64>::max();
    mirror.tail_active = false;
    mirror.tail_base = mirror.tail_size = mirror.tail_goal = 0;
    saveMeta(mirror);
}

void RemoteThreads::finished(Mirror& mirror, int code, bool crashed) {
    mirror.process.clear();
    if (mirror.stopping) {
        mirror.phase = Mirror::Phase::idle;
        return;
    }
    if (mirror.complete)
        saveMeta(mirror);
    auto why = sshFailure(crashed ? -1 : code, mirror.errors, mirror.target.machine);
    if (why.isEmpty())
        why = mirror.error_frame;
    if (why.isEmpty())
        why =
            mirror.hello
                ? QStringLiteral("Lost the connection to %1 (retrying)").arg(mirror.target.machine)
                : QStringLiteral("The thread helper on %1 stopped (exit %2)")
                      .arg(mirror.target.machine)
                      .arg(code);
    mirror.reason = std::move(why);
    mirror.phase = mirror.hello ? Mirror::Phase::retrying : Mirror::Phase::failed;
    retry(mirror);
    mirror.shown_path = path(mirror);
    emit changed(mirror.agent);
}

void RemoteThreads::retry(Mirror& mirror) {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const bool wanted = mirror.agent == shown_ || now - mirror.wanted_ms < timing_.keep.count();
    if (!wanted || timing_.waits.empty())
        return;
    const auto wait = timing_.waits.at(std::min(mirror.attempts, timing_.waits.size() - 1));
    ++mirror.attempts;
    mirror.retry.start(static_cast<int>(wait.count()));
    if (!check_.isActive())
        check_.start();
}

void RemoteThreads::check() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    bool any = false;
    for (auto& owned : mirrors_) {
        auto& mirror = *owned;
        if (!mirror.active())
            continue;
        const bool wanted = mirror.agent == shown_ || now - mirror.wanted_ms < timing_.keep.count();
        if (!wanted) {
            stop(mirror);
            notify(mirror.agent);
            continue;
        }
        // The agent's CLI moved to another conversation (/clear, /resume):
        // the mirror follows it.
        if (const auto target = resolve_ ? resolve_(mirror.agent) : std::nullopt;
            target && uuid(target->conversation) && !(*target == mirror.target)) {
            auto& moved = ensure(mirror.agent, *target);
            moved.wanted_ms = now;
            start(moved);
            moved.shown_path = path(moved);
            notify(moved.agent);
        }
        any = true;
    }
    if (!any)
        check_.stop();
}

bool RemoteThreads::supported(const QString& agent) const {
    const auto* found = mirror(agent);
    if (found == nullptr)
        return true;
    const bool failing =
        found->phase == Mirror::Phase::failed || found->phase == Mirror::Phase::retrying;
    return !failing || !path(*found).isEmpty();
}

QString RemoteThreads::unavailable(const QString& agent) const {
    const auto* found = mirror(agent);
    if (found == nullptr || found->phase == Mirror::Phase::following ||
        found->phase == Mirror::Phase::connecting || found->phase == Mirror::Phase::idle)
        return {};
    return found->reason;
}

QVariantMap RemoteThreads::state(const QString& agent) const {
    const auto* found = mirror(agent);
    if (found == nullptr)
        return {};
    static const std::map<Mirror::Phase, const char*> names{
        {Mirror::Phase::idle, "idle"},         {Mirror::Phase::connecting, "connecting"},
        {Mirror::Phase::waiting, "waiting"},   {Mirror::Phase::following, "following"},
        {Mirror::Phase::retrying, "retrying"}, {Mirror::Phase::failed, "failed"}};
    const bool cached = !path(*found).isEmpty();
    const bool failing =
        found->phase == Mirror::Phase::failed || found->phase == Mirror::Phase::retrying;
    const auto& machine = found->target.machine;
    QString status;
    if (found->phase == Mirror::Phase::waiting)
        status = QStringLiteral("No conversation saved on %1 yet").arg(machine);
    else if (!cached && (found->phase == Mirror::Phase::connecting ||
                         found->phase == Mirror::Phase::following))
        status = QStringLiteral("Loading the conversation from %1…").arg(machine);
    return {{QStringLiteral("machine"), machine},
            {QStringLiteral("phase"), QString::fromLatin1(names.at(found->phase))},
            {QStringLiteral("reason"), unavailable(agent)},
            {QStringLiteral("status"), status},
            {QStringLiteral("cached"), cached},
            {QStringLiteral("behind"), cached && failing}};
}

void RemoteThreads::forget(const QString& agent) {
    const auto found = std::ranges::find_if(
        mirrors_, [&agent](const auto& mirror) { return mirror->agent == agent; });
    if (found == mirrors_.end())
        return;
    stop(**found);
    mirrors_.erase(found);
}

void RemoteThreads::setCacheLimit(qint64 bytes) {
    cache_limit_ = std::max<qint64>(0, bytes);
    trimCache();
}

void RemoteThreads::trimCache() {
    if (root_.isEmpty())
        return;
    const QDir remote(QDir(root_).filePath(QStringLiteral("remote")));
    if (!remote.exists())
        return;
    // Each conversation's files, by when it was last shown (its meta file is
    // rewritten each time it is).
    struct Entry {
        qint64 bytes{};
        qint64 shown{};
        QStringList files;
    };
    QHash<QString, Entry> conversations;
    qint64 total = 0;
    QDirIterator files(remote.path(), QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto info = files.nextFileInfo();
        const auto key = info.absolutePath() + QLatin1Char('/') +
                         info.fileName().section(QLatin1Char('.'), 0, 0);
        auto& entry = conversations[key];
        entry.bytes += info.size();
        entry.files << info.absoluteFilePath();
        entry.shown = std::max(entry.shown, info.lastModified().toMSecsSinceEpoch());
        total += info.size();
    }
    if (total <= cache_limit_)
        return;
    QSet<QString> followed;
    for (const auto& mirror : mirrors_)
        if (mirror->active() || mirror->agent == shown_)
            followed.insert(mirror->folder + QLatin1Char('/') + mirror->target.conversation);
    QList<std::pair<qint64, QString>> order;
    for (auto entry = conversations.cbegin(); entry != conversations.cend(); ++entry)
        order.append({entry->shown, entry.key()});
    std::ranges::sort(order);
    for (const auto& [shown, key] : order) {
        if (total <= cache_limit_)
            break;
        if (followed.contains(key))
            continue;
        const auto& entry = conversations[key];
        for (const auto& file : entry.files)
            QFile::remove(file);
        total -= entry.bytes;
    }
}

// Positional contract; callers and tests pin the order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters,readability-function-cognitive-complexity)
QString RemoteThreads::upload(const QString& agent, const QString& local, QString* error) {
    const auto refuse = [error](const QString& why) {
        if (error != nullptr)
            *error = why;
        return QString();
    };
    const auto target = resolve_ ? resolve_(agent) : std::nullopt;
    if (!target || target->ssh.isEmpty() || machineFolder(target->machine).isEmpty())
        return refuse(QStringLiteral("This agent is not on another machine."));
    const QFileInfo info(local);
    if (!info.isFile() || !info.isReadable())
        return refuse(QStringLiteral("%1 is not a file lapis can read.").arg(info.fileName()));
    if (info.size() > upload_limit)
        return refuse(QStringLiteral("%1 is larger than 20 MB.").arg(info.fileName()));
    QFile source(local);
    if (!source.open(QIODevice::ReadOnly))
        return refuse(QStringLiteral("%1 could not be read.").arg(info.fileName()));
    const auto data = source.read(upload_limit + 1);
    if (data.size() > upload_limit)
        return refuse(QStringLiteral("%1 is larger than 20 MB.").arg(info.fileName()));
    // An unguessable name that keeps a plain extension, so the CLI reads an
    // image as one.
    static const QRegularExpression extension(QStringLiteral(R"(^[A-Za-z0-9]{1,16}$)"));
    QByteArray random(16, '\0');
    QRandomGenerator::system()->generate(random.begin(), random.end());
    auto name = QString::fromLatin1(random.toHex());
    if (extension.match(info.suffix()).hasMatch())
        name += QLatin1Char('.') + info.suffix();
    // Only lapis-made text in the command: the folder, the name and its
    // extension, checked above.
    const auto command =
        QStringLiteral(R"(umask 077 && d="$HOME/.cache/lapis/attachments" && mkdir -p "$d" && )"
                       R"({ cat > "$d/%1.part" && mv -f "$d/%1.part" "$d/%1" && )"
                       R"(printf '%s\n' "$d/%1"; } || { rm -f "$d/%1.part"; exit 1; })")
            .arg(name);
    const auto token = QStringLiteral("upload-%1").arg(++upload_serial_);
    auto* process = new UpdaterProcess(this);
    uploads_.insert(token, process);
    process->setProgram(target->ssh);
    process->setArguments(remote_helper_options() +
                          QStringList{QStringLiteral("--"), target->machine, command});
    const auto total = static_cast<double>(std::max<qsizetype>(1, data.size()));
    auto written = std::make_shared<qint64>(0);
    connect(process, &QIODevice::bytesWritten, this, [this, token, written, total](qint64 bytes) {
        *written += bytes;
        emit uploadProgress(token, std::min(1.0, static_cast<double>(*written) / total));
    });
    auto done = std::make_shared<bool>(false);
    const auto finish = [this, token, process, done, name,
                         machine = target->machine](int code, bool started) {
        if (std::exchange(*done, true))
            return;
        uploads_.remove(token);
        const auto output = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        const auto errors = process->readAllStandardError();
        process->deleteLater();
        if (started && code == 0 && output.startsWith(QLatin1Char('/')) &&
            output.endsWith(QLatin1Char('/') + name) && !output.contains(QLatin1Char('\n'))) {
            emit uploadFinished(token, output, {});
            return;
        }
        auto why = started ? sshFailure(code, errors, machine) : QString();
        if (why.isEmpty())
            why = started ? QStringLiteral("Could not copy the file to %1").arg(machine)
                          : QStringLiteral("ssh is not available on this Mac");
        emit uploadFinished(token, {}, why);
    };
    connect(process, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::CrashExit ? -1 : code, true);
    });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError failure) {
        if (failure == QProcess::FailedToStart)
            finish(-1, false);
    });
    // A transfer that stalls ends; its failure keeps the message.
    QTimer::singleShot(upload_timeout_ms, process, [process] { process->stopGroup(); });
    process->start();
    process->write(data);
    process->closeWriteChannel();
    return token;
}

} // namespace lapis::desktop
