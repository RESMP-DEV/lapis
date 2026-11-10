// Codex rollouts as the visual view's items. The record types and fields are
// those Codex 0.157 writes for its TUI; see CodexReader in transcript.hpp.
#include "transcript.hpp"
#include "transcript_parts.hpp"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace lapis::desktop::transcript {
using namespace detail;

namespace {
QString text_of(const QJsonObject& object, const char* key) {
    return object.value(QLatin1String(key)).toString();
}

qint64 number_of(const QJsonObject& object, const char* key) {
    return static_cast<qint64>(object.value(QLatin1String(key)).toDouble());
}

// The texts of a content list ({type, text}), joined by a blank line.
QString texts(const QJsonArray& content) {
    QStringList parts;
    for (const auto& value : content) {
        const auto part = value.toObject();
        if (const auto text = text_of(part, "text"); !text.trimmed().isEmpty())
            parts << text.trimmed();
    }
    return parts.join(QStringLiteral("\n\n"));
}

// Summary and raw reasoning are lists of strings or of {text} parts.
QString reasoning_text(const QJsonArray& parts) {
    QStringList out;
    for (const auto& value : parts) {
        const auto text = value.isString() ? value.toString() : text_of(value.toObject(), "text");
        if (!text.trimmed().isEmpty())
            out << text.trimmed();
    }
    return out.join(QStringLiteral("\n\n"));
}

// A path a command or a patch names, absolute (against the command's folder)
// and written from ~.
QString shown_path(const QString& path, const QString& folder) {
    if (path.isEmpty())
        return {};
    const auto absolute =
        QDir::isAbsolutePath(path) || folder.isEmpty() ? path : QDir(folder).filePath(path);
    return homeless(QDir::cleanPath(absolute));
}

// A unified diff's lines as the step's diff: added and removed lines
// counted, context kept for the preview, headers and hunk markers left out.
void add_unified_diff(Item& item, const QString& diff) {
    for (const auto& line : diff.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("@@")) || line.startsWith(QLatin1String("+++")) ||
            line.startsWith(QLatin1String("---")) || line.startsWith(QLatin1Char('\\')))
            continue;
        if (line.startsWith(QLatin1Char('+')) || line.startsWith(QLatin1Char('-'))) {
            add_lines(item, line.front(), line.mid(1));
        } else if (line.startsWith(QLatin1Char(' ')) &&
                   std::cmp_less(item.diff.size(), diff_preview_lines)) {
            item.diff.push_back({QLatin1Char(' '), clip(line.mid(1), 400)});
        }
    }
}

// How long an item took: its own duration ({secs, nanos}), else its times.
qint64 took_ms(const QJsonObject& item, qint64 started_ms, qint64 completed_ms) {
    const auto duration = item.value(QLatin1String("duration")).toObject();
    if (!duration.isEmpty())
        return number_of(duration, "secs") * 1000 + number_of(duration, "nanos") / 1'000'000;
    if (const auto ms = number_of(item, "durationMs"); ms > 0)
        return ms;
    return started_ms > 0 && completed_ms > started_ms ? completed_ms - started_ms : 0;
}

// A command as the step a person would name: one read, search or listing
// by what it touched (Codex's own reading of the command), else "Ran".
Item command_item(const QJsonObject& item) {
    Item step;
    step.kind = Kind::tool;
    const auto argv = item.value(QLatin1String("command")).toArray();
    QString command;
    if (argv.size() >= 3 && (argv.at(1).toString() == QLatin1String("-lc") ||
                             argv.at(1).toString() == QLatin1String("-c")))
        command = argv.at(2).toString();
    else {
        QStringList words;
        for (const auto& word : argv)
            words << word.toString();
        command = words.join(QLatin1Char(' '));
    }
    if (item.value(QLatin1String("command")).isString())
        command = text_of(item, "command");
    const auto folder = text_of(item, "cwd");
    const auto parsed = item.value(QLatin1String("parsed_cmd")).toArray();
    const auto only = parsed.size() == 1 ? parsed.first().toObject() : QJsonObject{};
    const auto kind = text_of(only, "type");
    if (kind == QLatin1String("read") && !text_of(only, "path").isEmpty()) {
        step.title = QStringLiteral("Read");
        step.body = shown_path(text_of(only, "path"), folder);
    } else if (kind == QLatin1String("search") && !text_of(only, "query").isEmpty()) {
        step.title = QStringLiteral("Searched for");
        step.body = text_of(only, "query");
        step.detail = shown_path(text_of(only, "path"), folder);
    } else if (kind == QLatin1String("list_files")) {
        step.title = QStringLiteral("Listed");
        step.body = shown_path(text_of(only, "path"), folder);
        if (step.body.isEmpty())
            step.body = homeless(folder);
    } else {
        step.title = QStringLiteral("Ran");
        step.body = clip(command, 2000);
    }
    auto output = text_of(item, "aggregated_output");
    if (output.isEmpty())
        output = text_of(item, "formatted_output");
    if (output.isEmpty())
        output = (text_of(item, "stdout") + QLatin1Char('\n') + text_of(item, "stderr"));
    set_result(step, output);
    const auto status = text_of(item, "status");
    const auto exit = item.value(QLatin1String("exit_code"));
    step.failed = status == QLatin1String("failed") || status == QLatin1String("declined") ||
                  (exit.isDouble() && exit.toInt() != 0);
    step.answered = status != QLatin1String("in_progress");
    return step;
}

