#include "transcript.hpp"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QColor>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QFileInfo>
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
    case Kind::turn: return QStringLiteral("turn");
    case Kind::other: break;
    }
    return QStringLiteral("other");
}

QString duration_text(qint64 ms) {
    const auto seconds = std::max<qint64>(0, (ms + 500) / 1000);
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
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

Item make(Kind kind, QString title, QString body = {}, QString detail = {}) {
    Item item;
    item.kind = kind;
    item.title = std::move(title);
    item.body = std::move(body);
    item.detail = std::move(detail);
    return item;
}

// Adds `text`'s lines to the diff preview and counts all of them.
void add_lines(Item& item, QChar sign, const QString& text) {
    if (text.isEmpty())
        return;
    const auto lines = text.split(QLatin1Char('\n'));
    (sign == QLatin1Char('+') ? item.added : item.removed) += static_cast<int>(lines.size());
    for (const auto& line : lines) {
        if (std::cmp_greater_equal(item.diff.size(), diff_preview_lines))
            return;
        item.diff.push_back({sign, clip(line, 400)});
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

QString first_line(const QString& text) {
    const auto end = text.indexOf(QLatin1Char('\n'));
    return end < 0 ? text : text.left(end) + QStringLiteral(" …");
}

// A tool call as one step: a past-tense verb and its target, in the words
// that matter, the way a person would summarize it.
Item tool_item(const QJsonObject& block) {
    Item item;
    item.kind = Kind::tool;
    item.tool_id = block.value(QLatin1String("id")).toString();
    const auto name = block.value(QLatin1String("name")).toString();
    const auto input = block.value(QLatin1String("input")).toObject();
    const auto text = [&input](const char* key) { return input.value(QLatin1String(key)).toString(); };
    const auto step = [&item](const char* verb, QString target, QString detail = {}) {
        item.title = QString::fromUtf8(verb);
        item.body = std::move(target);
        item.detail = std::move(detail);
    };
    if (name == QLatin1String("Bash")) {
        step("Ran", clip(text("command"), 2000), text("description"));
    } else if (name == QLatin1String("Read")) {
        step("Read", homeless(text("file_path")));
    } else if (name == QLatin1String("Write")) {
        step("Wrote", homeless(text("file_path")));
        add_lines(item, QLatin1Char('+'), text("content"));
    } else if (name == QLatin1String("Edit")) {
        step("Edited", homeless(text("file_path")));
        add_lines(item, QLatin1Char('-'), text("old_string"));
        add_lines(item, QLatin1Char('+'), text("new_string"));
    } else if (name == QLatin1String("MultiEdit")) {
        step("Edited", homeless(text("file_path")));
        for (const auto& edit : input.value(QLatin1String("edits")).toArray()) {
            add_lines(item, QLatin1Char('-'), edit.toObject().value(QLatin1String("old_string")).toString());
            add_lines(item, QLatin1Char('+'), edit.toObject().value(QLatin1String("new_string")).toString());
        }
    } else if (name == QLatin1String("Grep")) {
        step("Searched for", text("pattern"), homeless(text("path")));
    } else if (name == QLatin1String("Glob")) {
        step("Listed", text("pattern"), homeless(text("path")));
    } else if (name == QLatin1String("Agent") || name == QLatin1String("Task")) {
        step("Started agent", text("description"), clip(text("prompt"), 600));
    } else if (name == QLatin1String("WebFetch")) {
        step("Fetched", text("url"));
    } else if (name == QLatin1String("WebSearch")) {
        step("Searched the web for", text("query"));
    } else if (name == QLatin1String("Monitor")) {
        step("Watched", text("description").isEmpty() ? first_line(text("command")) : text("description"));
    } else if (name == QLatin1String("SendMessage")) {
        step("Messaged", text("to"), text("summary"));
    } else if (name == QLatin1String("ScheduleWakeup")) {
        step("Scheduled a wakeup", text("reason"));
    } else if (name == QLatin1String("Skill")) {
        step("Used skill", text("skill"));
    } else if (name == QLatin1String("TaskStop")) {
        step("Stopped task", text("task_id"));
    } else if (name == QLatin1String("ToolSearch")) {
        step("Loaded tools", text("query"));
    } else if (name == QLatin1String("AskUserQuestion")) {
        QStringList questions;
        for (const auto& question : input.value(QLatin1String("questions")).toArray())
            questions << question.toObject().value(QLatin1String("question")).toString();
        step("Asked", questions.join(QLatin1Char('\n')));
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
        step("Updated the plan", {}, todos.join(QLatin1Char('\n')));
    } else if (name.startsWith(QLatin1String("mcp__"))) {
        // mcp__server__tool: "Called tool" with the server as its detail.
        const auto parts = name.mid(5).split(QStringLiteral("__"));
        if (parts.size() == 2)
            step("Called", QString(parts[1]).replace(QLatin1Char('_'), QLatin1Char(' ')),
                 QString(parts[0]).replace(QLatin1Char('-'), QLatin1Char(' ')));
        else
            step("Called", name);
    } else {
        step("Called", name, input.isEmpty() ? QString{} : compact_json(input, 600));
    }
    return item;
}

void attach_result(Item& item, const QJsonObject& block) {
    item.answered = true;
    item.failed = block.value(QLatin1String("is_error")).toBool();
    // A file change that worked says so in a sentence the diff already shows.
    if (!item.failed && !item.diff.empty())
        return;
    const auto text = without_escapes(result_text(block.value(QLatin1String("content")))).trimmed();
    const auto lines = text.isEmpty() ? QStringList{} : text.split(QLatin1Char('\n'));
    item.result_lines = static_cast<int>(lines.size());
    item.result = clip(lines.mid(0, result_preview_lines).join(QLatin1Char('\n')), result_preview_chars);
}

bool is_step(Kind kind) { return kind == Kind::tool || kind == Kind::thinking; }
} // namespace

std::vector<Piece> pieces(const QString& markdown) {
    std::vector<Piece> out;
    QStringList prose;
    QStringList code;
    QString fence;
    QString language;
    const auto flush_prose = [&] {
        const auto text = prose.join(QLatin1Char('\n')).trimmed();
        if (!text.isEmpty())
            out.push_back({false, {}, text});
        prose.clear();
    };
    for (const auto& line : markdown.split(QLatin1Char('\n'))) {
        const auto trimmed = line.trimmed();
        if (fence.isEmpty()) {
            if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
                flush_prose();
                // The fence is its run of ` or ~; a closing fence is at least as long.
                qsizetype length = 0;
                while (length < trimmed.size() && trimmed[length] == trimmed[0])
                    ++length;
                fence = trimmed.left(length);
                language = trimmed.mid(length).trimmed().section(QLatin1Char(' '), 0, 0);
                continue;
            }
            prose << line;
        } else if (trimmed.startsWith(fence) && trimmed.mid(fence.size()).trimmed().isEmpty()) {
            out.push_back({true, language, code.join(QLatin1Char('\n'))});
            code.clear();
            fence.clear();
        } else {
            code << line;
        }
    }
    if (!fence.isEmpty()) // an unclosed block still reads as code
        out.push_back({true, language, code.join(QLatin1Char('\n'))});
    flush_prose();
    return out;
}

QString clean_copy(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QChar(0x2029), QLatin1Char('\n')); // paragraph separator
    text.replace(QChar(0x2028), QLatin1Char('\n')); // line separator
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QChar(0x00a0), QLatin1Char(' '));
    text.replace(QChar(0x202f), QLatin1Char(' '));
    for (const QChar invisible : {QChar(0xfffc), QChar(0x200b), QChar(0x200c), QChar(0x200d),
                                  QChar(0xfeff), QChar(0x2060)})
        text.remove(invisible);
    auto lines = text.split(QLatin1Char('\n'));
    for (auto& line : lines) {
        qsizetype end = line.size();
        while (end > 0 && (line[end - 1] == QLatin1Char(' ') || line[end - 1] == QLatin1Char('\t')))
            --end;
        line.truncate(end);
    }
    while (!lines.isEmpty() && lines.back().isEmpty())
        lines.removeLast();
    while (!lines.isEmpty() && lines.front().isEmpty())
        lines.removeFirst();
    return lines.join(QLatin1Char('\n'));
}

