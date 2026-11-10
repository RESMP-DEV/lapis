// Antigravity CLI transcripts as the visual view's items. The step types and
// fields are those agy 1.5.5 writes; see AntigravityReader in transcript.hpp.
#include "transcript.hpp"
#include "transcript_parts.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace lapis::desktop::transcript {
using namespace detail;

namespace {
QString text_of(const QJsonObject& object, const char* key) {
    return object.value(QLatin1String(key)).toString();
}

// The text between <tag> and </tag>, or empty.
QString inside(const QString& text, QStringView tag) {
    const auto open = QStringLiteral("<%1>").arg(tag);
    const auto close = QStringLiteral("</%1>").arg(tag);
    const auto start = text.indexOf(open);
    if (start < 0)
        return {};
    const auto from = start + open.size();
    const auto end = text.indexOf(close, from);
    return text.mid(from, end < 0 ? -1 : end - from).trimmed();
}

// What the person typed: agy wraps it in <USER_REQUEST> beside the time and
// settings notes it adds for the model.
QString request_text(const QString& content) {
    if (const auto request = inside(content, u"USER_REQUEST"); !request.isEmpty())
        return request;
    static const QRegularExpression notes(
        QStringLiteral(R"(<([A-Z_]+)>.*?</\1>)"),
        QRegularExpression::DotMatchesEverythingOption);
    return QString(content).remove(notes).trimmed();
}

// "Gemini 4 Argon (Medium)" from agy's note that the model selection
// changed: the model as shown and its effort ("medium"; "Extra High" is
// "xhigh").
void read_selection(const QString& content, Status& status) {
    static const QRegularExpression change(
        QStringLiteral(R"(setting `Model Selection` from .*? to (.+?)\.(?:\s|$))"));
    const auto match = change.match(inside(content, u"USER_SETTINGS_CHANGE"));
    if (!match.hasMatch())
        return;
    auto chosen = match.captured(1).trimmed();
    static const QRegularExpression level(QStringLiteral(R"(^(.*?)\s*\(([^()]+)\)$)"));
    if (const auto split = level.match(chosen); split.hasMatch()) {
        chosen = split.captured(1);
        auto effort = split.captured(2).toLower();
        status.effort = effort == QLatin1String("extra high") ? QStringLiteral("xhigh") : effort;
    }
    status.model = chosen;
    // Gemini models take a million tokens; others go by the usual table.
    status.window_tokens =
        chosen.contains(QLatin1String("gemini"), Qt::CaseInsensitive) ? 1'048'576 : 0;
}

// A tool call as one step, in the same words as the other CLIs' steps.
Item tool_item(const QJsonObject& call) {
    const auto name = text_of(call, "name");
    auto args = call.value(QLatin1String("args")).toObject();
    const auto arg = [&args](const char* key) { return text_of(args, key); };
    const auto summary = arg("toolSummary");
    Item item;
    item.kind = Kind::tool;
    const auto step = [&item](const char* verb, QString target, QString detail = {}) {
        item.title = QString::fromUtf8(verb);
        item.body = std::move(target);
        item.detail = std::move(detail);
    };
    if (name == QLatin1String("run_command")) {
        step("Ran", clip(arg("CommandLine"), 2000), summary);
    } else if (name == QLatin1String("view_file")) {
        step("Read", homeless(arg("AbsolutePath")));
    } else if (name == QLatin1String("read_file")) {
        step("Read", homeless(arg("Path")));
    } else if (name == QLatin1String("list_dir")) {
        step("Listed", homeless(arg("DirectoryPath")));
    } else if (name == QLatin1String("find_by_name")) {
        step("Listed", arg("Pattern"), homeless(arg("SearchDirectory")));
    } else if (name == QLatin1String("grep_search")) {
        step("Searched for", arg("Query"),
             homeless(arg("SearchPath").isEmpty() ? arg("SearchDirectory") : arg("SearchPath")));
    } else if (name == QLatin1String("write_file")) {
        step("Wrote", homeless(arg("Path")));
        add_lines(item, QLatin1Char('+'), arg("Content"));
    } else if (name == QLatin1String("write_to_file")) {
        step("Wrote", homeless(arg("TargetFile")));
        add_lines(item, QLatin1Char('+'), arg("CodeContent"));
    } else if (name == QLatin1String("edit_file")) {
        step("Edited", homeless(arg("Path")));
        add_lines(item, QLatin1Char('-'), arg("Target"));
        add_lines(item, QLatin1Char('+'), arg("Replacement"));
    } else if (name == QLatin1String("replace_file_content") ||
               name == QLatin1String("multi_replace_file_content")) {
        step("Edited", homeless(arg("TargetFile")));
        add_lines(item, QLatin1Char('-'), arg("TargetContent"));
        add_lines(item, QLatin1Char('+'), arg("ReplacementContent"));
        for (const auto& chunk : args.value(QLatin1String("ReplacementChunks")).toArray()) {
            add_lines(item, QLatin1Char('-'), text_of(chunk.toObject(), "TargetContent"));
            add_lines(item, QLatin1Char('+'), text_of(chunk.toObject(), "ReplacementContent"));
        }
    } else if (name == QLatin1String("search_web")) {
        step("Searched the web for", arg("query"));
    } else if (name == QLatin1String("read_url_content")) {
        step("Fetched", arg("Url"));
    } else if (name == QLatin1String("invoke_subagent")) {
        step("Started agent", summary);
    } else if (name == QLatin1String("manage_subagents")) {
        step("Managed agents", summary.isEmpty() ? arg("Action") : summary);
    } else if (name == QLatin1String("send_message")) {
        step("Messaged", arg("Recipient"));
    } else if (name == QLatin1String("manage_task")) {
        step("Checked task", summary.isEmpty() ? arg("TaskId") : summary);
    } else if (name == QLatin1String("schedule")) {
        step("Scheduled a wakeup", summary);
    } else {
        args.remove(QStringLiteral("toolAction"));
        args.remove(QStringLiteral("toolSummary"));
        step("Called", summary.isEmpty() ? name : summary,
             summary.isEmpty() ? (args.isEmpty() ? QString{} : compact_json(args, 600)) : name);
    }
    return item;
}

// A tool's result: agy heads it with "Created At:"/"Completed At:" lines
// (local time with its offset), which time the step and are not output.
void attach(Item& step, const QJsonObject& record) {
    const auto content = text_of(record, "content");
    QStringList kept;
    qint64 created = 0, completed = 0;
    for (const auto& line : content.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("Created At: "))) {
            if (created == 0)
                created = time_ms(line.mid(12).trimmed());
            continue;
        }
        if (line.startsWith(QLatin1String("Completed At: "))) {
            completed = time_ms(line.mid(14).trimmed());
            continue;
        }
        kept << line;
    }
    if (created > 0 && completed >= created)
        step.duration_ms = completed - created;
    static const QRegularExpression exited(QStringLiteral(R"(exited with code (-?\d+))"));
    const auto code = exited.match(content);
    step.answered = true;
    step.failed = text_of(record, "status") == QLatin1String("ERROR") ||
                  !text_of(record, "error").isEmpty() ||
                  (code.hasMatch() && code.captured(1).toInt() != 0);
    // A change that worked says so in a sentence the diff already shows.
    if (!step.failed && !step.diff.empty())
        return;
    auto output = kept.join(QLatin1Char('\n'));
    if (const auto error = text_of(record, "error"); !error.isEmpty())
        output += QLatin1Char('\n') + error;
    set_result(step, output);
}
} // namespace