// The output of an MCP tool: its text parts, an image as "[image]".
QString tool_output(const QJsonObject& item) {
    const auto result = item.value(QLatin1String("result")).toObject();
    QStringList parts;
    for (const auto& value : result.value(QLatin1String("content")).toArray()) {
        const auto part = value.toObject();
        const auto type = text_of(part, "type");
        if (type == QLatin1String("text"))
            parts << text_of(part, "text");
        else
            parts << QStringLiteral("[%1]").arg(type);
    }
    const auto error = item.value(QLatin1String("error"));
    if (error.isString())
        parts << error.toString();
    else if (error.isObject())
        parts << text_of(error.toObject(), "message");
    return parts.join(QLatin1Char('\n'));
}

// Web searches and the like, which Codex records as extensions by kind.
Item extension_item(const QJsonObject& item) {
    const auto kind = text_of(item, "kind");
    if (kind == QLatin1String("clock.sleep")) {
        auto step = make(Kind::tool, QStringLiteral("Waited"),
                         duration_text(number_of(item, "durationMs")));
        step.answered = true;
        return step;
    }
    if (kind == QLatin1String("web.search")) {
        const auto action = item.value(QLatin1String("action")).toObject();
        QStringList queries;
        for (const auto& query : action.value(QLatin1String("queries")).toArray())
            queries << query.toString();
        auto target = !text_of(action, "query").isEmpty() ? text_of(action, "query")
                      : !queries.isEmpty()                ? queries.join(QStringLiteral(", "))
                                                          : text_of(item, "query");
        const bool page = text_of(action, "type") == QLatin1String("openPage");
        if (page && !text_of(action, "url").isEmpty())
            target = text_of(action, "url");
        auto step = make(Kind::tool,
                         page ? QStringLiteral("Fetched") : QStringLiteral("Searched the web for"),
                         target);
        QStringList found;
        for (const auto& value : item.value(QLatin1String("results")).toArray()) {
            const auto result = value.toObject();
            found << QStringLiteral("%1  %2").arg(text_of(result, "title"), text_of(result, "url"));
        }
        set_result(step, found.join(QLatin1Char('\n')));
        step.answered = true;
        return step;
    }
    auto step = make(Kind::tool, QStringLiteral("Called"), kind, compact_json(item, 600));
    step.answered = true;
    return step;
}
} // namespace

void CodexReader::push(Item item) {
    item.record = record_;
    if (item.at_ms == 0)
        item.at_ms = at_ms_;
    item.after_step = is_step(item.kind) && !items_.empty() && is_step(items_.back().kind);
    items_.push_back(std::move(item));
}

