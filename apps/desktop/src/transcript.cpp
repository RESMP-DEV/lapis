#include "transcript.hpp"

#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <utility>

namespace lapis::desktop {
namespace transcript {
QString kind_name(Kind kind) {
    switch (kind) {
    case Kind::prompt: return QStringLiteral("prompt");
    case Kind::reply: return QStringLiteral("reply");
    case Kind::thinking: return QStringLiteral("thinking");
    case Kind::tool: return QStringLiteral("tool");
    case Kind::command: return QStringLiteral("command");
    case Kind::notice: return QStringLiteral("notice");
    case Kind::divider: return QStringLiteral("divider");
    case Kind::other: break;
    }
    return QStringLiteral("other");
}

namespace {
QString clip(QString text, qsizetype limit) {
    if (text.size() <= limit)
        return text;
    text.truncate(limit);
    // Never leave half of a surrogate pair at the cut.
    if (!text.isEmpty() && text.back().isHighSurrogate())
        text.chop(1);
    return text + QChar(0x2026);
}

QString homeless(QString path) {
    const auto home = QDir::homePath();
    if (path == home || path.startsWith(home + QLatin1Char('/')))
        path.replace(0, home.size(), QStringLiteral("~"));
    return path;
}

QString without_escapes(QString text) {
    static const QRegularExpression escapes(QStringLiteral(R"(\x1b\[[0-9;?]*[ -/]*[@-~])"));
    return text.remove(escapes);
}

// The text between <tag> and </tag>, or empty.
QString tagged(const QString& text, QStringView tag) {
    const auto open = QStringLiteral("<%1>").arg(tag);
    const auto close = QStringLiteral("</%1>").arg(tag);
    const auto start = text.indexOf(open);
    if (start < 0)
        return {};
    const auto from = start + open.size();
    const auto end = text.indexOf(close, from);
    return text.mid(from, end < 0 ? -1 : end - from).trimmed();
}

QString compact_json(const QJsonValue& value, qsizetype limit) {
    const auto bytes = value.isObject()  ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
                       : value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                         : QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return clip(QString::fromUtf8(bytes), limit);
}

void add_lines(std::vector<DiffLine>& diff, QChar sign, const QString& text) {
    const auto lines = text.split(QLatin1Char('\n'));
    for (const auto& line : lines) {
        if (std::cmp_greater_equal(diff.size(), diff_preview_lines))
            return;
        diff.push_back({sign, clip(line, 400)});
    }
}

QString result_text(const QJsonValue& content) {
    if (content.isString())
        return content.toString();
    QStringList parts;
    for (const auto& value : content.toArray()) {
        const auto block = value.toObject();
        const auto type = block.value(QLatin1String("type")).toString();
        if (type == QLatin1String("text"))
            parts << block.value(QLatin1String("text")).toString();
        else if (type == QLatin1String("image"))
            parts << QStringLiteral("[image]");
        else
            parts << QStringLiteral("[%1]").arg(type);
    }
    return parts.join(QLatin1Char('\n'));
}

// A tool call's card: what it was asked to do, in the words that matter.
Item tool_item(const QJsonObject& block) {
    Item item;
    item.kind = Kind::tool;
    item.tool_id = block.value(QLatin1String("id")).toString();
    const auto name = block.value(QLatin1String("name")).toString();
    const auto input = block.value(QLatin1String("input")).toObject();
    const auto text = [&input](const char* key) { return input.value(QLatin1String(key)).toString(); };
    item.title = name;
    if (name.startsWith(QLatin1String("mcp__"))) {
        const auto parts = name.mid(5).split(QStringLiteral("__"));
        if (parts.size() == 2)
            item.title = QStringLiteral("%1 · %2").arg(parts[1], parts[0]);
    }
    if (name == QLatin1String("Bash")) {
        item.body = clip(text("command"), 2000);
        item.detail = text("description");
    } else if (name == QLatin1String("Read")) {
        item.body = homeless(text("file_path"));
        if (input.contains(QLatin1String("offset")))
            item.detail = QStringLiteral("from line %1").arg(input.value(QLatin1String("offset")).toInt());
    } else if (name == QLatin1String("Write")) {
        item.body = homeless(text("file_path"));
        add_lines(item.diff, QLatin1Char('+'), text("content"));
    } else if (name == QLatin1String("Edit")) {
        item.body = homeless(text("file_path"));
        add_lines(item.diff, QLatin1Char('-'), text("old_string"));
        add_lines(item.diff, QLatin1Char('+'), text("new_string"));
    } else if (name == QLatin1String("MultiEdit")) {
        item.body = homeless(text("file_path"));
        for (const auto& edit : input.value(QLatin1String("edits")).toArray()) {
            add_lines(item.diff, QLatin1Char('-'), edit.toObject().value(QLatin1String("old_string")).toString());
            add_lines(item.diff, QLatin1Char('+'), edit.toObject().value(QLatin1String("new_string")).toString());
        }
    } else if (name == QLatin1String("Grep") || name == QLatin1String("Glob")) {
        item.body = text("pattern");
        if (!text("path").isEmpty())
            item.detail = homeless(text("path"));
    } else if (name == QLatin1String("Agent") || name == QLatin1String("Task")) {
        item.detail = text("description");
        item.body = clip(text("prompt"), 600);
    } else if (name == QLatin1String("WebFetch")) {
        item.body = text("url");
    } else if (name == QLatin1String("WebSearch")) {
        item.body = text("query");
    } else if (name == QLatin1String("TodoWrite")) {
        QStringList todos;
        for (const auto& todo : input.value(QLatin1String("todos")).toArray()) {
            const auto entry = todo.toObject();
            const auto status = entry.value(QLatin1String("status")).toString();
            const auto mark = status == QLatin1String("completed")     ? QStringLiteral("✓")
                              : status == QLatin1String("in_progress") ? QStringLiteral("▸")
                                                                       : QStringLiteral("·");
            todos << QStringLiteral("%1 %2").arg(mark, entry.value(QLatin1String("content")).toString());
        }
        item.body = todos.join(QLatin1Char('\n'));
    } else {
        item.body = input.isEmpty() ? QString{} : compact_json(input, 600);
    }
    return item;
}

void attach_result(Item& item, const QJsonObject& block) {
    const auto text = without_escapes(result_text(block.value(QLatin1String("content")))).trimmed();
    item.answered = true;
    item.failed = block.value(QLatin1String("is_error")).toBool();
    // A file change that worked says so in a sentence the diff already shows.
    if (!item.failed && !item.diff.empty())
        return;
    const auto lines = text.isEmpty() ? QStringList{} : text.split(QLatin1Char('\n'));
    item.result_lines = static_cast<int>(lines.size());
    item.result = clip(lines.mid(0, result_preview_lines).join(QLatin1Char('\n')), result_preview_chars);
}

Item make(Kind kind, QString title, QString body = {}, QString detail = {}) {
    Item item;
    item.kind = kind;
    item.title = std::move(title);
    item.body = std::move(body);
    item.detail = std::move(detail);
    return item;
}
} // namespace

int ClaudeReader::add(const QByteArray& line) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return -1;
    const auto record = document.object();
    if (record.value(QLatin1String("isSidechain")).toBool())
        return -1;
    const auto type = record.value(QLatin1String("type")).toString();
    const auto first = static_cast<int>(items_.size());
    int changed = -1;
    const auto push = [this](Item item) { items_.push_back(std::move(item)); };