void ClaudeReader::push(Item item) {
    item.record = record_;
    item.after_step = is_step(item.kind) && !items_.empty() && is_step(items_.back().kind);
    items_.push_back(std::move(item));
}

int ClaudeReader::add(const QByteArray& line) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return -1;
    const auto record = document.object();
    if (record.value(QLatin1String("isSidechain")).toBool())
        return -1;
    const auto type = record.value(QLatin1String("type")).toString();
    record_ = record.value(QLatin1String("uuid")).toString();
    const auto first = static_cast<int>(items_.size());
    int changed = -1;

    if (type == QLatin1String("system")) {
        const auto subtype = record.value(QLatin1String("subtype")).toString();
        const auto content = record.value(QLatin1String("content")).toString();
        if (subtype == QLatin1String("compact_boundary")) {
            push(make(Kind::divider, QStringLiteral("Conversation compacted")));
        } else if (subtype == QLatin1String("turn_duration")) {
            const auto ms = static_cast<qint64>(record.value(QLatin1String("durationMs")).toDouble());
            if (ms > 0)
                push(make(Kind::turn, QStringLiteral("Worked for %1").arg(duration_text(ms))));
        } else if (subtype == QLatin1String("api_error") ||
                   subtype.startsWith(QLatin1String("model_refusal"))) {
            auto item = make(Kind::notice, content.isEmpty() ? subtype : clip(content, 300));
            item.failed = true;
            push(std::move(item));
        } else if (subtype == QLatin1String("away_summary") && !content.isEmpty()) {
            push(make(Kind::notice, clip(content, 600)));
        }
        return static_cast<int>(items_.size()) > first ? first : -1;
    }
    // Records without a message (titles, modes, file snapshots, attachments)
    // are the CLI's bookkeeping, not conversation.
    const auto message = record.value(QLatin1String("message")).toObject();
    if (message.isEmpty() || (type != QLatin1String("user") && type != QLatin1String("assistant")))
        return -1;
    const bool meta = record.value(QLatin1String("isMeta")).toBool();
    const auto content = message.value(QLatin1String("content"));
    const auto thought_ms = static_cast<qint64>(record.value(QLatin1String("thinkingDurationMs")).toDouble());

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
            push(make(Kind::thinking,
                      thought_ms > 0 ? QStringLiteral("Thought for %1").arg(duration_text(thought_ms))
                                     : QStringLiteral("Thought"),
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
    case AddedRole: return item.added;
    case RemovedRole: return item.removed;
    case ResultLinesRole: return item.result_lines;
    case FailedRole: return item.failed;
    case AnsweredRole: return item.answered;
    case AfterStepRole: return item.after_step;
    case RecordRole: return item.record;
    default: return {};
    }
}

