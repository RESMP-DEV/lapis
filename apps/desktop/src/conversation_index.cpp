#include "conversation_index.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QThreadPool>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace lapis::desktop {
namespace {
constexpr qint64 kLineLimit = qint64{1} << 20; // longer lines are tool output, not metadata
constexpr int kClaudeHeadLines = 300;
constexpr int kCodexHeadLines = 200;
constexpr qint64 kTailBytes = qint64{128} * 1024;
constexpr int kTitleLength = 140;
constexpr double kHalfLifeDays = 14.0;
constexpr qint64 kDayMs = qint64{24} * 60 * 60 * 1000;

const QRegularExpression& uuid_name() {
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern;
}

std::optional<QJsonObject> record(const QByteArray& line) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    return document.object();
}

// One line of typed text: the CLIs' own wrappers (<command-name>, injected
// context, the local-command caveat) are not what someone typed.
QString typed(QString text) {
    text = text.trimmed();
    if (text.isEmpty() || text.startsWith(QLatin1Char('<')) || text.startsWith(QLatin1Char('#')) ||
        text.startsWith(QLatin1String("Caveat:")))
        return {};
    text = text.simplified();
    return text.size() > kTitleLength ? text.left(kTitleLength - 1) + QChar(0x2026) : text;
}

QString claude_user_text(const QJsonObject& entry) {
    const auto content =
        entry.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
    if (content.isString())
        return typed(content.toString());
    for (const auto& block : content.toArray()) {
        const auto part = block.toObject();
        if (part.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            return typed(part.value(QStringLiteral("text")).toString());
    }
    return {};
}

// The newest title Claude Code gave the session, from the end of the file.
QString claude_tail_title(QFile& file) {
    const auto size = file.size();
    if (!file.seek(std::max<qint64>(0, size - kTailBytes)))
        return {};
    auto tail = file.readAll();
    if (size > kTailBytes)
        tail = tail.mid(tail.indexOf('\n') + 1); // the first line is partial
    QString title;
    for (const auto& line : tail.split('\n')) {
        if (!line.contains("\"ai-title\""))
            continue;
        if (const auto entry = record(line);
            entry && entry->value(QStringLiteral("type")).toString() == QLatin1String("ai-title"))
            title = typed(entry->value(QStringLiteral("aiTitle")).toString());
    }
    return title;
}

QString codex_user_text(const QJsonObject& payload) {
    if (payload.value(QStringLiteral("type")).toString() != QLatin1String("message") ||
        payload.value(QStringLiteral("role")).toString() != QLatin1String("user"))
        return {};
    for (const auto& item : payload.value(QStringLiteral("content")).toArray()) {
        const auto part = item.toObject();
        if (const auto text = typed(part.value(QStringLiteral("text")).toString()); !text.isEmpty())
            return text;
    }
    return {};
}

QJsonObject to_json(const Conversation& conversation) {
    return {{QStringLiteral("h"), conversation.harness},
            {QStringLiteral("i"), conversation.id},
            {QStringLiteral("d"), conversation.directory},
            {QStringLiteral("t"), conversation.title},
            {QStringLiteral("m"), conversation.modified}};
}

std::optional<Conversation> from_json(const QJsonValue& value) {
    if (!value.isObject())
        return std::nullopt;
    const auto entry = value.toObject();
    Conversation conversation{
        entry.value(QStringLiteral("h")).toString(), entry.value(QStringLiteral("i")).toString(),
        entry.value(QStringLiteral("d")).toString(), entry.value(QStringLiteral("t")).toString(),
        entry.value(QStringLiteral("m")).toInteger()};
    if (conversation.id.isEmpty() || conversation.directory.isEmpty())
        return std::nullopt;
    return conversation;
}

struct ScanPaths {
    QString claude_home;
    QString codex_home;
    QString cache;
};

// One pass over the session files, reusing what the cache says about files
// whose size and time have not changed; the cache is rewritten when anything
// did. `alive` stops it early when the index goes away.
class Scan {
  public:
    Scan(const ScanPaths& paths, const std::atomic_bool& alive) : paths_(paths), alive_(alive) {
        QFile file(paths_.cache);
        if (!file.open(QIODevice::ReadOnly))
            return;
        const auto document = QJsonDocument::fromJson(file.readAll()).object();
        if (document.value(QStringLiteral("version")).toInt() == 1)
            cached_ = document.value(QStringLiteral("files")).toObject();
    }

    bool claude() {
        const QDir projects(QDir(paths_.claude_home).filePath(QStringLiteral("projects")));
        for (const auto& project : projects.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            for (const auto& session :
                 QDir(project.absoluteFilePath())
                     .entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files)) {
                if (!alive_.load())
                    return false;
                if (uuid_name().match(session.completeBaseName()).hasMatch())
                    visit(session,
                          [](const QString& path) { return conversations::read_claude(path); });
            }
        }
        return true;
    }

    // The cache keeps each rollout's own first message; names apply in
    // finish(), as Codex renames a thread without touching its rollout.
    bool codex() {
        QDirIterator rollouts(QDir(paths_.codex_home).filePath(QStringLiteral("sessions")),
                              {QStringLiteral("rollout-*.jsonl")}, QDir::Files,
                              QDirIterator::Subdirectories);
        while (rollouts.hasNext()) {
            if (!alive_.load())
                return false;
            visit(rollouts.nextFileInfo(),
                  [](const QString& path) { return conversations::read_codex(path, {}); });
        }
        return true;
    }

    std::vector<Conversation> finish() {
        const auto names = conversations::codex_thread_names(
            QDir(paths_.codex_home).filePath(QStringLiteral("session_index.jsonl")));
        for (auto& conversation : found_) {
            const auto name = names.value(conversation.id);
            if (conversation.harness == QLatin1String("codex") && !name.isEmpty())
                conversation.title = name;
        }
        if (dirty_ || files_.size() != cached_.size())
            save();
        std::sort(found_.begin(), found_.end(),
                  [](const auto& a, const auto& b) { return a.modified > b.modified; });
        return std::move(found_);
    }

  private:
    template <typename Read> void visit(const QFileInfo& info, const Read& read) {
        const auto path = info.absoluteFilePath();
        const auto size = info.size();
        const auto time = info.lastModified().toMSecsSinceEpoch();
        auto entry = cached_.value(path).toObject();
        if (entry.isEmpty() || entry.value(QStringLiteral("s")).toInteger() != size ||
            entry.value(QStringLiteral("m")).toInteger() != time) {
            dirty_ = true;
            entry = {{QStringLiteral("s"), size}, {QStringLiteral("m"), time}};
            if (const auto conversation = read(path))
                entry.insert(QStringLiteral("c"), to_json(*conversation));
        }
        files_.insert(path, entry);
        if (const auto conversation = from_json(entry.value(QStringLiteral("c"))))
            found_.push_back(*conversation);
    }

    void save() const {
        QSaveFile out(paths_.cache);
        if (!out.open(QIODevice::WriteOnly))
            return;
        out.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                            {QStringLiteral("files"), files_}})
                      .toJson(QJsonDocument::Compact));
        if (!out.commit())
            qWarning().noquote() << "Conversation cache not saved:" << paths_.cache;
    }

    const ScanPaths& paths_;
    const std::atomic_bool& alive_;
    QJsonObject cached_;
    QJsonObject files_;
    bool dirty_{};
    std::vector<Conversation> found_;
};