void CodexReader::item(const QJsonObject& item, qint64 started_ms, qint64 completed_ms) {
    const auto type = text_of(item, "type");
    record_ = text_of(item, "id");
    const auto timed = [started_ms](Item step) {
        step.at_ms = started_ms;
        return step;
    };
    if (type == QLatin1String("UserMessage")) {
        QStringList words;
        QStringList images; // local image paths, shown under the prompt
        int pictures = 0;   // images sent inline, with no path
        for (const auto& value : item.value(QLatin1String("content")).toArray()) {
            const auto part = value.toObject();
            const auto kind = text_of(part, "type");
            if (kind == QLatin1String("text"))
                words << text_of(part, "text");
            else if (!text_of(part, "path").isEmpty())
                images << text_of(part, "path");
            else if (kind.contains(QLatin1String("image"), Qt::CaseInsensitive))
                ++pictures;
        }
        const auto text = words.join(QLatin1Char('\n')).trimmed();
        // A message sent while the turn runs steers it: it stays in view
        // inside the turn rather than starting one.
        const bool starts = turn_.isEmpty() || !turn_prompted_;
        turn_prompted_ = true;
        const auto prompt = [this, starts](QString body) {
            auto entry = make(Kind::prompt, {}, std::move(body));
            entry.starts_turn = starts;
            push(std::move(entry));
        };
        if (!text.isEmpty())
            prompt(clip(text, body_chars));
        for (const auto& path : images)
            prompt(QStringLiteral("[image] %1").arg(path));
        if (text.isEmpty() && images.isEmpty())
            for (int i = 0; i < pictures; ++i)
                prompt(QStringLiteral("[image]"));
    } else if (type == QLatin1String("AgentMessage")) {
        if (const auto text = texts(item.value(QLatin1String("content")).toArray());
            !text.isEmpty())
            push(timed(make(Kind::reply, {}, clip(text, body_chars))));
    } else if (type == QLatin1String("Reasoning")) {
        auto text = reasoning_text(item.value(QLatin1String("summary_text")).toArray());
        if (text.isEmpty())
            text = reasoning_text(item.value(QLatin1String("raw_content")).toArray());
        // Codex records reasoning without a summary when summaries are off;
        // an empty "Thought" row would say nothing.
        if (text.isEmpty())
            return;
        const auto ms = took_ms(item, started_ms, completed_ms);
        push(timed(make(Kind::thinking,
                        ms >= 1000 ? QStringLiteral("Thought for %1").arg(duration_text(ms))
                                   : QStringLiteral("Thought"),
                        clip(text, body_chars))));
    } else if (type == QLatin1String("CommandExecution")) {
        auto step = timed(command_item(item));
        step.duration_ms = took_ms(item, started_ms, completed_ms);
        push(std::move(step));
    } else if (type == QLatin1String("FileChange")) {
        const auto changes = item.value(QLatin1String("changes")).toObject();
        const auto status = text_of(item, "status");
        const bool failed = !status.isEmpty() && status != QLatin1String("completed");
        for (auto change = changes.begin(); change != changes.end(); ++change) {
            const auto what = change.value().toObject();
            const auto kind = text_of(what, "type");
            auto step = timed(make(Kind::tool,
                                   kind == QLatin1String("add")      ? QStringLiteral("Wrote")
                                   : kind == QLatin1String("delete") ? QStringLiteral("Deleted")
                                                                     : QStringLiteral("Edited"),
                                   homeless(change.key())));
            if (const auto moved = text_of(what, "move_path"); !moved.isEmpty())
                step.detail = QStringLiteral("to %1").arg(homeless(moved));
            add_unified_diff(step, text_of(what, "unified_diff"));
            if (step.diff.empty() && kind == QLatin1String("add"))
                add_lines(step, QLatin1Char('+'), text_of(what, "content"));
            step.answered = true;
            step.failed = failed;
            if (failed)
                set_result(step, text_of(item, "stderr"));
            // One patch timed once, on its first file.
            if (change == changes.begin())
                step.duration_ms = took_ms(item, started_ms, completed_ms);
            push(std::move(step));
        }
    } else if (type == QLatin1String("McpToolCall")) {
        const auto arguments = item.value(QLatin1String("arguments")).toObject();
        const auto tool = QString(text_of(item, "tool")).replace(QLatin1Char('_'), QLatin1Char(' '));
        const auto server =
            QString(text_of(item, "server")).replace(QLatin1Char('-'), QLatin1Char(' '));
        const auto title = text_of(arguments, "title");
        auto step = timed(make(Kind::tool, QStringLiteral("Called"), title.isEmpty() ? tool : title,
                               title.isEmpty() ? server
                                               : QStringLiteral("%1 · %2").arg(server, tool)));
        set_result(step, tool_output(item));
        const auto result = item.value(QLatin1String("result")).toObject();
        step.failed = text_of(item, "status") == QLatin1String("failed") ||
                      result.value(QLatin1String("isError")).toBool();
        step.answered = text_of(item, "status") != QLatin1String("in_progress");
        step.duration_ms = took_ms(item, started_ms, completed_ms);
        push(std::move(step));
    } else if (type == QLatin1String("Extension")) {
        auto step = timed(extension_item(item));
        if (step.duration_ms == 0 && text_of(item, "kind") != QLatin1String("clock.sleep"))
            step.duration_ms = took_ms(item, started_ms, completed_ms);
        push(std::move(step));
    } else if (type == QLatin1String("ImageView")) {
        auto step = timed(make(Kind::tool, QStringLiteral("Viewed image"),
                               homeless(text_of(item, "path"))));
        step.answered = true;
        push(std::move(step));
    } else if (type == QLatin1String("ContextCompaction")) {
        // The context is now the summary; the next token count sizes it.
        push(make(Kind::divider, QStringLiteral("Conversation compacted")));
    } else if (!type.isEmpty()) {
        // An item lapis has not learned yet still shows, as Claude's do.
        push(timed(make(Kind::other, type, {}, compact_json(item, 400))));
    }
}

