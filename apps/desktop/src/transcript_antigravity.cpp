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
    static const QRegularExpression notes(QStringLiteral(R"(<([A-Z_]+)>.*?</\1>)"),
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
        step("Subagent", summary);
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

void AntigravityReader::call(const QJsonObject& call) {
    const auto name = text_of(call, "name");
    const auto args = call.value(QLatin1String("args")).toObject();
    auto item = tool_item(call);
    const int at = static_cast<int>(items_.size());
    std::vector<Item> more; // a step for each further subagent of one call
    if (name == QLatin1String("invoke_subagent")) {
        // Subagents: [{Model, Prompt, Role, TypeName, Workspace}], as a list
        // or as its JSON text.
        auto list = args.value(QLatin1String("Subagents"));
        if (list.isString())
            list = QJsonDocument::fromJson(list.toString().toUtf8()).array();
        const auto subagents = list.toArray();
        auto& tasks = spawned_[at];
        for (qsizetype i = 0; i < std::max<qsizetype>(1, subagents.size()); ++i) {
            const auto sub = subagents.isEmpty() ? QJsonObject{} : subagents.at(i).toObject();
            Task task;
            task.type = Task::Type::subagent;
            const auto role = text_of(sub, "Role");
            task.description = !role.isEmpty() ? role
                               : !text_of(sub, "Prompt").isEmpty()
                                   ? first_line(text_of(sub, "Prompt"))
                                   : item.body;
            task.agent_type = text_of(sub, "TypeName");
            task.model = text_of(sub, "Model");
            task.seen_ms = at_ms_;
            task.item = at + static_cast<int>(i);
            if (i == 0) {
                if (subagents.size() > 1 || item.body.isEmpty())
                    item.body = task.description;
                item.detail = clip(text_of(sub, "Prompt"), 600);
                item.task = start_task(std::move(task));
                tasks.push_back(item.task);
            } else {
                auto extra = make(Kind::tool, QStringLiteral("Subagent"), task.description,
                                  clip(text_of(sub, "Prompt"), 600));
                extra.task = start_task(std::move(task));
                tasks.push_back(extra.task);
                more.push_back(std::move(extra));
            }
        }
    } else if (name == QLatin1String("manage_task")) {
        // A check on a background task folds into that task's step.
        if (const int task = task_ids_.value(text_of(args, "TaskId"), -1); task >= 0) {
            item.quiet = true;
            spawned_[at] = {task};
        }
    }
    waiting_.push_back(at);
    called_.insert(at, name);
    push(std::move(item));
    for (auto& extra : more) {
        extra.answered = true;
        push(std::move(extra));
    }
}

int AntigravityReader::result(int at, const QJsonObject& record) {
    auto& step = items_[static_cast<size_t>(at)];
    attach(step, record);
    const auto name = called_.take(at);
    const auto content = text_of(record, "content");
    int changed = at;
    const auto touched = [&changed](int item) { changed = std::min(changed, item); };
    if (name == QLatin1String("invoke_subagent")) {
        // "Created the following subagents:" and a {conversationId,
        // logAbsoluteUri, workspaceUris} for each, in order.
        static const QRegularExpression made(
            QStringLiteral(R"re("conversationId"\s*:\s*"([^"]+)")re"));
        const auto tasks = spawned_.take(at);
        auto found = made.globalMatch(content);
        for (const int task : tasks) {
            if (!found.hasNext())
                break;
            name_task(task, found.next().captured(1));
        }
        if (step.failed)
            for (const int task : tasks)
                touched(
                    end_task(tasks_[static_cast<size_t>(task)], QStringLiteral("failed"), at_ms_));
        else {
            step.result.clear();
            step.result_lines = 0;
        }
    } else if (name == QLatin1String("run_command") &&
               text_of(record, "status") == QLatin1String("RUNNING")) {
        // "Tool is running as a background task with task id: `<id>`" and
        // "Task logs are available at: file:///.../task-N.log".
        static const QRegularExpression task_id(QStringLiteral(R"(task id: `([^`]+)`)"));
        static const QRegularExpression logs(QStringLiteral(R"(available at: (\S+))"));
        if (const auto id = task_id.match(content); id.hasMatch() && step.task < 0) {
            Task task;
            task.type = Task::Type::background;
            task.id = id.captured(1);
            task.item = at;
            task.description = first_line(step.body);
            task.started_ms = step.at_ms;
            task.seen_ms = at_ms_;
            if (const auto file = logs.match(content); file.hasMatch())
                task.file = file.captured(1);
            task.last_check = step.result;
            step.task = start_task(std::move(task));
            step.title = QStringLiteral("Started in background");
            step.result.clear();
            step.result_lines = 0;
            step.duration_ms = 0;
        }
    } else if (name == QLatin1String("manage_task")) {
        static const QRegularExpression checked(QStringLiteral(R"(task id: `?([^\s`]+/task-\d+))"));
        Task* task = nullptr;
        if (const auto checked_task = spawned_.take(at); !checked_task.empty())
            task = &tasks_[static_cast<size_t>(checked_task.front())];
        if (task == nullptr)
            if (const auto id = checked.match(content); id.hasMatch())
                task = task_named(id.captured(1));
        if (task != nullptr) {
            ++task->checks;
            task->seen_ms = std::max(task->seen_ms, at_ms_);
            task->last_check = step.result;
            static const QRegularExpression exited(QStringLiteral(R"(exited with code (-?\d+))"));
            if (const auto code = exited.match(content); code.hasMatch()) {
                task->exit_code = code.captured(1).toInt();
                task->exited = true;
            }
            if (task->status.isEmpty() && content.contains(QLatin1String("Status: DONE")))
                end_task(*task,
                         task->exited && task->exit_code != 0 ? QStringLiteral("failed")
                                                              : QStringLiteral("completed"),
                         at_ms_);
            touched(task->item);
        }
    } else if (name == QLatin1String("manage_subagents")) {
        // list: [{role, type, conversationId, transcript, state}]; an idle
        // subagent is done. kill stops those it names.
        static const QRegularExpression listed(
            QStringLiteral(R"re("conversationId"\s*:\s*"([^"]+)"[^}]*?"state"\s*:\s*"([^"]+)")re"));
        for (auto entries = listed.globalMatch(content); entries.hasNext();) {
            const auto entry = entries.next();
            if (auto* task = task_named(entry.captured(1))) {
                task->seen_ms = std::max(task->seen_ms, at_ms_);
                if (entry.captured(2) != QLatin1String("running") && task->status.isEmpty())
                    touched(end_task(*task, QStringLiteral("completed"), at_ms_));
            }
        }
    }
    return changed;
}

int AntigravityReader::system_note(const QString& content) {
    // `Task id "<conversation>/task-N" finished with result:` or `was
    // canceled with result:`, then what it printed.
    static const QRegularExpression finished(
        QStringLiteral(R"re(Task id "([^"]+)" (finished|was canceled))re"));
    if (const auto match = finished.match(content); match.hasMatch()) {
        auto* task = task_named(match.captured(1));
        if (task == nullptr || !task->status.isEmpty())
            return -1;
        static const QRegularExpression exited(QStringLiteral(R"(exited with code (-?\d+))"));
        if (const auto code = exited.match(content); code.hasMatch()) {
            task->exit_code = code.captured(1).toInt();
            task->exited = true;
        }
        const auto rest = content.mid(match.capturedEnd())
                              .section(QLatin1Char(':'), 1)
                              .remove(QStringLiteral("</SYSTEM_MESSAGE>"));
        if (!rest.trimmed().isEmpty())
            task->last_check = clip(without_escapes(rest).trimmed(), result_preview_chars);
        const bool canceled = match.captured(2) != QLatin1String("finished");
        return end_task(*task,
                        canceled                               ? QStringLiteral("stopped")
                        : task->exited && task->exit_code != 0 ? QStringLiteral("failed")
                                                               : QStringLiteral("completed"),
                        at_ms_);
    }
    // "[Message] timestamp=... sender=<conversation> priority=...": a
    // subagent's message to its parent, its report; it has finished.
    static const QRegularExpression message(QStringLiteral(R"(\[Message\][^\n]*sender=(\S+))"));
    if (const auto match = message.match(content); match.hasMatch()) {
        auto* task = task_named(match.captured(1));
        if (task == nullptr || task->type != Task::Type::subagent)
            return -1;
        const auto body = content.mid(content.indexOf(QLatin1Char('\n'), match.capturedEnd()) + 1);
        task->report = clip(QString(body).remove(QStringLiteral("</SYSTEM_MESSAGE>")).trimmed(),
                            result_preview_chars);
        return task->status.isEmpty() ? end_task(*task, QStringLiteral("completed"), at_ms_)
                                      : task->item;
    }
    return -1;
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
        // agy reads input sent while it works at its next step, inside the
        // running turn: input right after a tool step of an open turn
        // steers it. Long after one, the turn was left (interrupted).
        constexpr qint64 steer_window_ms = 10 * 60 * 1000;
        const bool steers = turn_open_ && step_at_ms_ > 0 && at_ms_ >= step_at_ms_ &&
                            at_ms_ - step_at_ms_ <= steer_window_ms;
        step_at_ms_ = 0;
        if (!steers) {
            waiting_.clear(); // results never written for calls before this
            called_.clear();
            spawned_.clear();
            turn_since_ms_ = at_ms_;
        }
        turn_open_ = true;
        bool shown = false;
        if (const auto text = request_text(content); !text.isEmpty()) {
            auto prompt = make(Kind::prompt, {}, clip(text, body_chars));
            prompt.starts_turn = !steers;
            prompt.steered = steers;
            taken(prompt);
            push(std::move(prompt));
            shown = true;
        }
        for (const auto& value : record.value(QLatin1String("media")).toArray()) {
            const auto media = value.toObject();
            auto path = text_of(media, "uri");
            if (path.startsWith(QLatin1String("file://")))
                path = QUrl(path).toLocalFile();
            const bool image = text_of(media, "mime_type").startsWith(QLatin1String("image/"));
            auto prompt =
                make(Kind::prompt, {},
                     QStringLiteral("%1 %2").arg(
                         image ? QStringLiteral("[image]") : QStringLiteral("[file]"), path));
            prompt.starts_turn = !shown && !steers;
            prompt.steered = steers;
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
        // A thought of only agy's stock line stays a step with no text.
        if (!thinking.isEmpty())
            push(make(Kind::thinking, QStringLiteral("Thought"),
                      clip(thought_text(thinking), body_chars)));
        if (!reply.isEmpty())
            push(make(Kind::reply, {}, clip(reply, body_chars)));
        for (const auto& value : calls)
            call(value.toObject());
        step_at_ms_ = calls.isEmpty() ? 0 : at_ms_;
        if (!reply.isEmpty() && calls.isEmpty()) {
            // Text with nothing left to do ends the turn.
            const auto ms =
                turn_since_ms_ > 0 && at_ms_ > turn_since_ms_ ? at_ms_ - turn_since_ms_ : 0;
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
        changed = result(at, record);
        if (turn_open_)
            step_at_ms_ = at_ms_;
    } else if (type == QLatin1String("SYSTEM_MESSAGE")) {
        // A task's end or a subagent's report lands on the step that
        // started it.
        changed = system_note(content);
        // Notes for the model; one after the turn ended wakes the agent.
        if (!turn_open_) {
            auto why = content;
            why.remove(QStringLiteral("<SYSTEM_MESSAGE>"))
                .remove(QStringLiteral("</SYSTEM_MESSAGE>"));
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