    if (type == QLatin1String("system")) {
        if (record.value(QLatin1String("subtype")).toString() == QLatin1String("compact_boundary"))
            push(make(Kind::divider, QStringLiteral("Conversation compacted")));
        return static_cast<int>(items_.size()) > first ? first : -1;
    }
    // Records without a message (titles, modes, file snapshots, attachments)
    // are the CLI's bookkeeping, not conversation.
    const auto message = record.value(QLatin1String("message")).toObject();
    if (message.isEmpty() || (type != QLatin1String("user") && type != QLatin1String("assistant")))
        return -1;
    const bool meta = record.value(QLatin1String("isMeta")).toBool();
    const auto content = message.value(QLatin1String("content"));

    const auto user_text = [&](const QString& raw) {
        if (meta || raw.startsWith(QLatin1String("<local-command-")))
            return;
        if (raw.startsWith(QLatin1String("<task-notification>"))) {
            auto summary = tagged(raw, u"summary");
            push(make(Kind::notice, summary.isEmpty() ? QStringLiteral("Background task finished")
                                                      : clip(summary, 300)));
            return;
        }
        if (raw.startsWith(QLatin1String("This session is being continued"))) {
            push(make(Kind::divider, QStringLiteral("Continued from an earlier conversation")));
            return;
        }
        if (raw.contains(QLatin1String("<command-name>"))) {
            push(make(Kind::command, tagged(raw, u"command-name"), tagged(raw, u"command-args")));
            return;
        }
        push(make(Kind::prompt, {}, clip(raw.trimmed(), body_chars)));
    };