int CodexReader::add(const QByteArray& line) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return -1;
    const auto record = document.object();
    const auto type = text_of(record, "type");
    const auto payload = record.value(QLatin1String("payload")).toObject();
    at_ms_ = time_ms(text_of(record, "timestamp"));
    const auto first = static_cast<int>(items_.size());

    if (type == QLatin1String("turn_context")) {
        if (const auto model = text_of(payload, "model"); !model.isEmpty())
            status_.model = model;
        if (const auto effort = text_of(payload, "effort"); !effort.isEmpty())
            status_.effort = effort;
        return -1;
    }
    if (type != QLatin1String("event_msg"))
        return -1; // response items repeat the work for the model; the rest is Codex's own
    const auto event = text_of(payload, "type");
    if (event == QLatin1String("task_started")) {
        turn_ = text_of(payload, "turn_id");
        if (turn_.isEmpty())
            turn_ = QStringLiteral("turn");
        turn_prompted_ = false;
        if (const auto window = number_of(payload, "model_context_window"); window > 0)
            status_.window_tokens = window;
    } else if (event == QLatin1String("thread_settings_applied")) {
        // /model and its effort take effect now, before the next turn says so.
        const auto settings = payload.value(QLatin1String("thread_settings")).toObject();
        if (const auto model = text_of(settings, "model"); !model.isEmpty())
            status_.model = model;
        if (const auto effort = text_of(settings, "reasoning_effort"); !effort.isEmpty())
            status_.effort = effort;
    } else if (event == QLatin1String("token_count")) {
        const auto info = payload.value(QLatin1String("info")).toObject();
        const auto last = info.value(QLatin1String("last_token_usage")).toObject();
        auto used = number_of(last, "total_tokens");
        if (used <= 0)
            used = number_of(last, "input_tokens") + number_of(last, "output_tokens");
        if (used > 0)
            status_.context_tokens = used;
        if (const auto window = number_of(info, "model_context_window"); window > 0)
            status_.window_tokens = window;
    } else if (event == QLatin1String("item_completed")) {
        item(payload.value(QLatin1String("item")).toObject(), number_of(payload, "started_at_ms"),
             number_of(payload, "completed_at_ms"));
    } else if (event == QLatin1String("task_complete") ||
               event == QLatin1String("turn_aborted")) {
        record_ = text_of(payload, "turn_id");
        if (event == QLatin1String("turn_aborted")) {
            const auto reason = text_of(payload, "reason");
            push(make(Kind::notice, reason.isEmpty() || reason == QLatin1String("interrupted")
                                        ? QStringLiteral("Interrupted")
                                        : QStringLiteral("Turn ended: %1").arg(reason)));
        }
        const auto ms = number_of(payload, "duration_ms");
        auto turn = make(Kind::turn, QStringLiteral("Worked for %1").arg(duration_text(ms)));
        turn.duration_ms = ms;
        push(std::move(turn));
        turn_.clear();
        turn_prompted_ = false;
    }
    return static_cast<int>(items_.size()) > first ? first : -1;
}
} // namespace lapis::desktop::transcript
