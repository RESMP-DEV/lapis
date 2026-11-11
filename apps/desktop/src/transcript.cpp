#include "transcript.hpp"

#include "code_highlight.hpp"
#include "slash_commands.hpp"
#include "transcript_math.hpp"
#include "transcript_parts.hpp"
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <limits>
#include <utility>

namespace lapis::desktop {
namespace transcript {
QString kind_name(Kind kind) {
    switch (kind) {
    case Kind::prompt:
        return QStringLiteral("prompt");
    case Kind::reply:
        return QStringLiteral("reply");
    case Kind::thinking:
        return QStringLiteral("thinking");
    case Kind::tool:
        return QStringLiteral("tool");
    case Kind::command:
        return QStringLiteral("command");
    case Kind::notice:
        return QStringLiteral("notice");
    case Kind::divider:
        return QStringLiteral("divider");
    case Kind::turn:
        return QStringLiteral("turn");
    case Kind::other:
        break;
    }
    return QStringLiteral("other");
}

QString duration_text(qint64 ms) {
    const auto seconds = std::max<qint64>(0, (ms + 500) / 1000);
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    if (seconds < 86400)
        return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
    return QStringLiteral("%1d %2h").arg(seconds / 86400).arg((seconds % 86400) / 3600);
}

namespace detail {
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
    if (!std::any_of(text.cbegin(), text.cend(), [](QChar c) {
            return c.unicode() < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\t');
        }))
        return text;
    // Control sequences (CSI with any private marker), operating system
    // commands (titles, colors, OSC 8 links) ended by BEL or ST, and other
    // two-byte escapes; then any control character left (binary output),
    // except tabs and newlines.
    static const QRegularExpression escapes(QStringLiteral(
        R"(\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)?|\x1b[P_^X][^\x1b]*(?:\x1b\\)?|\x1b[ -~]?)"));
    static const QRegularExpression controls(QStringLiteral(R"([\x00-\x08\x0b-\x1f\x7f])"));
    text.remove(escapes);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return text.remove(controls);
}

QString compact_json(const QJsonValue& value, qsizetype limit) {
    const auto bytes =
        value.isObject()  ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
        : value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                          : QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return clip(QString::fromUtf8(bytes), limit);
}

Item make(Kind kind, QString title, QString body, QString detail) {
    Item item;
    item.kind = kind;
    item.title = std::move(title);
    item.body = std::move(body);
    item.detail = std::move(detail);
    return item;
}

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

QString first_line(const QString& text) {
    const auto end = text.indexOf(QLatin1Char('\n'));
    return end < 0 ? text : text.left(end) + QStringLiteral(" …");
}

void set_result(Item& item, const QString& output) {
    const auto text = without_escapes(output).trimmed();
    const auto lines = text.isEmpty() ? QStringList{} : text.split(QLatin1Char('\n'));
    item.result_lines = static_cast<int>(lines.size());
    item.result =
        clip(lines.mid(0, result_preview_lines).join(QLatin1Char('\n')), result_preview_chars);
}

bool is_step(Kind kind) { return kind == Kind::tool || kind == Kind::thinking; }

qint64 time_ms(const QString& stamp) {
    if (stamp.isEmpty())
        return 0;
    auto at = QDateTime::fromString(stamp, Qt::ISODateWithMs);
    if (!at.isValid())
        at = QDateTime::fromString(stamp, Qt::ISODate);
    return at.isValid() ? at.toMSecsSinceEpoch() : 0;
}
} // namespace detail

namespace {
using namespace detail;

// The API name for a model as /model reports it: "Opus 5.5 (1M context)"
// is "claude-opus-5-5[1m]"; an API name stays as it is.
QString model_id(QString shown) {
    shown = without_escapes(shown).remove(QLatin1Char('`')).trimmed();
    if (shown.startsWith(QLatin1String("claude-")))
        return shown.section(QLatin1Char(' '), 0, 0);
    static const QRegularExpression name(QStringLiteral(R"(^([A-Za-z]+) (\d+)(?:\.(\d+))?)"));
    const auto match = name.match(shown);
    if (!match.hasMatch())
        return {};
    auto id = QStringLiteral("claude-%1-%2").arg(match.captured(1).toLower(), match.captured(2));
    if (match.hasCaptured(3))
        id += QLatin1Char('-') + match.captured(3);
    if (shown.contains(QLatin1String("1M context")))
        id += QLatin1String("[1m]");
    return id;
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

// Claude Code saves pasted text inside <pasted_content id="..."> tags (its
// terminal shows "[Pasted text #1]"); the prompt shows what was pasted.
QString unwrap_pastes(QString text) {
    if (!text.contains(QLatin1String("pasted_content")))
        return text;
    static const QRegularExpression tags(
        QStringLiteral(R"(<pasted_content(?: [^>\n]*)?>\n?|\n?</pasted_content>)"));
    return text.remove(tags);
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

// A tool call as one step: a past-tense verb and its target, in the words
// that matter, the way a person would summarize it.
Item tool_item(const QJsonObject& block) {
    Item item;
    item.kind = Kind::tool;
    item.tool_id = block.value(QLatin1String("id")).toString();
    const auto name = block.value(QLatin1String("name")).toString();
    const auto input = block.value(QLatin1String("input")).toObject();
    const auto text = [&input](const char* key) {
        return input.value(QLatin1String(key)).toString();
    };
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
            add_lines(item, QLatin1Char('-'),
                      edit.toObject().value(QLatin1String("old_string")).toString());
            add_lines(item, QLatin1Char('+'),
                      edit.toObject().value(QLatin1String("new_string")).toString());
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
        step("Watched",
             text("description").isEmpty() ? first_line(text("command")) : text("description"));
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
            todos << QStringLiteral("%1 %2").arg(mark,
                                                 entry.value(QLatin1String("content")).toString());
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
    // Claude Code adds notes for the model before or after some results
    // ("This file is already in your context", how old a memory file is);
    // the terminal never shows them.
    static const QRegularExpression trailing(
        QStringLiteral(R"((?:^|\n)<system-reminder>[\s\S]*</system-reminder>\s*$)"));
    static const QRegularExpression leading(
        QStringLiteral(R"(^\s*<system-reminder>[\s\S]*?</system-reminder>\n?)"));
    set_result(item,
               result_text(block.value(QLatin1String("content"))).remove(leading).remove(trailing));
}
} // namespace

namespace {
// The file a media path names, when it exists and the view can show it.
// `missing` is set when the path has a media extension but no file is there.
std::optional<Media> media_file(QString path, bool* missing) {
    static const QHash<QString, QString> kinds = {
        {QStringLiteral("png"), QStringLiteral("image")},
        {QStringLiteral("jpg"), QStringLiteral("image")},
        {QStringLiteral("jpeg"), QStringLiteral("image")},
        {QStringLiteral("bmp"), QStringLiteral("image")},
        {QStringLiteral("webp"), QStringLiteral("image")},
        {QStringLiteral("gif"), QStringLiteral("animation")},
        {QStringLiteral("svg"), QStringLiteral("svg")},
        {QStringLiteral("mp3"), QStringLiteral("audio")},
        {QStringLiteral("wav"), QStringLiteral("audio")},
        {QStringLiteral("m4a"), QStringLiteral("audio")},
        {QStringLiteral("aac"), QStringLiteral("audio")},
        {QStringLiteral("flac"), QStringLiteral("audio")},
        {QStringLiteral("aiff"), QStringLiteral("audio")},
        {QStringLiteral("caf"), QStringLiteral("audio")},
        {QStringLiteral("mp4"), QStringLiteral("video")},
        {QStringLiteral("mov"), QStringLiteral("video")},
        {QStringLiteral("m4v"), QStringLiteral("video")},
        // macOS's player cannot open these; they are cards for their app.
        {QStringLiteral("ogg"), QStringLiteral("file")},
        {QStringLiteral("opus"), QStringLiteral("file")},
        {QStringLiteral("webm"), QStringLiteral("file")},
        {QStringLiteral("mkv"), QStringLiteral("file")},
        {QStringLiteral("html"), QStringLiteral("html")},
        {QStringLiteral("htm"), QStringLiteral("html")},
        {QStringLiteral("pdf"), QStringLiteral("pdf")},
    };
    if (path.startsWith(QLatin1String("file://")))
        path = QUrl(path).toLocalFile();
    if (path.startsWith(QLatin1String("~/")))
        path = QDir::home().filePath(path.mid(2));
    if (!path.startsWith(QLatin1Char('/')))
        return std::nullopt;
    const auto suffix = QFileInfo(path).suffix().toLower();
    auto kind = kinds.value(suffix);
    if (kind.isEmpty())
        return std::nullopt;
    if (!QFileInfo(path).isFile()) {
        if (missing != nullptr)
            *missing = true;
        return std::nullopt;
    }
    // An image this build cannot decode (WebP without its plugin) is a card
    // that opens it, not an empty frame.
    static const auto readable = QImageReader::supportedImageFormats();
    if ((kind == QLatin1String("image") || kind == QLatin1String("svg")) &&
        !readable.contains(suffix.toLatin1()))
        kind = QStringLiteral("file");
    return Media{path, kind};
}

// The path a paragraph line is made of alone: a bare path, one in
// backticks, or a Markdown image or link to it. Indented lines (list items'
// continuations, indented code) and anything with other words are not.
QString lone_path(const QString& line) {
    if (line.isEmpty() || line.front().isSpace())
        return {};
    const auto text = line.trimmed();
    static const QRegularExpression markdown(
        QStringLiteral(R"(^!?\[[^\]]*\]\((?:<([^>\n]+)>|([^)\s]+))\)$)"));
    if (const auto match = markdown.match(text); match.hasMatch())
        return match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
    auto path = text;
    if (path.size() > 2 && path.startsWith(QLatin1Char('`')) && path.endsWith(QLatin1Char('`')))
        path = path.mid(1, path.size() - 2);
    const bool spaced = std::any_of(path.begin(), path.end(),
                                    [](QChar c) { return c.isSpace() || c == QLatin1Char('`'); });
    if (spaced || !(path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1String("~/")) ||
                    path.startsWith(QLatin1String("file://"))))
        return {};
    return path;
}

// At most this many pictures a reply shows in place; later lone paths stay text.
constexpr int inline_media_limit = 32;
} // namespace

std::vector<Piece> pieces(const QString& markdown, bool* missing) {
    std::vector<Piece> out;
    QStringList prose;
    QStringList code;
    QString fence;
    QString language;
    int shown = 0;
    const auto add_prose = [&out](const QStringList& lines) {
        const auto text = lines.join(QLatin1Char('\n')).trimmed();
        if (!text.isEmpty())
            out.push_back({false, {}, text, {}});
    };
    // A paragraph that is only a media path becomes that media, in place.
    const auto flush_prose = [&] {
        qsizetype from = 0;
        for (qsizetype i = 0; i < prose.size() && shown < inline_media_limit; ++i) {
            const bool alone = (i == 0 || prose[i - 1].trimmed().isEmpty()) &&
                               (i + 1 == prose.size() || prose[i + 1].trimmed().isEmpty());
            if (!alone)
                continue;
            const auto path = lone_path(prose[i]);
            if (path.isEmpty())
                continue;
            const auto file = media_file(path, missing);
            if (!file)
                continue;
            add_prose(prose.mid(from, i - from));
            out.push_back({false, {}, prose[i].trimmed(), *file});
            ++shown;
            from = i + 1;
        }
        add_prose(prose.mid(from));
        prose.clear();
    };
    for (const auto& line : markdown.split(QLatin1Char('\n'))) {
        const auto trimmed = line.trimmed();
        if (fence.isEmpty()) {
            if (trimmed.startsWith(QLatin1String("```")) ||
                trimmed.startsWith(QLatin1String("~~~"))) {
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
            out.push_back({true, language, code.join(QLatin1Char('\n')), {}});
            code.clear();
            fence.clear();
        } else {
            code << line;
        }
    }
    if (!fence.isEmpty()) // an unclosed block still reads as code
        out.push_back({true, language, code.join(QLatin1Char('\n')), {}});
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
    for (const QChar invisible :
         {QChar(0xfffc), QChar(0x200b), QChar(0x200c), QChar(0x200d), QChar(0xfeff), QChar(0x2060)})
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

qint64 context_window(const QString& model, qint64 used) {
    // Claude Code 2.1.319's model table: these have a 1M window natively.
    static const QStringList million{
        QStringLiteral("claude-opus-5"),   QStringLiteral("claude-fable-5"),
        QStringLiteral("claude-sonnet-5"), QStringLiteral("claude-haiku-5"),
        QStringLiteral("claude-opus-4-6"), QStringLiteral("claude-opus-4-7"),
        QStringLiteral("claude-opus-4-8"), QStringLiteral("claude-sonnet-4-6"),
        QStringLiteral("claude-haiku-4-5")};
    const auto name = model.toLower();
    qint64 window = 200'000;
    if (name.contains(QLatin1String("[1m]")) ||
        std::any_of(million.begin(), million.end(),
                    [&name](const QString& prefix) { return name.startsWith(prefix); }))
        window = 1'000'000;
    return std::max(window, used);
}

qint64 context_window(const Status& status) {
    if (status.window_tokens > 0)
        return std::max(status.window_tokens, status.context_tokens);
    return context_window(status.model, status.context_tokens);
}

QString format_name(Format format) {
    switch (format) {
    case Format::codex:
        return QStringLiteral("codex");
    case Format::antigravity:
        return QStringLiteral("antigravity");
    case Format::claude:
        break;
    }
    return QStringLiteral("claude");
}

std::optional<Format> format_named(const QString& name) {
    if (name == QLatin1String("claude"))
        return Format::claude;
    if (name == QLatin1String("codex"))
        return Format::codex;
    if (name == QLatin1String("antigravity") || name == QLatin1String("agy"))
        return Format::antigravity;
    return std::nullopt;
}

Format format_of(const QString& path) {
    const auto name = QFileInfo(path).fileName();
    if (name.startsWith(QLatin1String("rollout-")))
        return Format::codex;
    if (name == QLatin1String("transcript_full.jsonl") || name == QLatin1String("transcript.jsonl"))
        return Format::antigravity;
    return Format::claude;
}

std::unique_ptr<Reader> make_reader(Format format) {
    switch (format) {
    case Format::codex:
        return std::make_unique<CodexReader>();
    case Format::antigravity:
        return std::make_unique<AntigravityReader>();
    case Format::claude:
        break;
    }
    return std::make_unique<ClaudeReader>();
}

void Reader::note_sent(const QString& text) {
    const auto simple = text.simplified();
    if (!simple.isEmpty())
        sent_.push_back({simple, static_cast<int>(items_.size())});
}

QStringList Reader::sent() const {
    QStringList out;
    for (const auto& sent : sent_)
        out << sent.text;
    return out;
}

void Reader::taken(Item& prompt) {
    if (sent_.empty())
        return;
    // The CLI may clip or wrap what it saves, so a prompt's start will do.
    const auto simple = prompt.body.simplified();
    const auto same = [&simple](const Sent& sent) {
        const auto n = std::min<qsizetype>({simple.size(), sent.text.size(), 200});
        return simple == sent.text ||
               (n >= 40 && QStringView(simple).left(n) == QStringView(sent.text).left(n));
    };
    const auto match = std::find_if(sent_.begin(), sent_.end(), same);
    if (match == sent_.end())
        return;
    if (prompt.steered)
        prompt.waited = static_cast<int>(
            std::count_if(items_.begin() + std::min(match->at, static_cast<int>(items_.size())),
                          items_.end(), [](const Item& item) { return item.kind == Kind::tool; }));
    // Prompts are read in order: any sent before it are not coming.
    sent_.erase(sent_.begin(), match + 1);
}

QString chart_style_folder() { return QDir::home().filePath(QStringLiteral(".lapis/visual")); }

std::vector<Media> find_media(const QString& text, const QSet<QString>& skip, bool* missing) {
    // Markdown targets first, then bare paths; quotes, brackets and
    // backticks end a path, as does trailing punctuation.
    static const QRegularExpression markdown(
        QStringLiteral(R"(!?\[[^\]]*\]\((?:<([^>\n]+)>|([^)\s]+))\))"));
    static const QRegularExpression bare(QStringLiteral(
        R"((?:^|[\s`'"(\[@])((?:~|/)[^\s`'"()<>\[\]]+\.([A-Za-z0-9]{2,5}))(?=$|[\s`'".,;:!?)\]]))"));
    std::vector<Media> found;
    QSet<QString> seen;
    const auto consider = [&](const QString& written) {
        if (found.size() >= 8)
            return;
        const auto file = media_file(written, missing);
        if (!file || seen.contains(file->path) || skip.contains(file->path))
            return;
        seen.insert(file->path);
        found.push_back(*file);
    };
    for (auto it = markdown.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        consider(match.captured(1).isEmpty() ? match.captured(2) : match.captured(1));
    }
    for (auto it = bare.globalMatch(text); it.hasNext();)
        consider(it.next().captured(1));
    return found;
}

QString path_at(const QString& text, int position, const QString& folder) {
    if (position < 0 || position > text.size())
        return {};
    const auto stops = [](QChar c) {
        return c.isSpace() || c == QChar::ParagraphSeparator || c == QChar::LineSeparator ||
               QStringLiteral("\"'`()<>[]{},;|").contains(c);
    };
    qsizetype start = position, end = position;
    while (start > 0 && !stops(text.at(start - 1)))
        --start;
    while (end < text.size() && !stops(text.at(end)))
        ++end;
    auto word = text.mid(start, end - start);
    if (word.startsWith(QLatin1Char('@')))
        word.remove(0, 1); // an @ mention
    while (!word.isEmpty() && QStringLiteral(".:!?").contains(word.back()))
        word.chop(1);
    if (word.startsWith(QLatin1String("file://")))
        word = QUrl(word).toLocalFile();
    // "src/a.cpp:42" or "a.cpp:42:7": the line (and column) stay with it.
    static const QRegularExpression line(QStringLiteral(R"((:\d+){1,2}$)"));
    const auto suffix = line.match(word).captured(0);
    auto path = word.left(word.size() - suffix.size());
    if (path.isEmpty())
        return {};
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/")))
        path = QDir::home().filePath(path.mid(2));
    else if (!path.startsWith(QLatin1Char('/'))) {
        // Relative paths need a folder and must look like a path, not a word.
        if (folder.isEmpty() ||
            !(path.contains(QLatin1Char('/')) || path.contains(QLatin1Char('.'))))
            return {};
        path = QDir(folder).filePath(path);
    }
    const QFileInfo info(path);
    if (!info.exists())
        return {};
    return QDir::cleanPath(info.absoluteFilePath()) + suffix;
}

namespace {
// The rows above Claude Code's input box, trailing spaces trimmed.
QStringList above_input_box(const QStringList& screen) {
    // A row that is mostly a horizontal box rule: an input box's edge.
    const auto rule = [](const QString& row) {
        const auto trimmed = row.trimmed();
        if (trimmed.size() < 20)
            return false;
        qsizetype box = 0;
        for (const auto c : trimmed)
            if (c.unicode() >= 0x2500 && c.unicode() <= 0x257F)
                ++box;
        return box * 10 >= trimmed.size() * 8;
    };
    qsizetype cut = screen.size();
    // The input box: the lowest rule, then the rule above it (its top edge).
    qsizetype found = 0;
    for (qsizetype row = screen.size() - 1; row >= 0 && found < 2; --row) {
        if (rule(screen.at(row))) {
            cut = row;
            ++found;
        }
    }
    QStringList out = screen.mid(0, cut);
    for (auto& row : out) {
        while (!row.isEmpty() && row.back().isSpace())
            row.chop(1);
    }
    return out;
}

// An echoed prompt: "❯ text" (or the older "> text") at column 0.
bool prompt_row(const QString& row) {
    return (row.startsWith(QChar(0x276F)) || row.startsWith(QLatin1Char('>'))) &&
           (row.size() == 1 || row.at(1).isSpace());
}

// A message or step: led by "●" or "⏺" at column 0.
bool block_row(const QString& row) {
    return row.startsWith(QChar(0x25CF)) || row.startsWith(QChar(0x23FA));
}

// A status row: a spinner glyph at column 0, then a space ("✻ Thinking…").
bool status_row(const QString& row) {
    if (row.size() < 3 || !row.at(1).isSpace())
        return false;
    static const QString glyphs = QStringLiteral("✻✶✳✢✽✺✹✸✷·*∗");
    return glyphs.contains(row.at(0));
}

// Letters and digits only, lowercased: text as the screen and the markdown
// both show it, whatever the markup.
QString letters(QStringView text, qsizetype limit) {
    QString out;
    for (const auto c : text) {
        if (c.isLetterOrNumber())
            out += c.toLower();
        if (out.size() >= limit)
            break;
    }
    return out;
}
} // namespace

bool compacting_shown(const QStringList& screen) {
    const auto rows = above_input_box(screen);
    for (auto row = rows.size() - 1; row >= 0; --row) {
        if (rows.at(row).trimmed().isEmpty())
            continue;
        if (status_row(rows.at(row)))
            return rows.at(row).contains(QLatin1String("Compacting conversation"));
        // Claude Code draws hints under its spinner; look a few rows up.
        if (rows.size() - row > 4)
            break;
    }
    return false;
}

QStringList live_lines(const QStringList& screen, const QStringList& shown) {
    if (compacting_shown(screen))
        return {};
    QStringList out = above_input_box(screen);
    // What came before the last echoed prompt is earlier turns'.
    qsizetype start = 0;
    for (auto row = out.size() - 1; row >= 0; --row)
        if (prompt_row(out.at(row))) {
            start = row + 1;
            break;
        }
    // Of this turn's, only the last block is still being written; the
    // blocks above it are finished and already in the thread.
    qsizetype block = -1;
    for (auto row = out.size() - 1; row >= start && block < 0; --row)
        if (block_row(out.at(row)))
            block = row;
    if (block >= 0) {
        start = block;
        // A reply the thread already shows in full: keep only what follows.
        const auto head = letters(QStringView(out.at(block)).mid(1), 24);
        const bool rendered =
            head.size() >= 8 && std::any_of(shown.begin(), shown.end(), [&head](const auto& text) {
                return letters(text, head.size()) == head;
            });
        if (rendered) {
            // Only the live status row: earlier ones ("Waiting for 3
            // background agents") ended with the reply.
            for (auto row = out.size() - 1; row >= block; --row)
                if (status_row(out.at(row)))
                    return {out.at(row)};
            return {};
        }
    }
    out = out.mid(start);
    while (!out.isEmpty() && out.front().trimmed().isEmpty())
        out.removeFirst();
    while (!out.isEmpty() && out.back().trimmed().isEmpty())
        out.removeLast();
    return out;
}

bool dialog_showing(const QStringList& screen) {
    // Only the input box and a dialog's top edge span the screen from column
    // 0; boxes, sliders and code samples inside a dialog are indented or drawn
    // with other characters.
    const auto full_rule = [](const QString& row) {
        qsizetype end = row.size();
        while (end > 0 && row[end - 1].isSpace())
            --end;
        if (end < 20)
            return false;
        for (qsizetype i = 0; i < end; ++i)
            if (row[i] != QChar(0x2500))
                return false;
        return true;
    };
    qsizetype lowest = -1, above = -1;
    for (qsizetype row = screen.size() - 1; row >= 0 && above < 0; --row) {
        if (!full_rule(screen.at(row)))
            continue;
        (lowest < 0 ? lowest : above) = row;
    }
    if (lowest < 0)
        return false;
    // The input box: its first row is the prompt (or a blank start), and a
    // draft of several lines keeps it under a dozen rows.
    const auto between = lowest - above - 1;
    if (above >= 0 && between >= 1 && between <= 12) {
        const auto first = screen.at(above + 1).trimmed();
        if (first.isEmpty() || first.startsWith(QChar(0x276F)) ||
            first.startsWith(QLatin1Char('>')) || first.startsWith(QLatin1Char('!')) ||
            first.startsWith(QLatin1Char('#')))
            return false;
    }
    return true;
}

QString work_summary(const std::vector<Item>& items, int first, int last, int except) {
    int commands = 0, searches = 0, web = 0, agents = 0, tools = 0;
    QSet<QString> edited, read;
    for (int i = first; i <= last; ++i) {
        if (i == except)
            continue;
        const auto& item = items[static_cast<size_t>(i)];
        if (item.kind != Kind::tool)
            continue;
        const auto& verb = item.title;
        if (verb == QLatin1String("Ran"))
            ++commands;
        else if (verb == QLatin1String("Edited") || verb == QLatin1String("Wrote") ||
                 verb == QLatin1String("Deleted")) // Deleted: Codex's patches
            edited.insert(item.body);
        else if (verb == QLatin1String("Read"))
            read.insert(item.body);
        else if (verb == QLatin1String("Searched for") || verb == QLatin1String("Listed"))
            ++searches;
        else if (verb.startsWith(QLatin1String("Searched the web")) ||
                 verb == QLatin1String("Fetched"))
            ++web;
        else if (verb == QLatin1String("Started agent"))
            ++agents;
        else
            ++tools;
    }
    const auto counted = [](int n, const char* one, const char* many) {
        return n == 1 ? QString::fromUtf8(one) : QString::fromUtf8(many).arg(n);
    };
    QStringList parts;
    if (commands > 0)
        parts << counted(commands, "ran a command", "ran %1 commands");
    if (!edited.isEmpty())
        parts << counted(static_cast<int>(edited.size()), "edited a file", "edited %1 files");
    if (!read.isEmpty())
        parts << counted(static_cast<int>(read.size()), "read a file", "read %1 files");
    if (searches > 0)
        parts << counted(searches, "searched once", "searched %1 times");
    if (web > 0)
        parts << QStringLiteral("used the web");
    if (agents > 0)
        parts << counted(agents, "started an agent", "started %1 agents");
    if (tools > 0)
        parts << counted(tools, "called a tool", "called %1 tools");
    return parts.join(QStringLiteral(", "));
}

Layout lay_out(const std::vector<Item>& items, const QSet<int>& expanded, bool live) {
    Layout out;
    const int count = static_cast<int>(items.size());
    std::vector<int> turn_of(items.size(), -1);
    int open = -1;
    for (int i = 0; i < count; ++i) {
        const auto& item = items[static_cast<size_t>(i)];
        if (item.dropped)
            continue;
        if (item.kind == Kind::turn) {
            // The CLI's own "turn took" record ends the turn and times it.
            if (open >= 0) {
                auto& turn = out.turns[static_cast<size_t>(open)];
                turn.duration_ms = item.duration_ms;
                turn.last = i;
                turn.closed = true;
                turn_of[static_cast<size_t>(i)] = open;
                open = -1;
            }
            continue;
        }
        if (item.between_turns && open >= 0) {
            out.turns[static_cast<size_t>(open)].closed = true;
            open = -1;
        }
        // Text and an image sent together are one prompt in two items.
        const bool same_prompt = open >= 0 && item.kind == Kind::prompt && i > 0 &&
                                 items[static_cast<size_t>(i - 1)].kind == Kind::prompt &&
                                 items[static_cast<size_t>(i - 1)].record == item.record;
        if (item.starts_turn && !same_prompt) {
            if (open >= 0)
                out.turns[static_cast<size_t>(open)].closed = true;
            out.turns.push_back({.start = i,
                                 .last = i,
                                 .final_reply = -1,
                                 .steps = 0,
                                 .duration_ms = 0,
                                 .closed = false,
                                 .summary = {}});
            open = static_cast<int>(out.turns.size()) - 1;
        }
        if (open >= 0) {
            turn_of[static_cast<size_t>(i)] = open;
            out.turns[static_cast<size_t>(open)].last = i;
        }
    }

    // The end of the prompt that starts at `at`: text and an image sent
    // together are one prompt in several items of one record.
    const auto prompt_end = [&items](int at, int last) {
        int end = at;
        while (end + 1 <= last && items[static_cast<size_t>(end + 1)].kind == Kind::prompt &&
               items[static_cast<size_t>(end + 1)].record == items[static_cast<size_t>(at)].record)
            ++end;
        return end;
    };
    const auto at_of = [&items](int i) { return items[static_cast<size_t>(i)].at_ms; };
    // Each turn's prompt, its parts (split at the prompts it read mid-turn),
    // its final reply, and how much folds between.
    std::vector<int> header_end(out.turns.size());
    for (size_t t = 0; t < out.turns.size(); ++t) {
        auto& turn = out.turns[t];
        const int end = prompt_end(turn.start, turn.last);
        header_end[t] = end;
        turn.live = live && t + 1 == out.turns.size() && !turn.closed;
        turn.parts.push_back({.lead = turn.start, .first = end + 1});
        for (int i = end + 1; i <= turn.last; ++i) {
            const auto& item = items[static_cast<size_t>(i)];
            if (item.kind != Kind::prompt || item.queued || item.dropped)
                continue;
            // A steering prompt (its first item) leads the next part.
            const int lead = i;
            turn.parts.back().last = i - 1;
            i = prompt_end(i, turn.last);
            turn.parts.push_back({.lead = lead, .first = i + 1});
        }
        turn.parts.back().last = turn.last;
        // The final reply is the last part's last reply; a running turn's
        // replies so far are interim, listed in the live box.
        for (int i = turn.last; !turn.live && i >= turn.parts.back().first; --i)
            if (items[static_cast<size_t>(i)].kind == Kind::reply &&
                !items[static_cast<size_t>(i)].dropped) {
                turn.final_reply = i;
                break;
            }
        if (turn.duration_ms <= 0) {
            const auto first = at_of(turn.start);
            const auto last = at_of(turn.last);
            if (first > 0 && last > first)
                turn.duration_ms = last - first;
        }
        for (size_t p = 0; p < turn.parts.size(); ++p) {
            auto& part = turn.parts[p];
            for (int i = part.first; i <= part.last; ++i) {
                const auto& item = items[static_cast<size_t>(i)];
                // A prompt sent mid-turn stays in view, not folded with the work.
                if (i != turn.final_reply && item.kind != Kind::turn && item.kind != Kind::prompt &&
                    !item.dropped)
                    ++part.steps;
            }
            turn.steps += part.steps;
            const bool running = turn.live && p + 1 == turn.parts.size();
            if (part.steps > 0 && !running)
                part.summary = work_summary(items, part.first, part.last, turn.final_reply);
            // A part lasts from its prompt to the next one; the last, to the
            // turn's end.
            if (turn.parts.size() == 1) {
                part.duration_ms = turn.duration_ms;
            } else if (p + 1 < turn.parts.size()) {
                const auto from = at_of(part.lead), to = at_of(turn.parts[p + 1].lead);
                part.duration_ms = from > 0 && to > from ? to - from : 0;
            } else {
                const auto start = at_of(turn.start), from = at_of(part.lead);
                part.duration_ms = start > 0 && from >= start && turn.duration_ms > from - start
                                       ? turn.duration_ms - (from - start)
                                       : 0;
            }
        }
        if (turn.closed && turn.steps > 0)
            turn.summary = work_summary(items, end + 1, turn.last, turn.final_reply);
    }

    out.rows.reserve(items.size());
    for (int i = 0; i < count; ++i) {
        const int t = turn_of[static_cast<size_t>(i)];
        const auto kind = items[static_cast<size_t>(i)].kind;
        if (items[static_cast<size_t>(i)].dropped)
            continue;
        if (t < 0) {
            if (kind != Kind::turn)
                out.rows.push_back({.item = i, .turn = -1});
            continue;
        }
        if (i != out.turns[static_cast<size_t>(t)].start)
            continue; // a turn's rows are made at its start, below
        const auto& turn = out.turns[static_cast<size_t>(t)];
        const auto shown = [&items](int at) {
            const auto& item = items[static_cast<size_t>(at)];
            // A background task's wake-up starts a turn but is not worth a line.
            return !item.dropped && !item.quiet && item.kind != Kind::turn;
        };
        for (int at = turn.start; at <= header_end[static_cast<size_t>(t)]; ++at)
            if (shown(at))
                out.rows.push_back({.item = at, .turn = t});
        for (size_t p = 0; p < turn.parts.size(); ++p) {
            const auto& part = turn.parts[p];
            const int number = static_cast<int>(p);
            if (p > 0)
                for (int at = part.lead; at < part.first; ++at)
                    if (shown(at))
                        out.rows.push_back({.item = at, .turn = t});
            if (turn.live && p + 1 == turn.parts.size()) {
                out.rows.push_back({.item = -1, .turn = t, .part = number, .live = true});
                continue;
            }
            if (part.steps == 0)
                continue;
            out.rows.push_back({.item = -1, .turn = t, .part = number});
            if (!expanded.contains(part.lead))
                continue;
            for (int at = part.first; at <= part.last; ++at) {
                const auto& item = items[static_cast<size_t>(at)];
                if (at != turn.final_reply && item.kind != Kind::prompt && shown(at))
                    out.rows.push_back({.item = at, .turn = t, .in_work = true});
            }
        }
        // The final reply closes the turn, after any work shown above it.
        if (turn.final_reply >= 0 && !turn.live)
            out.rows.push_back({.item = turn.final_reply, .turn = t});
        // Prompts still waiting in the CLI's queue come last, in order.
        for (int at = header_end[static_cast<size_t>(t)] + 1; at <= turn.last; ++at) {
            const auto& item = items[static_cast<size_t>(at)];
            if (item.kind == Kind::prompt && item.queued && !item.dropped)
                out.rows.push_back({.item = at, .turn = t});
        }
    }
    return out;
}

std::vector<int> live_items(const std::vector<Item>& items, const Layout& layout, int turn) {
    std::vector<int> out;
    if (turn < 0 || std::cmp_greater_equal(turn, layout.turns.size()))
        return out;
    const auto& part = layout.turns[static_cast<size_t>(turn)].parts.back();
    for (int at = part.first; at <= part.last; ++at) {
        const auto& item = items[static_cast<size_t>(at)];
        if (!item.dropped && !item.quiet && item.kind != Kind::turn && item.kind != Kind::prompt)
            out.push_back(at);
    }
    return out;
}

void ClaudeReader::push(Item item) {
    item.record = record_;
    item.at_ms = at_ms_;
    item.after_step = is_step(item.kind) && !items_.empty() && is_step(items_.back().kind);
    items_.push_back(std::move(item));
}

void ClaudeReader::command(const QString& raw) {
    const auto name = tagged(raw, u"command-name");
    status_.compacting_since = 0;
    // The record a started /compact gets when it ends: already shown.
    if (compact_item_ >= 0 && name == QLatin1String("/compact"))
        return;
    compact_item_ = -1;
    auto item = make(Kind::command, name, tagged(raw, u"command-args"));
    item.between_turns = name == QLatin1String("/compact");
    push(std::move(item));
}

int ClaudeReader::command_output(const QString& raw) {
    // /model and /effort take effect now, before the next answer reports them.
    const auto said = without_escapes(tagged(raw, u"local-command-stdout")).trimmed();
    const auto first = said.section(QLatin1Char('\n'), 0, 0);
    static const QRegularExpression model(
        QStringLiteral(R"(^Set model to (.+?)(?: and saved| for this session|$))"));
    static const QRegularExpression effort(QStringLiteral(R"(^Set effort level to ([a-z]+))"));
    if (const auto set = model.match(first); set.hasMatch()) {
        if (const auto id = model_id(set.captured(1)); !id.isEmpty())
            status_.model = id;
    } else if (const auto level = effort.match(first); level.hasMatch()) {
        status_.effort = level.captured(1);
    }
    QStringList parts;
    for (const auto tag :
         {u"local-command-stdout", u"local-command-stderr", u"bash-stdout", u"bash-stderr"}) {
        if (const auto text = without_escapes(tagged(raw, tag)).trimmed(); !text.isEmpty())
            parts << text;
    }
    // Output belongs to the command just run; with anything else between
    // (a compaction, a reply) there is no telling which command it answers,
    // except a /compact's, which comes after the compaction it ran.
    int at = -1;
    if (!items_.empty() && items_.back().kind == Kind::command)
        at = static_cast<int>(items_.size()) - 1;
    else if (compact_item_ >= 0)
        at = compact_item_;
    compact_item_ = -1;
    if (parts.isEmpty() || at < 0)
        return -1;
    auto& item = items_[static_cast<size_t>(at)];
    // stdout and stderr can arrive as separate records for one command.
    const auto text = item.result.isEmpty()
                          ? parts.join(QLatin1Char('\n'))
                          : item.result + QLatin1Char('\n') + parts.join(QLatin1Char('\n'));
    item.result_lines = static_cast<int>(text.count(QLatin1Char('\n'))) + 1;
    // A table (/context) reads whole, so only its size is bounded, not its lines.
    item.result = clip(text, result_preview_chars);
    return at;
}

int ClaudeReader::add(const QByteArray& line) {
    // Claude Code's request logs and file snapshots are a quarter of a long
    // conversation's bytes and never shown: skip them by the type they lead
    // with, before parsing.
    if (line.startsWith("{\"type\":\"")) {
        const auto type = QByteArrayView(line).mid(9, 24);
        for (const auto prefix : {QByteArrayView("api-request"), QByteArrayView("file-history-"),
                                  QByteArrayView("ai-title\""), QByteArrayView("last-prompt\""),
                                  QByteArrayView("pr-link\""), QByteArrayView("frame-link\"")})
            if (type.startsWith(prefix))
                return -1;
    }
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return -1;
    const auto record = document.object();
    if (record.value(QLatin1String("isSidechain")).toBool())
        return -1;
    const auto type = record.value(QLatin1String("type")).toString();
    record_ = record.value(QLatin1String("uuid")).toString();
    const auto stamp = record.value(QLatin1String("timestamp")).toString();
    at_ms_ =
        stamp.isEmpty() ? 0 : QDateTime::fromString(stamp, Qt::ISODateWithMs).toMSecsSinceEpoch();
    const auto first = static_cast<int>(items_.size());
    int changed = -1;

    if (type == QLatin1String("system")) {
        const auto subtype = record.value(QLatin1String("subtype")).toString();
        const auto content = record.value(QLatin1String("content")).toString();
        if (subtype == QLatin1String("compact_boundary")) {
            // The context is now the summary; the size before no longer applies.
            status_.context_tokens =
                static_cast<qint64>(record.value(QLatin1String("compactMetadata"))
                                        .toObject()
                                        .value(QLatin1String("postTokens"))
                                        .toDouble());
            status_.compacting_since = 0;
            push(make(Kind::divider, QStringLiteral("Conversation compacted")));
        } else if (subtype == QLatin1String("turn_duration")) {
            const auto ms =
                static_cast<qint64>(record.value(QLatin1String("durationMs")).toDouble());
            if (ms > 0) {
                auto item =
                    make(Kind::turn, QStringLiteral("Worked for %1").arg(duration_text(ms)));
                item.duration_ms = ms;
                push(std::move(item));
            }
        } else if (subtype == QLatin1String("api_error") ||
                   subtype.startsWith(QLatin1String("model_refusal"))) {
            auto item = make(Kind::notice, content.isEmpty() ? subtype : clip(content, 300));
            item.failed = true;
            push(std::move(item));
        } else if (subtype == QLatin1String("away_summary") && !content.isEmpty()) {
            push(make(Kind::notice, clip(content, 600)));
        } else if (subtype == QLatin1String("local_command")) {
            // A command run while a dialog was involved is recorded here
            // rather than as the person's message; it reads the same.
            if (content.contains(QLatin1String("<command-name>")))
                command(content);
            else
                changed = command_output(content);
        } else if (subtype == QLatin1String("informational") ||
                   subtype == QLatin1String("model_consent_fallback") ||
                   subtype == QLatin1String("scheduled_task_fire")) {
            const auto text = without_escapes(content).trimmed();
            if (!text.isEmpty()) {
                auto item = make(Kind::notice, clip(text, 300));
                // A scheduled wakeup prompts the agent, as a background task does.
                item.starts_turn = subtype == QLatin1String("scheduled_task_fire");
                push(std::move(item));
            }
        }
        return static_cast<int>(items_.size()) > first ? first : changed;
    }
    // A prompt sent while the agent works waits in Claude Code's queue: shown
    // at once as queued, then taken in by the running turn (absorbed), sent
    // on as the next turn (dequeued, and recorded again as a prompt), or
    // withdrawn.
    if (type == QLatin1String("queue-operation")) {
        const auto operation = record.value(QLatin1String("operation")).toString();
        const auto text = record.value(QLatin1String("content")).toString();
        if (operation == QLatin1String("enqueue")) {
            // Background task notices are not the person's words.
            // A prompt that starts with a paste is still the person's.
            if (text.trimmed().isEmpty() || (text.startsWith(QLatin1Char('<')) &&
                                             !text.startsWith(QLatin1String("<pasted_content")))) {
                queued_.emplace_back(text, -1);
                return -1;
            }
            auto item = make(Kind::prompt, {}, clip(unwrap_pastes(text).trimmed(), body_chars));
            item.queued = true;
            queued_.emplace_back(text, first);
            push(std::move(item));
            return first;
        }
        auto waiting = queued_.begin();
        if (operation != QLatin1String("dequeue") || !text.isEmpty())
            waiting = std::find_if(queued_.begin(), queued_.end(),
                                   [&text](const auto& entry) { return entry.first == text; });
        if (waiting == queued_.end())
            return -1;
        const int at = waiting->second;
        queued_.erase(waiting);
        if (at < 0)
            return -1;
        auto& item = items_[static_cast<size_t>(at)];
        item.queued = false;
        item.dropped = true;
        if (record.value(QLatin1String("reason")).toString() ==
            QLatin1String("absorbed_mid_turn")) {
            // Read by the running turn between two steps: it moves to where
            // it was read (Claude Code then records it as a queued_command
            // attachment), counting the steps it waited.
            auto steered = item;
            steered.dropped = false;
            steered.steered = true;
            steered.waited = static_cast<int>(
                std::count_if(items_.begin() + at + 1, items_.end(),
                              [](const Item& step) { return step.kind == Kind::tool; }));
            push(std::move(steered));
        }
        return at;
    }
    // Records without a message (titles, modes, file snapshots, attachments)
    // are the CLI's bookkeeping, not conversation.
    const auto message = record.value(QLatin1String("message")).toObject();
    if (message.isEmpty() || (type != QLatin1String("user") && type != QLatin1String("assistant")))
        return -1;
    const bool meta = record.value(QLatin1String("isMeta")).toBool();
    const auto content = message.value(QLatin1String("content"));
    const auto thought_ms =
        static_cast<qint64>(record.value(QLatin1String("thinkingDurationMs")).toDouble());

    bool prompted = false; // this record carried prompt text of its own
    const auto user_text = [&](const QString& raw) {
        if (raw.startsWith(QLatin1String("<local-command-std")) ||
            raw.startsWith(QLatin1String("<bash-std"))) {
            if (const int at = command_output(raw); at >= 0)
                changed = changed < 0 ? at : std::min(changed, at);
            return;
        }
        if (meta && raw.startsWith(QLatin1String("[Image: source: "))) {
            // Where a pasted image was saved, written after the prompt that
            // names it ("[Image #1]"): the prompt shows it when it exists.
            const auto end = raw.lastIndexOf(QLatin1Char(']'));
            const auto path = raw.mid(16, end < 16 ? -1 : end - 16).trimmed();
            for (auto at = static_cast<int>(items_.size()) - 1; at >= 0; --at) {
                auto& item = items_[static_cast<size_t>(at)];
                if (item.kind != Kind::prompt)
                    continue;
                if (!path.isEmpty() && !item.detail.contains(path))
                    item.detail +=
                        (item.detail.isEmpty() ? QString() : QStringLiteral("\n")) + path;
                changed = changed < 0 ? at : std::min(changed, at);
                break;
            }
            return;
        }
        if (meta || raw.startsWith(QLatin1String("<local-command-")))
            return;
        if (raw.startsWith(QLatin1String("<bash-input>"))) {
            // A "!" command runs in the shell; it does not prompt the agent.
            push(make(Kind::command, QStringLiteral("!"), tagged(raw, u"bash-input")));
            return;
        }
        if (raw.startsWith(QLatin1String("[Request interrupted by user"))) {
            push(make(Kind::notice, QStringLiteral("Interrupted")));
            return;
        }
        if (raw.startsWith(QLatin1String("<task-notification>"))) {
            // A background task waking the agent starts a turn of its own.
            auto summary = tagged(raw, u"summary");
            auto item =
                make(Kind::notice, summary.isEmpty() ? QStringLiteral("Background task finished")
                                                     : clip(summary, 300));
            item.starts_turn = true;
            item.quiet = true; // the turn's "Worked for" line says enough
            push(std::move(item));
            return;
        }
        if (raw.startsWith(QLatin1String("This session is being continued"))) {
            // Right after the compaction's own divider it is the same event.
            if (items_.empty() || items_.back().kind != Kind::divider)
                push(make(Kind::divider, QStringLiteral("Continued from an earlier conversation")));
            return;
        }
        if (raw.contains(QLatin1String("<command-name>"))) {
            command(raw);
            return;
        }
        const auto text = raw.trimmed();
        if (text == QLatin1String("/compact") || text.startsWith(QLatin1String("/compact "))) {
            // Claude Code writes a /compact as the person's text when the
            // compaction starts, and its command record when it ends. It
            // runs between turns: it starts none and joins none.
            auto item = make(Kind::command, QStringLiteral("/compact"),
                             text.mid(QLatin1String("/compact").size()).trimmed());
            item.between_turns = true;
            compact_item_ = static_cast<int>(items_.size());
            status_.compacting_since = at_ms_;
            push(std::move(item));
            return;
        }
        status_.compacting_since = 0;
        compact_item_ = -1;
        auto item = make(Kind::prompt, {}, clip(unwrap_pastes(text).trimmed(), body_chars));
        item.starts_turn = true;
        push(std::move(item));
        prompted = true;
    };

    // What the agent reports with each answer: model, effort, context use.
    if (type == QLatin1String("assistant")) {
        const auto model = message.value(QLatin1String("model")).toString();
        if (!model.isEmpty() && !model.startsWith(QLatin1Char('<'))) // "<synthetic>" errors
            status_.model = model;
        if (const auto effort = record.value(QLatin1String("effort")).toString(); !effort.isEmpty())
            status_.effort = effort;
        const auto usage = message.value(QLatin1String("usage")).toObject();
        const auto tokens = [&usage](const char* key) {
            return static_cast<qint64>(usage.value(QLatin1String(key)).toDouble());
        };
        const auto used = tokens("input_tokens") + tokens("cache_read_input_tokens") +
                          tokens("cache_creation_input_tokens") + tokens("output_tokens");
        if (used > 0)
            status_.context_tokens = used;
    }

    if (content.isString()) {
        if (type == QLatin1String("user"))
            user_text(content.toString());
        else
            push(make(Kind::reply, {}, clip(content.toString().trimmed(), body_chars)));
    }
    QStringList attached; // "[image]", "[document]" in order
    for (const auto& value : content.toArray()) {
        const auto block = value.toObject();
        const auto kind = block.value(QLatin1String("type")).toString();
        if (kind == QLatin1String("text")) {
            const auto text = block.value(QLatin1String("text")).toString();
            if (type == QLatin1String("user"))
                user_text(text);
            else if (!text.trimmed().isEmpty())
                push(make(Kind::reply, {}, clip(text.trimmed(), body_chars)));
        } else if (kind == QLatin1String("thinking") ||
                   kind == QLatin1String("redacted_thinking")) {
            push(make(
                Kind::thinking,
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
                auto& step = items_[static_cast<size_t>(at)];
                attach_result(step, block);
                if (step.at_ms > 0 && at_ms_ > step.at_ms)
                    step.duration_ms = at_ms_ - step.at_ms; // call to result
                changed = changed < 0 ? at : std::min(changed, at);
            }
        } else if (kind == QLatin1String("image") || kind == QLatin1String("document")) {
            if (!meta)
                attached << QStringLiteral("[%1]").arg(kind);
        } else {
            push(make(Kind::other, kind, {}, compact_json(block, 400)));
        }
    }
    // Text sent with an image already names it ("[Image #2] is this good?");
    // an attachment stands as the prompt only when the record has no text.
    if (!prompted) {
        for (const auto& name : attached) {
            auto item = make(Kind::prompt, {}, name);
            item.starts_turn = true;
            push(std::move(item));
        }
    }
    if (static_cast<int>(items_.size()) > first)
        changed = changed < 0 ? first : std::min(changed, first);
    return changed;
}
} // namespace transcript

namespace {
struct ReadResult {
    std::unique_ptr<transcript::Reader> reader;
    qint64 offset{};
    QByteArray pending;
};

// Feeds `bytes` (after `pending`, a line started earlier) to the reader and
// keeps a trailing unfinished line for next time. Returns the first item a
// line changed, or -1.
int feed(transcript::Reader& reader, QByteArray& pending, const QByteArray& bytes) {
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

ReadResult read_whole(const QString& path, transcript::Format format) {
    ReadResult result{transcript::make_reader(format), 0, {}};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return result;
    while (!file.atEnd())
        feed(*result.reader, result.pending, file.read(qint64{1} << 20));
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

TranscriptModel::TranscriptModel(QObject* parent)
    : QAbstractListModel(parent), reader_(transcript::make_reader(transcript::Format::claude)) {}

transcript::Format TranscriptModel::format_for(const QString& path) const {
    return forced_format_ ? *forced_format_ : transcript::format_of(path);
}

int TranscriptModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(layout_.rows.size());
}

QVariant TranscriptModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || std::cmp_greater_equal(index.row(), layout_.rows.size()))
        return {};
    const auto& row = layout_.rows[static_cast<size_t>(index.row())];
    if (row.item < 0) {
        // A part's "Worked for" row, standing for its folded work, or the
        // running turn's live work box.
        const auto& turn = layout_.turns[static_cast<size_t>(row.turn)];
        const auto& part = turn.parts[static_cast<size_t>(row.part)];
        switch (role) {
        case KindRole:
            return row.live ? QStringLiteral("live") : QStringLiteral("work");
        case TitleRole:
            if (row.live)
                return QStringLiteral("Working");
            return part.duration_ms > 0 ? QStringLiteral("Worked for %1")
                                              .arg(transcript::duration_text(part.duration_ms))
                                        : QStringLiteral("Worked");
        case StepsRole:
            return part.steps;
        case DetailRole:
            return part.summary;
        case AtRole:
            return reader_->items()[static_cast<size_t>(row.live ? turn.start : part.lead)].at_ms;
        case DurationRole:
            return part.duration_ms;
        case ExpandedRole:
            return !row.live && expanded_.contains(part.lead);
        case DiffRole:
            return QVariantList{};
        case AddedRole:
        case RemovedRole:
        case ResultLinesRole:
            return 0;
        case WaitedRole:
        case ItemRole:
            return -1;
        case QueuedRole:
        case FailedRole:
        case AnsweredRole:
        case AfterStepRole:
        case InWorkRole:
        case SteeredRole:
            return false;
        default:
            return QString();
        }
    }
    switch (role) {
    case InWorkRole:
        return row.in_work;
    case ExpandedRole:
        return false;
    case StepsRole:
        return 0;
    default:
        return item_data(reader_->items()[static_cast<size_t>(row.item)], row.item, role);
    }
}

QVariant TranscriptModel::item_data(const transcript::Item& item, int index, int role) {
    switch (role) {
    case InWorkRole:
    case ExpandedRole:
        return false;
    case StepsRole:
        return 0;
    case ItemRole:
        return index;
    case SteeredRole:
        return item.steered;
    case WaitedRole:
        return item.waited;
    case KindRole:
        return transcript::kind_name(item.kind);
    case TitleRole:
        return item.title;
    case BodyRole:
        return item.body;
    case DetailRole:
        return item.detail;
    case ResultRole:
        return item.result;
    case DiffRole:
        return diff_rows(item.diff);
    case AddedRole:
        return item.added;
    case RemovedRole:
        return item.removed;
    case ResultLinesRole:
        return item.result_lines;
    case FailedRole:
        return item.failed;
    case AnsweredRole:
        return item.answered;
    case AfterStepRole:
        return item.after_step;
    case RecordRole:
        return item.record;
    case QueuedRole:
        return item.queued;
    case AtRole:
        return item.at_ms;
    case DurationRole:
        return item.duration_ms;
    default:
        return {};
    }
}

QHash<int, QByteArray> TranscriptModel::roleNames() const { return role_names(); }

QHash<int, QByteArray> TranscriptModel::role_names() {
    return {
        {KindRole, "kind"},         {TitleRole, "title"},       {BodyRole, "body"},
        {DetailRole, "detail"},     {ResultRole, "result"},     {DiffRole, "diff"},
        {AddedRole, "added"},       {RemovedRole, "removed"},   {ResultLinesRole, "resultLines"},
        {FailedRole, "failed"},     {AnsweredRole, "answered"}, {AfterStepRole, "afterStep"},
        {RecordRole, "record"},     {ExpandedRole, "expanded"}, {InWorkRole, "inWork"},
        {StepsRole, "steps"},       {QueuedRole, "queued"},     {AtRole, "at"},
        {DurationRole, "duration"}, {SteeredRole, "steered"},   {WaitedRole, "waited"},
        {ItemRole, "item"}};
}

LiveStepsModel::LiveStepsModel(TranscriptModel* owner) : owner_(owner) {}

int LiveStepsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant LiveStepsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || std::cmp_greater_equal(index.row(), items_.size()))
        return {};
    const int at = items_[static_cast<size_t>(index.row())];
    return TranscriptModel::item_data(owner_->items()[static_cast<size_t>(at)], at, role);
}

QHash<int, QByteArray> LiveStepsModel::roleNames() const { return TranscriptModel::role_names(); }

QVariantMap LiveStepsModel::entry(int row) const {
    QVariantMap out;
    if (row < 0 || std::cmp_greater_equal(row, items_.size()))
        return out;
    const auto names = roleNames();
    for (auto name = names.cbegin(); name != names.cend(); ++name)
        out.insert(QString::fromUtf8(name.value()), data(index(row), name.key()));
    return out;
}

int LiveStepsModel::rowOf(int item) const {
    const auto at = std::ranges::find(items_, item);
    return at == items_.end() ? -1 : static_cast<int>(at - items_.begin());
}

void LiveStepsModel::show(std::vector<int> items, int changed) {
    const auto old_size = items_.size();
    if (items.size() > old_size && std::equal(items_.begin(), items_.end(), items.begin())) {
        // The running turn wrote more: new rows at the end.
        beginInsertRows({}, static_cast<int>(old_size), static_cast<int>(items.size() - 1));
        items_ = std::move(items);
        endInsertRows();
        emit countChanged();
    } else if (items != items_) {
        beginResetModel();
        items_ = std::move(items);
        endResetModel();
        emit countChanged();
        return;
    }
    // Rows kept whose item changed (a result came in) are redrawn.
    int first = -1, last = -1;
    for (size_t i = 0; i < std::min(old_size, items_.size()); ++i)
        if (items_[i] >= changed) {
            if (first < 0)
                first = static_cast<int>(i);
            last = static_cast<int>(i);
        }
    if (first >= 0)
        emit dataChanged(index(first), index(last));
}

namespace {
// A row's identity across layouts: its item; for a part's "Worked for" row,
// minus (its lead + 1); for the live row, that less 2^40.
qint64 row_key(const transcript::Layout& layout, const transcript::Row& row) {
    if (row.item >= 0)
        return row.item;
    const auto& turn = layout.turns[static_cast<size_t>(row.turn)];
    const qint64 key = -(turn.parts[static_cast<size_t>(row.part)].lead + 1);
    return row.live ? key - (qint64{1} << 40) : key;
}
// The last item a row's look depends on.
int row_depends_on(const transcript::Layout& layout, const transcript::Row& row) {
    return row.item >= 0 ? row.item : layout.turns[static_cast<size_t>(row.turn)].last;
}
bool same_row(const transcript::Layout& a, const transcript::Row& x, const transcript::Layout& b,
              const transcript::Row& y) {
    return row_key(a, x) == row_key(b, y) && x.in_work == y.in_work;
}
} // namespace

void TranscriptModel::relayout(int changed) {
    auto next = transcript::lay_out(reader_->items(), expanded_, live_);
    const auto old_size = layout_.rows.size();
    const auto new_size = next.rows.size();
    size_t prefix = 0;
    while (prefix < old_size && prefix < new_size &&
           same_row(layout_, layout_.rows[prefix], next, next.rows[prefix]))
        ++prefix;
    size_t suffix = 0;
    while (suffix < old_size - prefix && suffix < new_size - prefix &&
           same_row(layout_, layout_.rows[old_size - 1 - suffix], next,
                    next.rows[new_size - 1 - suffix]))
        ++suffix;
    // Rows that left go first (the old layout still answers for the rest),
    // then rows that arrived, so the view keeps every row that stayed.
    if (old_size - suffix > prefix) {
        beginRemoveRows({}, static_cast<int>(prefix), static_cast<int>(old_size - suffix - 1));
        layout_.rows.erase(layout_.rows.begin() + static_cast<qsizetype>(prefix),
                           layout_.rows.begin() + static_cast<qsizetype>(old_size - suffix));
        endRemoveRows();
    }
    if (new_size - suffix > prefix) {
        beginInsertRows({}, static_cast<int>(prefix), static_cast<int>(new_size - suffix - 1));
        layout_ = std::move(next);
        endInsertRows();
    } else {
        layout_ = std::move(next);
    }
    // Rows kept whose item, or whose turn's timing, changed are redrawn.
    int first = -1;
    int last = -1;
    for (size_t i = 0; i < layout_.rows.size(); ++i)
        if (row_depends_on(layout_, layout_.rows[i]) >= changed) {
            if (first < 0)
                first = static_cast<int>(i);
            last = static_cast<int>(i);
        }
    if (first >= 0)
        emit dataChanged(index(first), index(last));
    if (old_size != new_size)
        emit countChanged();
    showLive(changed);
}

void TranscriptModel::showLive(int changed) {
    const bool running = !layout_.turns.empty() && layout_.turns.back().live;
    if (!running)
        live_steps_.clear();
    else
        live_steps_.show(transcript::live_items(reader_->items(), layout_,
                                                static_cast<int>(layout_.turns.size()) - 1),
                         changed);
    live_steps_.setActive(running);
}

void TranscriptModel::setLive(bool live) {
    if (live == live_)
        return;
    live_ = live;
    relayout(std::numeric_limits<int>::max());
    emit liveChanged();
}

void TranscriptModel::noteSent(const QString& text) {
    if (format() == transcript::Format::claude || path_.isEmpty())
        return;
    reader_->note_sent(text);
    notePending();
}

void TranscriptModel::notePending() {
    const auto pending = reader_->sent();
    if (pending == shown_pending_)
        return;
    shown_pending_ = pending;
    emit pendingChanged();
}

void TranscriptModel::toggleWork(int row) {
    if (row < 0 || std::cmp_greater_equal(row, layout_.rows.size()))
        return;
    const auto& shown = layout_.rows[static_cast<size_t>(row)];
    if (shown.item >= 0 || shown.live)
        return;
    const int lead =
        layout_.turns[static_cast<size_t>(shown.turn)].parts[static_cast<size_t>(shown.part)].lead;
    if (!expanded_.remove(lead))
        expanded_.insert(lead);
    relayout(std::numeric_limits<int>::max());
    emit dataChanged(index(row), index(row), {ExpandedRole});
}

QStringList TranscriptModel::pastedFiles() const {
    const auto* clipboard = QGuiApplication::clipboard();
    const auto* mime = clipboard != nullptr ? clipboard->mimeData() : nullptr;
    if (mime == nullptr)
        return {};
    if (mime->hasUrls()) {
        QStringList files;
        for (const auto& url : mime->urls())
            if (url.isLocalFile())
                files << url.toLocalFile();
        if (!files.isEmpty())
            return files;
    }
    if (!mime->hasImage())
        return {};
    const auto image = qvariant_cast<QImage>(mime->imageData());
    if (image.isNull())
        return {};
    // Pasted images live with the system's temporary files, which macOS
    // clears on its own; the agent reads them right after sending.
    QDir folder(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    if (!folder.mkpath(QStringLiteral("lapis-attachments")) ||
        !folder.cd(QStringLiteral("lapis-attachments")))
        return {};
    const auto path = folder.filePath(
        QStringLiteral("pasted-%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
    return image.save(path, "PNG") ? QStringList{path} : QStringList{};
}

bool TranscriptModel::isImage(const QString& path) const {
    // What the Codex app sends as an image rather than a file.
    static const QStringList images{
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"),  QStringLiteral("webp"), QStringLiteral("heic"),
        QStringLiteral("heif"), QStringLiteral("avif"), QStringLiteral("bmp"),
        QStringLiteral("tiff"), QStringLiteral("tif")};
    return images.contains(QFileInfo(path).suffix().toLower());
}

QString TranscriptModel::localPath(const QUrl& url) const {
    return url.isLocalFile() ? url.toLocalFile() : QString();
}

namespace {
// A media file as the view takes it: {path, url, kind, name, type, width,
// height, alpha, size}.
QVariantMap media_map(const transcript::Media& item) {
    const QFileInfo info(item.path);
    // A picture's own size, read from its header (an SVG's from its width
    // and height), so the view can lay it out before decoding; and whether
    // it can be see-through, so it gets no frame drawn round it.
    QSize pixels;
    bool alpha = false;
    if (item.kind == QLatin1String("image") || item.kind == QLatin1String("svg") ||
        item.kind == QLatin1String("animation")) {
        QImageReader reader(item.path);
        pixels = reader.size();
        alpha = item.kind == QLatin1String("svg") ||
                QImage::toPixelFormat(reader.imageFormat()).alphaUsage() == QPixelFormat::UsesAlpha;
    }
    return QVariantMap{{QStringLiteral("path"), item.path},
                       {QStringLiteral("url"), QUrl::fromLocalFile(item.path)},
                       {QStringLiteral("kind"), item.kind},
                       {QStringLiteral("name"), info.fileName()},
                       {QStringLiteral("type"), info.suffix().toUpper()},
                       {QStringLiteral("width"), pixels.width()},
                       {QStringLiteral("height"), pixels.height()},
                       {QStringLiteral("alpha"), alpha},
                       {QStringLiteral("size"), info.size()}};
}
} // namespace

const TranscriptModel::ReplyParts& TranscriptModel::reply_parts(const QString& markdown) const {
    if (const auto kept = replies_.constFind(markdown); kept != replies_.cend())
        return *kept;
    bool missing = false;
    ReplyParts parts;
    QSet<QString> shown;
    for (const auto& piece : transcript::pieces(markdown, &missing)) {
        const bool media = !piece.media.path.isEmpty();
        if (media)
            shown.insert(piece.media.path);
        parts.pieces.append(
            QVariantMap{{QStringLiteral("code"), piece.code},
                        {QStringLiteral("language"), piece.language},
                        {QStringLiteral("text"), piece.text},
                        {QStringLiteral("media"),
                         media ? QVariant(media_map(piece.media)) : QVariant::fromValue(nullptr)}});
    }
    for (const auto& item : transcript::find_media(markdown, shown, &missing))
        parts.media.append(media_map(item));
    if (missing) {
        unsettled_ = std::move(parts);
        return unsettled_;
    }
    if (replies_.size() >= kept_replies)
        replies_.clear();
    return *replies_.insert(markdown, std::move(parts));
}

QVariantList TranscriptModel::pieces(const QString& markdown) const {
    return reply_parts(markdown).pieces;
}

QVariantList TranscriptModel::replyMedia(const QString& markdown) const {
    return reply_parts(markdown).media;
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
    const auto inline_code_color = style.value(QStringLiteral("inlineCodeColor")).value<QColor>();
    const auto inline_code_text =
        style.value(QStringLiteral("inlineCodeTextColor")).value<QColor>();
    const auto link_color = style.value(QStringLiteral("linkColor")).value<QColor>();
    QFont base(style.value(QStringLiteral("sans")).toString());
    base.setPixelSize(size);
    document.setDefaultFont(base);
    document.setDocumentMargin(0);
    // Images show under the reply at the column's width, not inside the
    // text at their pixel size: an image becomes a link to itself here.
    static const QRegularExpression image(QStringLiteral(R"(!(\[[^\]]*\]\())"));
    // Math is taken out before the import, so markdown never reads LaTeX's
    // underscores, asterisks and backslashes, and set back in after it.
    const auto prepared =
        transcript::math::prepare(QString(markdown).replace(image, QStringLiteral("\\1")));
    document.setMarkdown(prepared.markdown, QTextDocument::MarkdownDialectGitHub);
    transcript::math::fill(document, prepared.spans);

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
        const bool equation = block_format.hasProperty(transcript::math::display_property);
        block_format.setTopMargin(first ? 0 : level > 0 ? size * 0.8 : equation ? size * 0.15 : 0);
        // The last block adds nothing: the view spaces pieces itself.
        block_format.setBottomMargin(!block.next().isValid()       ? 0
                                     : level > 0                   ? size * 0.3
                                     : block.textList() != nullptr ? size * 0.25
                                                                   : size * 0.65);
        first = false;
        // A code block the reply's pieces did not split out (indented, or
        // fenced inside a quote) keeps the code well; code inside a sentence
        // takes the quieter inline colors.
        const bool code_block = block_format.hasProperty(QTextFormat::BlockCodeFence) ||
                                block_format.hasProperty(QTextFormat::BlockCodeLanguage);
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
                const auto& background = code_block ? code_color : inline_code_color;
                if (background.isValid())
                    format.setBackground(background);
                if (!code_block && inline_code_text.isValid())
                    format.setForeground(inline_code_text); // a link's color wins below
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

    // Box drawing and block characters in the code font, where they are made
    // to join: "┌─┐" and bar charts in prose read as lines, not dashes.
    static const QRegularExpression boxes(QStringLiteral("[\\x{2500}-\\x{259F}]+"));
    for (auto found = document.find(boxes); !found.isNull(); found = document.find(boxes, found)) {
        QTextCharFormat format;
        format.setFontFamilies({mono});
        found.mergeCharFormat(format);
    }

    // Tables: a thin grid in the theme's quiet border color, roomy cells and
    // a bolder header. (Qt Quick draws only a table's whole grid, not rules
    // per cell or full cell backgrounds, so a rows-only look is not
    // available here.)
    const auto border = style.value(QStringLiteral("borderColor")).value<QColor>();
    for (auto* frame : document.rootFrame()->childFrames()) {
        auto* table = qobject_cast<QTextTable*>(frame);
        if (table == nullptr)
            continue;
        auto format = table->format();
        format.setBorderCollapse(true);
        format.setBorder(1);
        format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        if (border.isValid())
            format.setBorderBrush(border);
        format.setCellSpacing(0);
        format.setCellPadding(size * 0.5);
        format.setTopMargin(size * 0.3);
        format.setBottomMargin(size * 0.3);
        table->setFormat(format);
        for (int column = 0; column < table->columns(); ++column) {
            QTextCursor header_cell = table->cellAt(0, column).firstCursorPosition();
            header_cell.setPosition(table->cellAt(0, column).lastPosition(),
                                    QTextCursor::KeepAnchor);
            QTextCharFormat bold;
            bold.setFontWeight(QFont::DemiBold);
            header_cell.mergeCharFormat(bold);
        }
    }

    // A quote reads quieter than the reply around it.
    const auto quote = style.value(QStringLiteral("quoteColor")).value<QColor>();
    if (quote.isValid())
        for (auto block = document.begin(); block.isValid(); block = block.next())
            if (block.blockFormat().hasProperty(QTextFormat::BlockQuoteLevel) &&
                block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0) {
                QTextCursor quoted(block);
                quoted.select(QTextCursor::BlockUnderCursor);
                QTextCharFormat format;
                format.setForeground(quote);
                quoted.mergeCharFormat(format);
            }
    cursor.endEditBlock();
}

QVariantList TranscriptModel::slashCommands(const QString& harness, const QString& folder) const {
    return slash_commands(harness, QDir::homePath(), folder);
}

QVariantList TranscriptModel::media(const QString& text) const {
    QVariantList out;
    for (const auto& item : transcript::find_media(text))
        out.append(media_map(item));
    return out;
}

void TranscriptModel::writeChartStyle(const QVariantMap& colors) const {
    const auto hex = [&colors](const char* key) {
        return colors.value(QLatin1String(key)).value<QColor>().name(QColor::HexRgb).mid(1);
    };
    const bool light = colors.value(QStringLiteral("light")).toBool();
    // Tokyo Night's accents in the order a plot uses them.
    const QStringList series =
        light ? QStringList{QStringLiteral("2e7de9"), QStringLiteral("587539"),
                            QStringLiteral("b15c00"), QStringLiteral("9854f1"),
                            QStringLiteral("007197"), QStringLiteral("f52a65"),
                            QStringLiteral("8c6c3e"), QStringLiteral("387068")}
              : QStringList{QStringLiteral("7aa2f7"), QStringLiteral("9ece6a"),
                            QStringLiteral("ff9e64"), QStringLiteral("bb9af7"),
                            QStringLiteral("7dcfff"), QStringLiteral("f7768e"),
                            QStringLiteral("e0af68"), QStringLiteral("73daca")};
    const auto background = hex("background");
    const auto text = hex("text");
    const auto muted = hex("muted");
    const auto grid = hex("grid");
    QStringList quoted;
    for (const auto& color : series)
        quoted << QStringLiteral("'%1'").arg(color);
    const auto rc =
        QStringLiteral(
            "# Written by lapis from the visual view's theme; it is rewritten when the theme "
            "changes.\n"
            "figure.facecolor: %1\naxes.facecolor: %1\nsavefig.facecolor: %1\nsavefig.edgecolor: "
            "%1\n"
            "text.color: %2\naxes.labelcolor: %2\naxes.titlecolor: %2\nxtick.color: %3\n"
            "ytick.color: %3\nxtick.labelcolor: %2\nytick.labelcolor: %2\naxes.edgecolor: %4\n"
            "axes.grid: True\ngrid.color: %4\ngrid.linewidth: 0.6\naxes.spines.top: False\n"
            "axes.spines.right: False\naxes.prop_cycle: cycler('color', [%5])\n"
            "legend.facecolor: %1\nlegend.edgecolor: %4\nlegend.labelcolor: %2\n"
            "legend.frameon: False\nfont.family: sans-serif\n"
            "font.sans-serif: Helvetica Neue, Helvetica, Arial, DejaVu Sans\n"
            "savefig.dpi: 200\nsavefig.bbox: tight\nimage.cmap: viridis\n")
            .arg(background, text, muted, grid, quoted.join(QStringLiteral(", ")));
    QJsonArray palette;
    for (const auto& color : series)
        palette.append(QLatin1Char('#') + color);
    const auto json =
        QJsonDocument(QJsonObject{{QStringLiteral("background"), QLatin1Char('#') + background},
                                  {QStringLiteral("text"), QLatin1Char('#') + text},
                                  {QStringLiteral("muted"), QLatin1Char('#') + muted},
                                  {QStringLiteral("grid"), QLatin1Char('#') + grid},
                                  {QStringLiteral("light"), light},
                                  {QStringLiteral("series"), palette}})
            .toJson();
    const QDir folder(transcript::chart_style_folder());
    if (!folder.exists() && !QDir().mkpath(folder.path()))
        return;
    const auto write = [](const QString& path, const QByteArray& bytes) {
        QFile existing(path);
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes)
            return;
        existing.close();
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(bytes);
    };
    write(folder.filePath(QStringLiteral("matplotlibrc")), rc.toUtf8());
    write(folder.filePath(QStringLiteral("palette.json")), json);
}

void TranscriptModel::highlight(QQuickTextDocument* target, const QString& language,
                                bool light) const {
    if (target == nullptr || target->textDocument() == nullptr)
        return;
    auto* document = target->textDocument();
    // One highlighter per document; a theme or language change replaces it.
    for (auto* old : document->findChildren<code::Highlighter*>(Qt::FindDirectChildrenOnly))
        delete old;
    new code::Highlighter(document, language, light);
}

void TranscriptModel::hoverLink(QQuickTextDocument* target, const QString& link) const {
    if (target == nullptr || target->textDocument() == nullptr)
        return;
    QTextDocument& document = *target->textDocument();
    struct Change {
        int position;
        int length;
        bool underline;
    };
    std::vector<Change> changes;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const auto part = fragment.fragment();
            if (!part.isValid() || !part.charFormat().isAnchor())
                continue;
            const bool want = !link.isEmpty() && part.charFormat().anchorHref() == link;
            if (part.charFormat().fontUnderline() != want)
                changes.push_back({part.position(), part.length(), want});
        }
    QTextCursor cursor(&document);
    for (const auto& change : changes) {
        cursor.setPosition(change.position);
        cursor.setPosition(change.position + change.length, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setFontUnderline(change.underline);
        cursor.mergeCharFormat(format);
    }
}

QString TranscriptModel::pathAt(QQuickTextDocument* source, int position,
                                const QString& folder) const {
    if (source == nullptr || source->textDocument() == nullptr)
        return {};
    return transcript::path_at(source->textDocument()->toPlainText(), position, folder);
}

namespace {
struct Editor {
    const char* id;
    const char* label;
    const char* app;     // the bundle, in /Applications or ~/Applications
    const char* command; // its command line tool inside the bundle, if any
    bool lines;          // the tool opens "file:line:column"
};
constexpr Editor kEditors[] = {
    {"vscode", "VS Code", "Visual Studio Code.app", "Contents/Resources/app/bin/code", true},
    {"cursor", "Cursor", "Cursor.app", "Contents/Resources/app/bin/cursor", true},
    {"zed", "Zed", "Zed.app", "Contents/MacOS/cli", true},
    {"textedit", "TextEdit", "TextEdit.app", nullptr, false},
};
QString installed(const Editor& editor) {
    for (const auto& base :
         {QStringLiteral("/Applications"), QDir::home().filePath(QStringLiteral("Applications")),
          QStringLiteral("/System/Applications")}) {
        const auto bundle = QDir(base).filePath(QString::fromUtf8(editor.app));
        if (QFileInfo(bundle).isDir())
            return bundle;
    }
    return {};
}
} // namespace

QVariantList TranscriptModel::editors() const {
    QVariantList out;
    for (const auto& editor : kEditors)
        if (!installed(editor).isEmpty())
            out.append(QVariantMap{{QStringLiteral("id"), QString::fromUtf8(editor.id)},
                                   {QStringLiteral("label"), QString::fromUtf8(editor.label)}});
    out.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("system")},
                           {QStringLiteral("label"), tr("Default app")}});
    return out;
}

bool TranscriptModel::openPath(const QString& target, const QString& choice) const {
    static const QRegularExpression line(QStringLiteral(R"((:\d+){1,2}$)"));
    const auto suffix = line.match(target).captured(0);
    auto path = target.left(target.size() - suffix.size());
    if (path.startsWith(QLatin1String("file://")))
        path = QUrl(path).toLocalFile();
    else if (path.startsWith(QLatin1String("~/")))
        path = QDir::home().filePath(path.mid(2));
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    // A folder is shown in the Finder.
    if (info.isDir())
        return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    for (const auto& editor : kEditors) {
        if (choice != QLatin1String("auto") && choice != QLatin1String(editor.id))
            continue;
        const auto bundle = installed(editor);
        if (bundle.isEmpty())
            continue;
        if (editor.command != nullptr) {
            const auto tool = QDir(bundle).filePath(QString::fromUtf8(editor.command));
            if (QFileInfo(tool).isExecutable()) {
                QStringList arguments;
                if (editor.lines && !suffix.isEmpty() &&
                    QLatin1String(editor.id) != QLatin1String("zed"))
                    arguments << QStringLiteral("-g");
                arguments << path + (editor.lines ? suffix : QString());
                if (QProcess::startDetached(tool, arguments))
                    return true;
            }
        }
        return QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                       {QStringLiteral("-a"), bundle, path});
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

double TranscriptModel::runningSince() const {
    if (layout_.turns.empty() || layout_.turns.back().closed)
        return 0;
    return static_cast<double>(
        reader_->items()[static_cast<size_t>(layout_.turns.back().start)].at_ms);
}

QStringList TranscriptModel::liveLines(const QStringList& screen) const {
    // Cut at Claude Code's input box; another CLI's screen would stream its
    // own prompt and footer, so it shows no tail rather than a wrong one.
    if (!screenKnown())
        return {};
    // The latest replies the thread shows, so a finished one still on the
    // screen is not repeated under "Working for".
    QStringList shown;
    const auto& items = reader_->items();
    for (auto item = items.rbegin(); item != items.rend() && shown.size() < 3; ++item)
        if (item->kind == transcript::Kind::reply && !item->body.isEmpty())
            shown << item->body.left(400);
    return transcript::live_lines(screen, shown);
}

bool TranscriptModel::compactingShown(const QStringList& screen) const {
    return screenKnown() && transcript::compacting_shown(screen);
}

bool TranscriptModel::dialogShowing(const QStringList& screen) const {
    return screenKnown() && transcript::dialog_showing(screen);
}

QVariantMap TranscriptModel::fonts() const {
    // New York and SF Mono ship with macOS but are hidden from the font list;
    // loading the files gives them names the view can ask for.
    static const QVariantMap loaded = [] {
        const auto family = [](const char* file, const char* fallback) {
            const auto id = QFontDatabase::addApplicationFont(QString::fromUtf8(file));
            const auto families =
                id < 0 ? QStringList{} : QFontDatabase::applicationFontFamilies(id);
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

namespace {
// The text of a block in [from, to), with each math span the range covers
// whole given back as written ($x^2$), so a copy pastes as its source; part
// of a span copies as shown.
QString selected_text(const QTextBlock& block, int from, int to) {
    namespace math = transcript::math;
    if (to <= from)
        return {};
    // Where each span starts and ends in this block.
    QHash<int, std::pair<int, int>> spans;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto part = it.fragment();
        if (!part.isValid() || !part.charFormat().hasProperty(math::span_property))
            continue;
        const int id = part.charFormat().intProperty(math::span_property);
        const int part_end = part.position() + part.length();
        auto found = spans.find(id);
        if (found == spans.end())
            spans.insert(id, {part.position(), part_end});
        else
            found->second = part_end;
    }
    if (spans.isEmpty())
        return block.text().mid(from - block.position(), to - from);
    QString out;
    int copied_span = -1;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto part = it.fragment();
        if (!part.isValid())
            continue;
        const int part_start = part.position();
        const int part_end = part_start + part.length();
        const int a = std::max(from, part_start);
        const int b = std::min(to, part_end);
        if (a >= b)
            continue;
        const auto format = part.charFormat();
        if (format.hasProperty(math::span_property)) {
            const int id = format.intProperty(math::span_property);
            const auto extent = spans.value(id);
            if (from <= extent.first && to >= extent.second) {
                if (id != copied_span)
                    out += format.stringProperty(math::source_property);
                copied_span = id;
                continue;
            }
        }
        out += part.text().mid(a - part_start, b - a);
    }
    return out;
}
} // namespace

QString transcript::selection_text(const QTextDocument& document, int start, int end) {
    QString out;
    const QTextList* previous_list = nullptr;
    for (auto block = document.findBlock(start); block.isValid() && block.position() < end;
         block = block.next()) {
        const int from = std::max(start, block.position());
        const int to = std::min(end, block.position() + block.length() - 1);
        const auto text = selected_text(block, from, to);
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
    const auto generation = ++generation_;
    const bool was_loading = loading_;
    loading_ = false;
    // The reader is swapped inside the reset, so the view never sees the old
    // rows over the new reader.
    beginResetModel();
    // Keep what was read (a load still running has nothing whole to keep).
    if (!path_.isEmpty() && !was_loading) {
        std::erase_if(kept_, [this](const Parsed& kept) { return kept.path == path_; });
        kept_.insert(kept_.begin(),
                     Parsed{path_, std::move(reader_), offset_, std::move(pending_)});
        if (kept_.size() > kept_conversations)
            kept_.pop_back();
    }
    path_ = path;
    const auto format = format_for(path);
    const auto hit = std::find_if(kept_.begin(), kept_.end(), [&path, format](const Parsed& kept) {
        return kept.path == path && kept.reader->format() == format;
    });
    if (hit != kept_.end() && !path.isEmpty()) {
        reader_ = std::move(hit->reader);
        offset_ = hit->offset;
        pending_ = std::move(hit->pending);
        kept_.erase(hit);
    } else {
        reader_ = transcript::make_reader(format);
        offset_ = 0;
        pending_.clear();
    }
    expanded_.clear();
    layout_ = transcript::lay_out(reader_->items(), expanded_, live_);
    endResetModel();
    emit pathChanged();
    if (was_loading)
        emit loadingChanged();
    emit countChanged();
    noteStatus();
    showLive(0);
    notePending();
    const bool warm = !reader_->items().empty() || offset_ > 0;
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
    QThreadPool::globalInstance()->start([guard = QPointer<TranscriptModel>(this), path, format,
                                          generation] {
        auto result = read_whole(path, format);
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
    auto result = read_whole(path_, format_for(path_));
    apply(std::move(result.reader), result.offset, std::move(result.pending));
}

void TranscriptModel::apply(std::unique_ptr<transcript::Reader> reader, qint64 offset,
                            QByteArray pending) {
    const bool format_changed = reader->format() != reader_->format();
    beginResetModel();
    reader_ = std::move(reader);
    layout_ = transcript::lay_out(reader_->items(), expanded_, live_);
    offset_ = offset;
    pending_ = std::move(pending);
    endResetModel();
    if (format_changed)
        emit pathChanged(); // the format and what depends on it
    emit countChanged();
    noteStatus();
    showLive(0);
    notePending();
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
    const auto changed = feed(*reader_, pending_, bytes);
    noteStatus();
    if (changed >= 0)
        relayout(changed);
    notePending();
}

void TranscriptModel::noteStatus() {
    if (reader_->status() == shown_status_)
        return;
    shown_status_ = reader_->status();
    emit statusChanged();
}
} // namespace lapis::desktop