void AntigravityReader::push(Item item) {
    item.record = record_;
    item.at_ms = at_ms_;
    item.after_step = is_step(item.kind) && !items_.empty() && is_step(items_.back().kind);
    items_.push_back(std::move(item));
}

void AntigravityReader::wake(const QString& why) {
    auto item = make(Kind::notice, why);
    item.starts_turn = true;
    item.quiet = true; // the turn's "Worked for" line says enough
    push(std::move(item));
    turn_open_ = true;
    turn_since_ms_ = at_ms_;
}

int AntigravityReader::add(const QByteArray& line) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return -1;
    const auto record = document.object();
    const auto type = text_of(record, "type");
    if (const auto index = record.value(QLatin1String("step_index")); index.isDouble()) {
        const auto step = static_cast<qint64>(index.toDouble());
        if (steps_.contains(step))
            return -1;
        steps_.insert(step);
        record_ = QString::number(step);
    } else {
        record_.clear();
    }
    at_ms_ = time_ms(text_of(record, "created_at"));
    const auto first = static_cast<int>(items_.size());
    const auto content = text_of(record, "content");
    int changed = -1;
    const auto tokens = [&record] {
        const auto count = [&record](const char* key) {
            return static_cast<qint64>(record.value(QLatin1String(key)).toDouble());
        };
        return count("input_tokens") + count("cache_read_tokens") + count("output_tokens");
    };

    if (type == QLatin1String("USER_INPUT")) {
        read_selection(content, status_);
        waiting_.clear(); // results never written for calls before this
        turn_open_ = true;
        turn_since_ms_ = at_ms_;
        bool shown = false;
        if (const auto text = request_text(content); !text.isEmpty()) {
            auto prompt = make(Kind::prompt, {}, clip(text, body_chars));
            prompt.starts_turn = true;
            push(std::move(prompt));
            shown = true;
        }
        for (const auto& value : record.value(QLatin1String("media")).toArray()) {
            const auto media = value.toObject();
            auto path = text_of(media, "uri");
            if (path.startsWith(QLatin1String("file://")))
                path = QUrl(path).toLocalFile();
            const bool image = text_of(media, "mime_type").startsWith(QLatin1String("image/"));
            auto prompt = make(Kind::prompt, {},
                               QStringLiteral("%1 %2").arg(image ? QStringLiteral("[image]")
                                                                 : QStringLiteral("[file]"),
                                                           path));
            prompt.starts_turn = !shown;
            shown = true;
            push(std::move(prompt));
        }
    } else if (type == QLatin1String("PLANNER_RESPONSE")) {
        if (const auto used = tokens(); used > 0)
            status_.context_tokens = used;
        const auto thinking = text_of(record, "thinking").trimmed();
        const auto calls = record.value(QLatin1String("tool_calls")).toArray();
        const auto reply = content.trimmed();
        if (thinking.isEmpty() && reply.isEmpty() && calls.isEmpty())
            return -1;
        if (!turn_open_)
            wake(QStringLiteral("The agent continued"));
        if (!thinking.isEmpty())
            push(make(Kind::thinking, QStringLiteral("Thought"), clip(thinking, body_chars)));
        if (!reply.isEmpty())
            push(make(Kind::reply, {}, clip(reply, body_chars)));
        for (const auto& call : calls) {
            waiting_.push_back(static_cast<int>(items_.size()));
            push(tool_item(call.toObject()));
        }
        if (!reply.isEmpty() && calls.isEmpty()) {
            // Text with nothing left to do ends the turn.
            const auto ms = turn_since_ms_ > 0 && at_ms_ > turn_since_ms_
                                ? at_ms_ - turn_since_ms_
                                : 0;
            auto turn = make(Kind::turn, QStringLiteral("Worked for %1").arg(duration_text(ms)));
            turn.duration_ms = ms;
            push(std::move(turn));
            turn_open_ = false;
        }
    } else if (type == QLatin1String("GENERIC")) {
        // Results come in call order; one with no call waiting is the CLI's.
        if (waiting_.empty())
            return -1;
        const int at = waiting_.front();
        waiting_.erase(waiting_.begin());
        attach(items_[static_cast<size_t>(at)], record);
        changed = at;
    } else if (type == QLatin1String("SYSTEM_MESSAGE")) {
        // Notes for the model; one after the turn ended wakes the agent.
        if (!turn_open_) {
            auto why = content;
            why.remove(QStringLiteral("<SYSTEM_MESSAGE>")).remove(QStringLiteral("</SYSTEM_MESSAGE>"));
            wake(clip(first_line(why.trimmed()), 300));
        }
    } else if (type == QLatin1String("ERROR_MESSAGE")) {
        auto item = make(Kind::notice, clip(content.trimmed(), 300));
        item.failed = true;
        push(std::move(item));
    } else if (type == QLatin1String("CHECKPOINT")) {
        // agy's summary of the steps before, which stands in for them.
        if (const auto used = tokens(); used > 0)
            status_.context_tokens = used;
        push(make(Kind::divider, QStringLiteral("Conversation checkpoint")));
    }
    // EPHEMERAL_MESSAGE and step types lapis does not know are agy's own.
    if (static_cast<int>(items_.size()) > first)
        return changed < 0 ? first : std::min(changed, first);
    return changed;
}
} // namespace lapis::desktop::transcript