    if (content.isString()) {
        if (type == QLatin1String("user"))
            user_text(content.toString());
        else
            push(make(Kind::reply, {}, clip(content.toString().trimmed(), body_chars)));
    }
    for (const auto& value : content.toArray()) {
        const auto block = value.toObject();
        const auto kind = block.value(QLatin1String("type")).toString();
        if (kind == QLatin1String("text")) {
            const auto text = block.value(QLatin1String("text")).toString();
            if (type == QLatin1String("user"))
                user_text(text);
            else if (!text.trimmed().isEmpty())
                push(make(Kind::reply, {}, clip(text.trimmed(), body_chars)));
        } else if (kind == QLatin1String("thinking") || kind == QLatin1String("redacted_thinking")) {
            push(make(Kind::thinking, {},
                      clip(block.value(QLatin1String("thinking")).toString().trimmed(), body_chars)));
        } else if (kind == QLatin1String("tool_use")) {
            auto item = tool_item(block);
            tools_.insert(item.tool_id, static_cast<int>(items_.size()));
            push(std::move(item));
        } else if (kind == QLatin1String("tool_result")) {
            const auto at = tools_.value(block.value(QLatin1String("tool_use_id")).toString(), -1);
            if (at >= 0) {
                attach_result(items_[static_cast<size_t>(at)], block);
                changed = changed < 0 ? at : std::min(changed, at);
            }
        } else if (kind == QLatin1String("image")) {
            if (!meta)
                push(make(Kind::prompt, {}, QStringLiteral("[image]")));
        } else {
            push(make(Kind::other, kind, {}, compact_json(block, 400)));
        }
    }
    if (static_cast<int>(items_.size()) > first)
        changed = changed < 0 ? first : std::min(changed, first);
    return changed;
}
} // namespace transcript

namespace {
struct ReadResult {
    transcript::ClaudeReader reader;
    qint64 offset{};
    QByteArray pending;
};

// Feeds `bytes` (after `pending`, a line started earlier) to the reader and
// keeps a trailing unfinished line for next time. Returns the first item a
// line changed, or -1.
int feed(transcript::ClaudeReader& reader, QByteArray& pending, const QByteArray& bytes) {
    pending += bytes;
    int changed = -1;
    qsizetype start = 0;
    for (qsizetype end = pending.indexOf('\n'); end >= 0; end = pending.indexOf('\n', start)) {
        const auto at = reader.add(pending.sliced(start, end - start));
        if (at >= 0)
            changed = changed < 0 ? at : std::min(changed, at);
        start = end + 1;
    }
    pending.remove(0, start);
    return changed;
}

ReadResult read_whole(const QString& path) {
    ReadResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return result;
    while (!file.atEnd())
        feed(result.reader, result.pending, file.read(qint64{1} << 20));
    result.offset = file.pos();
    return result;
}

QVariantList diff_rows(const std::vector<transcript::DiffLine>& diff) {
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(diff.size()));
    for (const auto& line : diff)
        rows.append(QVariantMap{{QStringLiteral("sign"), QString(line.sign)},
                                {QStringLiteral("text"), line.text}});
    return rows;
}
} // namespace