QHash<int, QByteArray> TranscriptModel::roleNames() const {
    return {{KindRole, "kind"},
            {TitleRole, "title"},
            {BodyRole, "body"},
            {DetailRole, "detail"},
            {ResultRole, "result"},
            {DiffRole, "diff"},
            {AddedRole, "added"},
            {RemovedRole, "removed"},
            {ResultLinesRole, "resultLines"},
            {FailedRole, "failed"},
            {AnsweredRole, "answered"},
            {AfterStepRole, "afterStep"},
            {RecordRole, "record"}};
}

QVariantList TranscriptModel::pieces(const QString& markdown) const {
    QVariantList out;
    for (const auto& piece : transcript::pieces(markdown))
        out.append(QVariantMap{{QStringLiteral("code"), piece.code},
                               {QStringLiteral("language"), piece.language},
                               {QStringLiteral("text"), piece.text}});
    return out;
}

void TranscriptModel::style(QQuickTextDocument* target, const QString& markdown,
                            const QVariantMap& style) const {
    if (target == nullptr || target->textDocument() == nullptr)
        return;
    QTextDocument& document = *target->textDocument();
    const auto size = std::max(8, style.value(QStringLiteral("size")).toInt());
    const auto code_size = std::max(8, style.value(QStringLiteral("codeSize")).toInt());
    const auto mono = style.value(QStringLiteral("mono")).toString();
    const auto code_color = style.value(QStringLiteral("codeColor")).value<QColor>();
    const auto link_color = style.value(QStringLiteral("linkColor")).value<QColor>();
    QFont base(style.value(QStringLiteral("sans")).toString());
    base.setPixelSize(size);
    document.setDefaultFont(base);
    document.setDocumentMargin(0);
    document.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);

    // Qt's importer sizes headings and code on its own scale; put them on
    // the view's, relative to the body text.
    // Formats are gathered first and applied after: changing a fragment's
    // format while walking the fragments can merge or split them.
    const qreal heading_scale[] = {1.0, 1.4, 1.25, 1.12, 1.0, 1.0, 1.0};
    struct BlockChange {
        int position;
        QTextBlockFormat format;
    };
    struct CharChange {
        int position;
        int length;
        QTextCharFormat format;
    };
    std::vector<BlockChange> blocks;
    std::vector<CharChange> chars;
    bool first = true;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        auto block_format = block.blockFormat();
        const int level = std::clamp(block_format.headingLevel(), 0, 6);
        block_format.setLineHeight(level > 0 ? 120 : 140, QTextBlockFormat::ProportionalHeight);
        block_format.setTopMargin(first ? 0 : level > 0 ? size * 0.8 : 0);
        // The last block adds nothing: the view spaces pieces itself.
        block_format.setBottomMargin(!block.next().isValid()        ? 0
                                     : level > 0                   ? size * 0.3
                                     : block.textList() != nullptr ? size * 0.25
                                                                   : size * 0.65);
        first = false;
        blocks.push_back({block.position(), block_format});
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const auto part = fragment.fragment();
            if (!part.isValid())
                continue;
            auto format = part.charFormat();
            bool touched = false;
            if (level > 0) {
                QFont font = format.font();
                font.setPixelSize(static_cast<int>(size * heading_scale[level]));
                font.setWeight(QFont::DemiBold);
                format.setFont(font, QTextCharFormat::FontPropertiesSpecifiedOnly);
                touched = true;
            }
            if (format.fontFixedPitch()) {
                QFont font = format.font();
                font.setFamilies({mono});
                font.setPixelSize(code_size);
                format.setFont(font, QTextCharFormat::FontPropertiesSpecifiedOnly);
                if (code_color.isValid())
                    format.setBackground(code_color);
                touched = true;
            }
            if (format.isAnchor() && link_color.isValid()) {
                format.setForeground(link_color);
                format.setFontUnderline(false);
                touched = true;
            }
            if (touched)
                chars.push_back({part.position(), part.length(), format});
        }
    }
    QTextCursor cursor(&document);
    cursor.beginEditBlock();
    for (const auto& change : blocks) {
        cursor.setPosition(change.position);
        cursor.setBlockFormat(change.format);
    }
    for (const auto& change : chars) {
        cursor.setPosition(change.position);
        cursor.setPosition(change.position + change.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(change.format);
    }
    cursor.endEditBlock();
}

