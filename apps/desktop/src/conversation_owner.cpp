#include "conversation_owner.hpp"
#include "platform/process_files.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <optional>
#include <utility>

namespace lapis::desktop::conversation_owner {
namespace {
// Times in the files are to the second (agy) and the clocks the same.
constexpr qint64 clock_slack_ms = 2000;

struct Started {
    QString id;
    qint64 ms{};
};

bool uuid(const QString& value) { return !QUuid::fromString(value).isNull(); }

// The first line of a conversation file as JSON, read without the rest.
QJsonObject firstRecord(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readLine(qint64{1} * 1024 * 1024)).object();
}

QString canonical(const QString& path) {
    const auto resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? QDir::cleanPath(path) : resolved;
}

// Is `folder` the folder `directory` names (both cleaned, links resolved)?
bool sameFolder(const QString& folder, const QString& directory) {
    return !folder.isEmpty() && !directory.isEmpty() && canonical(folder) == canonical(directory);
}

qint64 isoMs(const QString& text) {
    const auto at = QDateTime::fromString(text, Qt::ISODateWithMs);
    return at.isValid() ? at.toMSecsSinceEpoch() : 0;
}

// A Codex rollout's thread when it is a TUI thread (not a subagent's or a
// `codex exec` run's): its session_meta has its id, source, start and folder.
struct Rollout {
    Started thread;
    QString folder;
};
std::optional<Rollout> tuiRollout(const QString& path) {
    const auto meta = firstRecord(path).value(QStringLiteral("payload")).toObject();
    const auto id = meta.value(QStringLiteral("id")).toString();
    if (meta.value(QStringLiteral("source")).toString() != QLatin1String("cli") || !uuid(id))
        return std::nullopt;
    return Rollout{{id, isoMs(meta.value(QStringLiteral("timestamp")).toString())},
                   meta.value(QStringLiteral("cwd")).toString()};
}

qint64 antigravityStart(const QString& conversation) {
    const auto path = antigravity_transcript(conversation);
    return path.isEmpty() ? 0
                          : isoMs(firstRecord(path).value(QStringLiteral("created_at")).toString());
}

// Codex TUI threads started in `folder` since `since_ms`, under
// sessions/YYYY/MM/DD (local dates; no symlinks), from the start day on.
QList<Started> codexStarted(const QString& folder, qint64 since_ms) {
    const QDir sessions(codex_home() + QStringLiteral("/sessions"));
    const auto first_day = QDateTime::fromMSecsSinceEpoch(since_ms).date();
    QList<Started> found;
    for (auto day = QDateTime::currentDateTime().date();
         day >= first_day && first_day.daysTo(day) <= 7; day = day.addDays(-1)) {
        // A thread started just after midnight UTC may sit in either
        // neighbouring folder, so the start day counts.
        const QDir folder_of_day(sessions.filePath(day.toString(QStringLiteral("yyyy/MM/dd"))));
        if (QFileInfo(folder_of_day.path()).isSymLink())
            continue;
        for (const auto& name : folder_of_day.entryList({QStringLiteral("rollout-*.jsonl")},
                                                        QDir::Files | QDir::NoSymLinks))
            if (const auto rollout = tuiRollout(folder_of_day.filePath(name));
                rollout && rollout->thread.ms >= since_ms && sameFolder(rollout->folder, folder))
                found.append(rollout->thread);
    }
    return found;
}

// The conversations agy says it used in `folder`: its cache of each
// folder's last one, and its summaries' workspace folders.
QSet<QString> antigravityFolderConversations(const QString& folder) {
    QSet<QString> found;
    const QDir cache(antigravity_home() + QStringLiteral("/cache"));
    const auto read = [&cache](const char* name) {
        QFile file(cache.filePath(QString::fromLatin1(name)));
        if (!file.open(QIODevice::ReadOnly) || file.size() > qint64{32} * 1024 * 1024)
            return QJsonObject{};
        return QJsonDocument::fromJson(file.readAll()).object();
    };
    const auto last = read("last_conversations.json");
    for (auto entry = last.begin(); entry != last.end(); ++entry)
        if (sameFolder(entry.key(), folder))
            found.insert(entry.value().toString());
    const auto summaries =
        read("conversation_metadata.json").value(QStringLiteral("conversations")).toObject();
    for (auto entry = summaries.begin(); entry != summaries.end(); ++entry) {
        const auto uris = entry.value()
                              .toObject()
                              .value(QStringLiteral("summary"))
                              .toObject()
                              .value(QStringLiteral("WorkspaceURIs"))
                              .toArray();
        if (std::any_of(uris.begin(), uris.end(), [&folder](const QJsonValue& uri) {
                const QUrl url(uri.toString());
                return url.isLocalFile() && sameFolder(url.toLocalFile(), folder);
            }))
            found.insert(entry.key());
    }
    return found;
}

// agy conversations begun since `since_ms` (their first step's created_at)
// that agy records as used in `folder`.
QList<Started> antigravityStarted(const QString& folder, qint64 since_ms) {
    const QDir brain(antigravity_home() + QStringLiteral("/brain"));
    QList<Started> found;
    for (const auto& name : brain.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (!uuid(name))
            continue;
        // A cheap first cut; the transcript's own time decides.
        const QFileInfo info(brain.filePath(name));
        const auto made = info.birthTime().isValid() ? info.birthTime() : info.lastModified();
        if (made.toMSecsSinceEpoch() < since_ms - 60'000)
            continue;
        if (const auto started = antigravityStart(name); started >= since_ms)
            found.append({name, started});
    }
    if (found.isEmpty())
        return found;
    const auto here = antigravityFolderConversations(folder);
    found.removeIf(
        [&here](const Started& conversation) { return !here.contains(conversation.id); });
    return found;
}

qint64 serviceStart(const QString& endpoint) {
    const auto born = QFileInfo(endpoint).birthTime();
    return born.isValid() ? born.toMSecsSinceEpoch() : 0;
}
} // namespace

QString codex_home() {
    return qEnvironmentVariable("CODEX_HOME", QDir::homePath() + QStringLiteral("/.codex"));
}

QString antigravity_home() {
    return QDir::home().filePath(QStringLiteral(".gemini/antigravity-cli"));
}

QString antigravity_transcript(const QString& conversation) {
    if (!uuid(conversation))
        return {};
    const auto path = QDir(antigravity_home())
                          .filePath(QStringLiteral("brain/%1/.system_generated/logs/"
                                                   "transcript_full.jsonl")
                                        .arg(conversation));
    return QFileInfo(path).isFile() ? path : QString();
}

qint64 service_pid(const QString& endpoint) {
    // QLockFile's contents: pid, application name, host name, then ids.
    QFile lock(endpoint + QStringLiteral(".lock"));
    if (!lock.open(QIODevice::ReadOnly) || lock.size() > 4096)
        return 0;
    const auto lines = QString::fromUtf8(lock.readAll()).split(QLatin1Char('\n'));
    if (lines.size() < 3 || lines.at(1) != QLatin1String("lapis_session_service") ||
        (!lines.at(2).isEmpty() && lines.at(2) != QSysInfo::machineHostName()))
        return 0;
    bool number = false;
    const auto pid = lines.at(0).toLongLong(&number);
    return number && platform::process_alive(pid) ? pid : 0;
}

// Cohesive positional parser flow; callers and tests pin it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters,readability-function-cognitive-complexity)
QString held_conversation(const QString& harness, const QString& endpoint, const QString& pin) {
    const bool agy = harness == QLatin1String("agy");
    if (!agy && harness != QLatin1String("codex"))
        return {};
    const auto pid = service_pid(endpoint);
    if (pid == 0)
        return {};
    const auto home = QFileInfo(agy ? antigravity_home() : codex_home()).canonicalFilePath();
    if (home.isEmpty())
        return {};
    const auto presence = home + QStringLiteral("/presence/");
    const auto databases = home + QStringLiteral("/conversations/");
    const auto sessions = home + QStringLiteral("/sessions/");
    QList<Started> held;
    QList<Started> opened; // agy's databases, when it holds no presence lock
    const auto add = [](QList<Started>& list, const QString& id, auto start) {
        if (std::none_of(list.cbegin(), list.cend(),
                         [&id](const Started& known) { return known.id == id; }))
            list.append({id, start()});
    };
    for (const auto process : platform::process_tree(pid)) {
        for (const auto& path : platform::open_files(process)) {
            const auto name = QFileInfo(path).fileName();
            if (agy && path.startsWith(presence) && name.endsWith(QLatin1String(".lock"))) {
                if (const auto id = name.chopped(5); uuid(id))
                    add(held, id, [&id] { return antigravityStart(id); });
            } else if (agy && path.startsWith(databases) && name.endsWith(QLatin1String(".db"))) {
                if (const auto id = name.chopped(3); uuid(id))
                    add(opened, id, [&id] { return antigravityStart(id); });
            } else if (!agy && path.startsWith(sessions) &&
                       name.startsWith(QLatin1String("rollout-")) &&
                       name.endsWith(QLatin1String(".jsonl"))) {
                if (const auto rollout = tuiRollout(path))
                    add(held, rollout->thread.id, [&rollout] { return rollout->thread.ms; });
            }
        }
    }
    if (held.isEmpty())
        held = std::move(opened);
    if (held.isEmpty())
        return {};
    if (held.size() > 1 && std::any_of(held.cbegin(), held.cend(),
                                       [&pin](const Started& known) { return known.id == pin; }))
        return pin;
    return std::max_element(
               held.cbegin(), held.cend(),
               [](const Started& left, const Started& right) { return left.ms < right.ms; })
        ->id;
}

// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
QHash<QString, QString> pins(const QList<Agent>& agents) {
    QHash<QString, QString> changed;
    QHash<QString, QString> current;
    const auto handled = [](const Agent& agent) {
        return !agent.recorded &&
               (agent.harness == QLatin1String("agy") || agent.harness == QLatin1String("codex"));
    };
    for (const auto& agent : agents)
        current.insert(agent.id, agent.known);
    // What each agent's own processes hold is its, whatever else is known.
    for (const auto& agent : agents) {
        if (!handled(agent))
            continue;
        if (const auto held = held_conversation(agent.harness, agent.endpoint, agent.known);
            !held.isEmpty() && held != agent.known) {
            changed.insert(agent.id, held);
            current.insert(agent.id, held);
        }
    }
    // Before that, the start time, where it singles out one agent.
    for (const auto& agent : agents) {
        if (!handled(agent) || !current.value(agent.id).isEmpty())
            continue;
        const auto started = serviceStart(agent.endpoint);
        if (started == 0)
            continue;
        QSet<QString> taken;
        for (const auto& other : agents)
            if (other.id != agent.id && other.harness == agent.harness)
                if (const auto theirs = current.value(other.id); !theirs.isEmpty())
                    taken.insert(theirs);
        const auto since_ms = started - clock_slack_ms;
        auto candidates = agent.harness == QLatin1String("agy")
                              ? antigravityStarted(agent.folder, since_ms)
                              : codexStarted(agent.folder, since_ms);
        candidates.removeIf([&taken](const Started& one) { return taken.contains(one.id); });
        if (candidates.isEmpty())
            continue;
        const auto first = *std::min_element(
            candidates.cbegin(), candidates.cend(),
            [](const Started& left, const Started& right) { return left.ms < right.ms; });
        // Another running agent of this CLI in this folder, with no
        // conversation yet, that had started by then could have begun it.
        const bool shared = std::any_of(agents.cbegin(), agents.cend(), [&](const Agent& other) {
            if (other.id == agent.id || other.harness != agent.harness ||
                !current.value(other.id).isEmpty() || !sameFolder(other.folder, agent.folder))
                return false;
            const auto their_start = serviceStart(other.endpoint);
            return their_start != 0 && their_start - clock_slack_ms <= first.ms &&
                   service_pid(other.endpoint) != 0;
        });
        if (shared)
            continue;
        changed.insert(agent.id, first.id);
        current.insert(agent.id, first.id);
    }
    return changed;
}
} // namespace lapis::desktop::conversation_owner