std::vector<Conversation> scan(const ScanPaths& paths, const std::atomic_bool& alive) {
    Scan pass(paths, alive);
    if (!pass.claude() || !pass.codex())
        return {};
    return pass.finish();
}

QString place(const QString& directory) {
    const auto home = QDir::homePath();
    if (directory == home)
        return QStringLiteral("~");
    if (directory.startsWith(home + QLatin1Char('/')))
        return QStringLiteral("~") + directory.mid(home.size());
    return directory;
}
} // namespace

namespace conversations {
std::optional<Conversation> read_claude(const QString& path) {
    QFile file(path);
    const QFileInfo info(path);
    if (!uuid_name().match(info.completeBaseName()).hasMatch() || !file.open(QIODevice::ReadOnly))
        return std::nullopt;
    QString entrypoint;
    QString cwd;
    QString first;
    QString title;
    for (int count = 0; count < kClaudeHeadLines && !file.atEnd(); ++count) {
        const auto line = file.readLine(kLineLimit);
        const auto entry = record(line);
        if (!entry)
            continue;
        if (entrypoint.isEmpty())
            entrypoint = entry->value(QStringLiteral("entrypoint")).toString();
        // Automation says so on its first lines; stop reading it at once.
        if (!entrypoint.isEmpty() && entrypoint != QLatin1String("cli"))
            return std::nullopt;
        if (cwd.isEmpty())
            cwd = entry->value(QStringLiteral("cwd")).toString();
        const auto type = entry->value(QStringLiteral("type")).toString();
        if (type == QLatin1String("ai-title"))
            title = typed(entry->value(QStringLiteral("aiTitle")).toString());
        else if (type == QLatin1String("user") && first.isEmpty() &&
                 !entry->value(QStringLiteral("isMeta")).toBool())
            first = claude_user_text(*entry);
    }
    if (entrypoint != QLatin1String("cli") || cwd.isEmpty())
        return std::nullopt;
    if (!file.atEnd())
        if (const auto later = claude_tail_title(file); !later.isEmpty())
            title = later;
    return Conversation{QStringLiteral("claude"), info.completeBaseName(), QDir::cleanPath(cwd),
                        title.isEmpty() ? first : title, info.lastModified().toMSecsSinceEpoch()};
}

std::optional<Conversation> read_codex(const QString& path, const QHash<QString, QString>& names) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const auto head = record(file.readLine(kLineLimit));
    if (!head || head->value(QStringLiteral("type")).toString() != QLatin1String("session_meta"))
        return std::nullopt;
    const auto meta = head->value(QStringLiteral("payload")).toObject();
    const auto source = meta.value(QStringLiteral("source"));
    if (source.isObject() || source.toString() == QLatin1String("exec") ||
        !meta.value(QStringLiteral("parent_thread_id")).toString().isEmpty() ||
        meta.value(QStringLiteral("thread_source")).toString() == QLatin1String("subagent"))
        return std::nullopt;
    const auto id = meta.value(QStringLiteral("id")).toString();
    const auto cwd = meta.value(QStringLiteral("cwd")).toString();
    if (!uuid_name().match(id).hasMatch() || cwd.isEmpty())
        return std::nullopt;
    QString title = names.value(id);
    for (int count = 0; title.isEmpty() && count < kCodexHeadLines && !file.atEnd(); ++count)
        if (const auto entry = record(file.readLine(kLineLimit));
            entry &&
            entry->value(QStringLiteral("type")).toString() == QLatin1String("response_item"))
            title = codex_user_text(entry->value(QStringLiteral("payload")).toObject());
    return Conversation{QStringLiteral("codex"), id, QDir::cleanPath(cwd), title,
                        QFileInfo(path).lastModified().toMSecsSinceEpoch()};
}