QVariantMap TranscriptModel::fonts() const {
    // New York and SF Mono ship with macOS but are hidden from the font list;
    // loading the files gives them names the view can ask for.
    static const QVariantMap loaded = [] {
        const auto family = [](const char* file, const char* fallback) {
            const auto id = QFontDatabase::addApplicationFont(QString::fromUtf8(file));
            const auto families = id < 0 ? QStringList{} : QFontDatabase::applicationFontFamilies(id);
            return families.isEmpty() ? QString::fromUtf8(fallback) : families.front();
        };
        return QVariantMap{
            {QStringLiteral("serif"), family("/System/Library/Fonts/NewYork.ttf", "Georgia")},
            {QStringLiteral("mono"), family("/System/Library/Fonts/SFNSMono.ttf", "Menlo")}};
    }();
    return loaded;
}

void TranscriptModel::copy(const QString& text) const {
    const auto clean = transcript::clean_copy(text);
    if (auto* clipboard = QGuiApplication::clipboard(); clipboard != nullptr && !clean.isEmpty())
        clipboard->setText(clean);
}

void TranscriptModel::copySelection(QQuickTextDocument* source, int start, int end) const {
    if (source == nullptr || source->textDocument() == nullptr || end <= start)
        return;
    copy(transcript::selection_text(*source->textDocument(), start, end));
}