TranscriptModel::TranscriptModel(QObject* parent) : QAbstractListModel(parent) {}

int TranscriptModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : shown_;
}

QVariant TranscriptModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= shown_)
        return {};
    const auto& item = reader_.items()[static_cast<size_t>(index.row())];
    switch (role) {
    case KindRole: return transcript::kind_name(item.kind);
    case TitleRole: return item.title;
    case BodyRole: return item.body;
    case DetailRole: return item.detail;
    case ResultRole: return item.result;
    case DiffRole: return diff_rows(item.diff);
    case ResultLinesRole: return item.result_lines;
    case FailedRole: return item.failed;
    case AnsweredRole: return item.answered;
    default: return {};
    }
}

QHash<int, QByteArray> TranscriptModel::roleNames() const {
    return {{KindRole, "kind"},       {TitleRole, "title"},   {BodyRole, "body"},
            {DetailRole, "detail"},   {ResultRole, "result"}, {DiffRole, "diff"},
            {ResultLinesRole, "resultLines"}, {FailedRole, "failed"}, {AnsweredRole, "answered"}};
}

void TranscriptModel::setPath(const QString& path) {
    if (path == path_)
        return;
    path_ = path;
    emit pathChanged();
    const auto generation = ++generation_;
    beginResetModel();
    reader_ = {};
    shown_ = 0;
    offset_ = 0;
    pending_.clear();
    endResetModel();
    emit countChanged();
    auto* watcher = findChild<QFileSystemWatcher*>();
    if (watcher == nullptr) {
        watcher = new QFileSystemWatcher(this);
        connect(watcher, &QFileSystemWatcher::fileChanged, this, [this] {
            if (!loading_)
                readMore();
        });
    }
    if (!watcher->files().isEmpty())
        watcher->removePaths(watcher->files());
    if (path.isEmpty())
        return;
    watcher->addPath(path);
    loading_ = true;
    emit loadingChanged();
    QThreadPool::globalInstance()->start([guard = QPointer<TranscriptModel>(this), path, generation] {
        auto result = read_whole(path);
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, generation, result = std::move(result)]() mutable {
                if (guard == nullptr || guard->generation_ != generation)
                    return;
                guard->apply(std::move(result.reader), result.offset, std::move(result.pending));
            },
            Qt::QueuedConnection);
    });
}

void TranscriptModel::loadNow() {
    ++generation_;
    auto result = read_whole(path_);
    apply(std::move(result.reader), result.offset, std::move(result.pending));
}

void TranscriptModel::apply(transcript::ClaudeReader reader, qint64 offset, QByteArray pending) {
    beginResetModel();
    reader_ = std::move(reader);
    shown_ = static_cast<int>(reader_.items().size());
    offset_ = offset;
    pending_ = std::move(pending);
    endResetModel();
    emit countChanged();
    if (loading_) {
        loading_ = false;
        emit loadingChanged();
    }
    readMore(); // whatever the CLI wrote while the file was being read
}

void TranscriptModel::readMore() {
    QFile file(path_);
    if (path_.isEmpty() || !file.open(QIODevice::ReadOnly))
        return;
    if (file.size() < offset_) { // rewritten from the start
        loadNow();
        return;
    }
    if (file.size() == offset_ || !file.seek(offset_))
        return;
    const auto bytes = file.readAll();
    offset_ += bytes.size();
    const auto changed = feed(reader_, pending_, bytes);
    if (changed < 0)
        return;
    // Rows past shown_ already exist in the reader; the view learns of them
    // only through the insert below.
    const auto old_size = shown_;
    if (changed < old_size)
        emit dataChanged(index(changed), index(old_size - 1));
    const auto new_size = static_cast<int>(reader_.items().size());
    if (new_size > old_size) {
        beginInsertRows({}, old_size, new_size - 1);
        shown_ = new_size;
        endInsertRows();
        emit countChanged();
    }
}
} // namespace lapis::desktop