QHash<QString, QString> codex_thread_names(const QString& path) {
    QHash<QString, QString> names;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return names;
    while (!file.atEnd()) {
        const auto entry = record(file.readLine(kLineLimit));
        if (!entry)
            continue;
        const auto id = entry->value(QStringLiteral("id")).toString();
        const auto name = entry->value(QStringLiteral("thread_name")).toString().simplified();
        if (!id.isEmpty() && !name.isEmpty())
            names.insert(id, name.left(kTitleLength));
    }
    return names;
}

QHash<QString, double> folder_heat(const std::vector<Conversation>& all, qint64 now_ms,
                                   const QStringList& open) {
    QHash<QString, double> heat;
    for (const auto& conversation : all) {
        const auto days = static_cast<double>(std::max<qint64>(0, now_ms - conversation.modified)) /
                          static_cast<double>(kDayMs);
        heat[conversation.directory] += std::pow(0.5, days / kHalfLifeDays);
    }
    for (const auto& folder : open)
        heat[QDir::cleanPath(folder)] += 1.0;
    return heat;
}

QStringList order_children(const QHash<QString, double>& heat, const QString& parent,
                           const QStringList& names, int hot) {
    // Work in a folder under a child counts for that child.
    auto base = QDir::cleanPath(parent);
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    QHash<QString, double> child_heat;
    for (auto entry = heat.cbegin(); entry != heat.cend(); ++entry) {
        if (entry.key().startsWith(base) && entry.key().size() > base.size())
            child_heat[entry.key().mid(base.size()).section(QLatin1Char('/'), 0, 0)] +=
                entry.value();
    }
    QStringList warm;
    QStringList rest;
    for (const auto& name : names)
        (child_heat.value(name) > 0.0 ? warm : rest).append(name);
    std::stable_sort(warm.begin(), warm.end(), [&](const QString& a, const QString& b) {
        const auto left = child_heat.value(a);
        const auto right = child_heat.value(b);
        return left != right ? left > right : a.compare(b, Qt::CaseInsensitive) < 0;
    });
    if (warm.size() > hot) {
        rest += warm.mid(hot);
        warm = warm.mid(0, hot);
    }
    std::stable_sort(rest.begin(), rest.end(), [](const QString& a, const QString& b) {
        const bool a_last = a.startsWith(QLatin1Char('_'));
        const bool b_last = b.startsWith(QLatin1Char('_'));
        if (a_last != b_last)
            return b_last;
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return warm + rest;
}

QString age_text(qint64 then_ms, qint64 now_ms) {
    constexpr qint64 minute = 60;
    constexpr qint64 hour = 60 * minute;
    constexpr qint64 day = 24 * hour;
    const auto seconds = std::max<qint64>(0, now_ms - then_ms) / 1000;
    if (seconds < minute)
        return QStringLiteral("now");
    if (seconds < hour)
        return QStringLiteral("%1 min").arg(seconds / minute);
    if (seconds < day)
        return QStringLiteral("%1 h").arg(seconds / hour);
    if (seconds < 2 * day)
        return QStringLiteral("yesterday");
    if (seconds < 7 * day)
        return QStringLiteral("%1 d").arg(seconds / day);
    const auto then = QDateTime::fromMSecsSinceEpoch(then_ms).date();
    const auto now = QDateTime::fromMSecsSinceEpoch(now_ms).date();
    return then.toString(then.year() == now.year() ? QStringLiteral("MMM d")
                                                   : QStringLiteral("MMM d yyyy"));
}
} // namespace conversations

ConversationIndex::ConversationIndex(QString claude_home, QString codex_home, QString cache_path,
                                     QObject* parent)
    : QObject(parent), claude_home_(std::move(claude_home)), codex_home_(std::move(codex_home)),
      cache_path_(std::move(cache_path)) {}

ConversationIndex::~ConversationIndex() { alive_->store(false); }

void ConversationIndex::refresh() {
    if (scanning_->exchange(true))
        return;
    QThreadPool::globalInstance()->start([paths = ScanPaths{claude_home_, codex_home_, cache_path_},
                                          busy = scanning_, alive = alive_,
                                          self = QPointer<ConversationIndex>(this)] {
        QElapsedTimer timer;
        timer.start();
        auto found = scan(paths, *alive);
        qInfo().noquote() << "Conversations:" << found.size() << "in" << timer.elapsed() << "ms";
        auto* app = QCoreApplication::instance();
        if (app == nullptr || !alive->load()) {
            busy->store(false);
            return;
        }
        QMetaObject::invokeMethod(app, [self, alive, busy, found = std::move(found)]() mutable {
            busy->store(false);
            if (alive->load() && self)
                self->setConversations(std::move(found));
        });
    });
}

void ConversationIndex::setConversations(std::vector<Conversation> all) {
    all_ = std::move(all);
    ready_ = true;
    emit changed();
}

QVariantList ConversationIndex::recent(const QString& query, int limit) const {
    const auto words = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    QVariantList rows;
    for (const auto& conversation : all_) {
        if (rows.size() >= limit)
            break;
        const auto where = place(conversation.directory);
        const auto haystack = (conversation.title + QLatin1Char(' ') + where + QLatin1Char(' ') +
                               conversation.harness)
                                  .toLower();
        if (!std::all_of(words.cbegin(), words.cend(),
                         [&](const QString& word) { return haystack.contains(word); }))
            continue;
        rows.append(QVariantMap{
            {QStringLiteral("harness"), conversation.harness},
            {QStringLiteral("id"), conversation.id},
            {QStringLiteral("directory"), conversation.directory},
            {QStringLiteral("place"), where},
            {QStringLiteral("title"),
             conversation.title.isEmpty() ? QStringLiteral("(no message)") : conversation.title},
            {QStringLiteral("when"), conversations::age_text(conversation.modified, now)},
        });
    }
    return rows;
}

QStringList ConversationIndex::orderFolders(const QString& parent, const QStringList& names) const {
    const auto heat = conversations::folder_heat(all_, QDateTime::currentMSecsSinceEpoch(),
                                                 open_folders_ ? open_folders_() : QStringList{});
    return conversations::order_children(heat, parent, names);
}
} // namespace lapis::desktop