QString transcript::selection_text(const QTextDocument& document, int start, int end) {
    QString out;
    const QTextList* previous_list = nullptr;
    for (auto block = document.findBlock(start); block.isValid() && block.position() < end;
         block = block.next()) {
        const int from = std::max(start, block.position());
        const int to = std::min(end, block.position() + block.length() - 1);
        const auto text = block.text().mid(from - block.position(), std::max(0, to - from));
        const auto* list = block.textList();
        QString marker;
        // A list item keeps its bullet or number when it is copied from its start.
        if (list != nullptr && from == block.position()) {
            const auto format = list->format();
            marker = QString(std::max(0, format.indent() - 1) * 2, QLatin1Char(' '));
            const bool numbered = format.style() == QTextListFormat::ListDecimal ||
                                  format.style() == QTextListFormat::ListLowerAlpha ||
                                  format.style() == QTextListFormat::ListUpperAlpha;
            marker += numbered ? QStringLiteral("%1. ").arg(list->itemNumber(block) + 1)
                               : QStringLiteral("- ");
        }
        // Items of one list, or of a list nested in it, stay on adjacent
        // lines; anything else starts a new paragraph.
        const bool same_list = list != nullptr && previous_list != nullptr &&
                               (list == previous_list || list->format().indent() > 1 ||
                                previous_list->format().indent() > 1);
        if (!out.isEmpty())
            out += same_list ? QStringLiteral("\n") : QStringLiteral("\n\n");
        out += marker + text;
        previous_list = list;
    }
    return clean_copy(out);
}

void TranscriptModel::setPath(const QString& path) {
    if (path == path_)
        return;
    // Keep what was read (a load still running has nothing whole to keep).
    if (!path_.isEmpty() && !loading_) {
        std::erase_if(kept_, [this](const Parsed& kept) { return kept.path == path_; });
        kept_.insert(kept_.begin(), Parsed{path_, std::move(reader_), offset_, std::move(pending_)});
        if (kept_.size() > kept_conversations)
            kept_.pop_back();
    }
    path_ = path;
    emit pathChanged();
    const auto generation = ++generation_;
    if (loading_) {
        loading_ = false;
        emit loadingChanged();
    }
    const auto hit = std::find_if(kept_.begin(), kept_.end(),
                                  [&path](const Parsed& kept) { return kept.path == path; });
    beginResetModel();
    if (hit != kept_.end() && !path.isEmpty()) {
        reader_ = std::move(hit->reader);
        offset_ = hit->offset;
        pending_ = std::move(hit->pending);
        kept_.erase(hit);
    } else {
        reader_ = {};
        offset_ = 0;
        pending_.clear();
    }
    shown_ = static_cast<int>(reader_.items().size());
    endResetModel();
    emit countChanged();
    const bool warm = shown_ > 0 || offset_ > 0;
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
    if (warm) {
        readMore(); // only what the CLI wrote since it was last shown
        return;
    }
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
