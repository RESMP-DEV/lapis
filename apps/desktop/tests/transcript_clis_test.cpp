// The visual view's readers for CLIs other than Claude Code, against the
// synthetic conversations in tests/visual: each record type becomes the
// thread's items, turns fold and are timed, the toolbar's model, effort and
// context follow, unknown records are ignored or kept as other blocks, and a
// file still being written is followed line by line.
#include "transcript.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <iterator>

using lapis::desktop::TranscriptModel;
using namespace lapis::desktop::transcript;

namespace {
int failures = 0;
void require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

QString fixture(const char* name) {
    return QDir(QStringLiteral(LAPIS_VISUAL_FIXTURES)).filePath(QString::fromUtf8(name));
}
const char* const kCodex =
    "codex/rollout-2026-11-10T10-00-00-0199a000-0000-7000-8000-00000000c0de.jsonl";

QList<QByteArray> lines_of(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    auto lines = file.readAll().split('\n');
    while (!lines.isEmpty() && lines.back().isEmpty())
        lines.removeLast();
    return lines;
}

template <typename R> R read_all(const QString& path) {
    R reader;
    for (const auto& line : lines_of(path))
        (void)reader.add(line);
    return reader;
}

void formats_are_told_apart() {
    require(format_of(QStringLiteral("/s/2026/11/10/rollout-2026-11-10T10-00-00-x.jsonl")) ==
                Format::codex,
            "rollout is Codex");
    require(format_of(QStringLiteral("/b/id/.system_generated/logs/transcript_full.jsonl")) ==
                Format::antigravity,
            "transcript_full is Antigravity");
    require(format_of(QStringLiteral("/p/-work/0b4d.jsonl")) == Format::claude, "else Claude");
    require(format_named(QStringLiteral("agy")) == Format::antigravity &&
                format_named(QStringLiteral("codex")) == Format::codex &&
                !format_named(QStringLiteral("grok")),
            "formats by name");
    require(make_reader(Format::codex)->format() == Format::codex, "Codex reader made");
}

void codex_items() {
    const auto reader = read_all<CodexReader>(fixture(kCodex));
    const auto& items = reader.items();
    require(items.size() == 25, "Codex: 25 items");
    if (items.size() != 25) {
        for (const auto& item : items)
            std::fprintf(stderr, "  %s %s | %s\n", qPrintable(kind_name(item.kind)),
                         qPrintable(item.title), qPrintable(item.body.left(40)));
        return;
    }
    require(items[0].kind == Kind::prompt && items[0].starts_turn &&
                items[0].body == "Make the greeting configurable and run the tests",
            "Codex: prompt from UserMessage, not the response_item copy");
    require(items[1].kind == Kind::thinking && items[1].title == "Thought for 2s" &&
                items[1].body.startsWith("**Finding the greeting**"),
            "Codex: reasoning summary as thinking; empty reasoning skipped");
    require(items[2].kind == Kind::reply && items[2].body.startsWith("I'll find"),
            "Codex: commentary is a reply");
    require(items[3].kind == Kind::tool && items[3].title == "Searched for" &&
                items[3].body == "greeting" && items[3].detail == "/work/demo/src" &&
                items[3].duration_ms == 420 && items[3].answered && !items[3].failed,
            "Codex: a search by Codex's reading of the command");
    require(items[4].after_step && !items[3].after_step, "Codex: steps draw tight");
    require(items[4].title == "Read" && items[4].body == "/work/demo/src/greet.cpp" &&
                items[4].result_lines == 4,
            "Codex: a read with its output");
    require(items[5].title == "Listed" && items[5].body == "/work/demo/tests", "Codex: listing");
    require(items[6].title == "Wrote" && items[6].body == "/work/demo/src/flags.hpp" &&
                items[6].diff.empty(),
            "Codex: an added file");
    require(items[7].title == "Edited" && items[7].body == "/work/demo/src/greet.cpp" &&
                items[7].added == 3 && items[7].removed == 2 && items[7].diff.size() == 7 &&
                items[7].diff[0].sign == ' ' && items[7].diff[1].sign == '-' &&
                items[7].diff[3].text == "std::string greet(const std::string& word) {",
            "Codex: a patch as a diff with context");
    require(items[8].title == "Deleted" && items[8].body == "/work/demo/src/old_greet.cpp",
            "Codex: a deleted file");
    require(items[9].title == "Ran" && items[9].body == "ctest --test-dir build" &&
                items[9].failed && items[9].result_lines == 4 && items[9].duration_ms == 69000,
            "Codex: a failed command, its output and time");
    require(items[10].title == "Called" && items[10].body == "Look up flag parsing" &&
                items[10].detail == "docs server · search docs" &&
                items[10].result == "Flags are parsed in main()." && items[10].duration_ms == 5000,
            "Codex: an MCP tool call");
    require(items[11].title == "Searched the web for" && items[11].body == "gtest flag parsing" &&
                items[11].result.contains("https://example.com/flags"),
            "Codex: a web search and its results");
    require(items[12].title == "Waited" && items[12].body == "30s", "Codex: a sleep");
    require(items[13].title == "Viewed image" && items[13].body == "/work/demo/docs/screenshot.png",
            "Codex: an image viewed");
    require(items[14].kind == Kind::reply && items[14].body.contains("```cpp"),
            "Codex: final answer keeps its markdown");
    require(items[15].kind == Kind::turn && items[15].duration_ms == 192000 &&
                items[15].title == "Worked for 3m 12s",
            "Codex: task_complete times the turn");
    require(items[16].kind == Kind::prompt && items[16].starts_turn &&
                items[16].body == "Does this screenshot match?",
            "Codex: text with an inline image is one prompt");
    require(items[17].kind == Kind::prompt && !items[17].starts_turn &&
                items[17].body == "Also check the footer" && items[17].steered &&
                items[17].waited == -1,
            "Codex: a message sent mid-turn steers that turn (its wait unknown)");
    require(items[18].kind == Kind::other && items[18].title == "FuturePlan",
            "Codex: an unknown item stays visible");
    require(items[19].kind == Kind::notice && items[19].title == "Interrupted",
            "Codex: an interrupted turn says so");
    require(items[20].kind == Kind::turn && items[20].duration_ms == 19000,
            "Codex: turn_aborted ends the turn");
    require(items[21].kind == Kind::divider && items[21].title == "Conversation compacted",
            "Codex: compaction divider");
    require(items[22].kind == Kind::prompt && items[23].kind == Kind::reply &&
                items[24].kind == Kind::turn && items[24].duration_ms == 9000,
            "Codex: a turn after compaction");

    const auto& status = reader.status();
    require(status.model == "gpt-6-codex" && status.effort == "high",
            "Codex: model from turn_context, effort from thread settings");
    require(status.context_tokens == 12300 && status.window_tokens == 272000 &&
                context_window(status) == 272000,
            "Codex: context from the last token count, window as Codex reports it");

    const auto layout = lay_out(items, {});
    require(layout.turns.size() == 3, "Codex: three turns");
    if (layout.turns.size() == 3) {
        const auto& first = layout.turns[0];
        require(first.closed && first.final_reply == 14 && first.steps == 13 &&
                    first.duration_ms == 192000,
                "Codex: the first turn folds 13 steps under its final reply");
        require(first.summary == "ran a command, edited 3 files, read a file, searched 2 times, "
                                 "used the web, called 3 tools",
                "Codex: work summary");
        require(layout.turns[1].closed && layout.turns[1].final_reply == -1 &&
                    layout.turns[1].duration_ms == 19000,
                "Codex: the interrupted turn");
    }
}

void codex_bad_lines_are_ignored() {
    CodexReader reader;
    require(reader.add("not json") == -1 && reader.add("[1,2]") == -1 &&
                reader.add(R"({"type":"event_msg"})") == -1 &&
                reader.add(R"({"type":"event_msg","payload":{"type":"item_completed"}})") == -1 &&
                reader.add(R"({"type":"response_item","payload":{"type":"message","role":"user",)"
                           R"("content":[{"type":"input_text","text":"x"}]}})") == -1,
            "Codex: unparsable, empty and model-facing records add nothing");
    require(reader.items().empty(), "Codex: nothing shown");
}

// The model reads a rollout by its name and follows it as Codex appends,
// keeping a half-written last line until it is finished.
void codex_file_is_followed() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("rollout-2026-11-10T10-00-00-test.jsonl"));
    const auto lines = lines_of(fixture(kCodex));
    require(lines.size() > 30, "Codex fixture read");
    QByteArray head;
    for (int i = 0; i < 24; ++i)
        head += lines[i] + '\n';
    const auto partial = lines[24].left(lines[24].size() / 2);
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(head + partial) > 0, "write head");
    }
    TranscriptModel model;
    model.setPath(path);
    model.loadNow();
    require(model.format() == Format::codex && model.formatName() == "codex",
            "model: Codex by the file's name");
    require(!model.forkable() && !model.screenKnown(), "model: no fork, no screen layout");
    require(model.liveLines({QStringLiteral("working"), QString(30, QChar(0x2500)),
                             QStringLiteral("> draft"), QString(30, QChar(0x2500))})
                    .isEmpty() &&
                !model.dialogShowing({QString(30, QChar(0x2500)), QStringLiteral("Settings")}),
            "model: no live tail or dialog peek for Codex");
    // The first turn is closed: prompt, its "Worked for" row, its reply.
    require(model.rowCount() == 3, "model: the first turn folds to three rows");
    require(model.data(model.index(1), TranscriptModel::KindRole).toString() == "work" &&
                model.data(model.index(1), TranscriptModel::TitleRole).toString() ==
                    "Worked for 3m 12s",
            "model: Worked for row");
    require(model.model() == "gpt-6-codex" && model.effort() == "medium" &&
                model.contextWindow() == 272000 && model.contextTokens() == 42000,
            "model: status so far");
    {
        QFile file(path);
        QByteArray rest = lines[24].mid(partial.size()) + '\n';
        for (qsizetype i = 25; i < lines.size(); ++i)
            rest += lines[i] + '\n';
        require(file.open(QIODevice::Append) && file.write(rest) > 0, "append rest");
    }
    model.readMore();
    require(model.effort() == "high" && model.contextTokens() == 12300,
            "model: the finished line and the rest were read");
    // Turn 1: 3 rows; turn 2 (closed, nothing to fold but its notices) and
    // turn 3 after the divider.
    bool found_steer = false;
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.data(model.index(row), TranscriptModel::BodyRole).toString() ==
            "Also check the footer")
            found_steer = true;
    require(found_steer, "model: the mid-turn message shows");
    require(model.runningSince() == 0, "model: no turn running");
}
const char* const kAntigravity = "antigravity/transcript_full.jsonl";

void antigravity_items() {
    const auto reader = read_all<AntigravityReader>(fixture(kAntigravity));
    const auto& items = reader.items();
    require(items.size() == 22, "agy: 22 items");
    if (items.size() != 22) {
        for (const auto& item : items)
            std::fprintf(stderr, "  %s %s | %s\n", qPrintable(kind_name(item.kind)),
                         qPrintable(item.title), qPrintable(item.body.left(40)));
        return;
    }
    require(items[0].kind == Kind::prompt && items[0].starts_turn &&
                items[0].body == "Make the greeting configurable and run the tests",
            "agy: the request without agy's notes");
    require(items[1].kind == Kind::thinking && items[1].title == "Thought", "agy: thinking");
    require(items[2].title == "Searched for" && items[2].body == "greeting" &&
                items[2].detail == "/work/demo/src" && items[2].answered &&
                items[2].result == "src/greet.cpp:3:    return \"hello\";",
            "agy: a search and its result, timestamps left out");
    require(items[3].title == "Read" && items[3].body == "/work/demo/src/greet.cpp" &&
                items[3].duration_ms == 1000 && items[3].after_step,
            "agy: a read timed from its result");
    require(items[4].kind == Kind::reply, "agy: text beside tool calls is a reply");
    require(items[5].title == "Edited" && items[5].added == 2 && items[5].removed == 2 &&
                items[5].result.isEmpty() && items[5].answered,
            "agy: an edit as a diff");
    require(items[6].title == "Wrote" && items[6].body == "/work/demo/src/flags.hpp" &&
                items[6].added == 3,
            "agy: a written file");
    require(items[7].title == "Ran" && items[7].body == "ctest --test-dir build" &&
                items[7].detail == "Run the tests" && items[7].failed &&
                items[7].duration_ms == 69000 && items[7].result_lines == 5,
            "agy: a failed command by its exit code");
    require(items[8].kind == Kind::notice && items[8].failed &&
                items[8].title.startsWith("Error: The stream"),
            "agy: an error message; a mid-turn system note is not shown");
    require(items[9].kind == Kind::reply && items[10].kind == Kind::turn &&
                items[10].duration_ms == 192000 && items[10].title == "Worked for 3m 12s",
            "agy: a reply with no calls ends and times the turn");
    require(items[11].kind == Kind::notice && items[11].starts_turn && items[11].quiet &&
                items[11].title == "The following background task finished: tests",
            "agy: a system note after the turn wakes the agent");
    require(items[12].title == "Checked task" && items[12].body == "Check the test task",
            "agy: task check");
    require(items[14].kind == Kind::turn && items[14].duration_ms == 12000, "agy: woken turn");
    require(items[15].kind == Kind::prompt && items[15].starts_turn &&
                items[16].kind == Kind::prompt && !items[16].starts_turn &&
                items[16].body == "[image] /work/demo/docs/screenshot.png" &&
                items[16].record == items[15].record,
            "agy: an attached image under its prompt");
    require(items[17].kind == Kind::divider && items[17].title == "Conversation checkpoint",
            "agy: checkpoint divider; ephemeral, unknown and repeated steps skipped");
    require(items[18].title == "Searched the web for" && items[18].duration_ms == 3000,
            "agy: web search");
    require(items[19].title == "Called" && items[19].body == "Look up the greeting docs" &&
                items[19].detail == "lookup_docs" && items[19].failed &&
                items[19].result == "tool lookup_docs is not available",
            "agy: an unknown tool and its error");
    require(items[21].kind == Kind::turn && items[21].duration_ms == 30000, "agy: last turn");

    const auto& status = reader.status();
    require(status.model == "Gemini 4 Argon" && status.effort == "medium" &&
                status.window_tokens == 1'048'576 && status.context_tokens == 35200,
            "agy: model and effort from the settings note, context from the last response");

    const auto layout = lay_out(items, {});
    require(layout.turns.size() == 3, "agy: three turns");
    if (layout.turns.size() == 3) {
        require(layout.turns[0].closed && layout.turns[0].final_reply == 9 &&
                    layout.turns[0].steps == 8 &&
                    layout.turns[0].summary ==
                        "ran a command, edited 2 files, read a file, searched once",
                "agy: the first turn folds");
        require(layout.turns[1].closed && layout.turns[1].duration_ms == 12000,
                "agy: the woken turn");
    }
}

void antigravity_bad_lines_are_ignored() {
    AntigravityReader reader;
    require(reader.add("{") == -1 && reader.add("42") == -1 &&
                reader.add(R"({"type":"GENERIC","content":"no call waits"})") == -1 &&
                reader.add(R"({"type":"PLANNER_RESPONSE"})") == -1 &&
                reader.add(R"({"type":"EPHEMERAL_MESSAGE","content":"x"})") == -1,
            "agy: bad lines, orphan results and empty responses add nothing");
    require(reader.items().empty(), "agy: nothing shown");
}

void antigravity_file_is_followed() {
    QTemporaryDir directory;
    QDir(directory.path()).mkpath(QStringLiteral("logs"));
    const auto path = directory.filePath(QStringLiteral("logs/transcript_full.jsonl"));
    const auto lines = lines_of(fixture(kAntigravity));
    QByteArray head;
    for (int i = 0; i < 8; ++i)
        head += lines[i] + '\n';
    const auto partial = lines[8].left(40);
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(head + partial) > 0, "write head");
    }
    TranscriptModel model;
    model.setPath(path);
    model.loadNow();
    require(model.format() == Format::antigravity && model.formatName() == "antigravity" &&
                !model.forkable() && !model.screenKnown(),
            "model: Antigravity by the file's name");
    // While the agent works, a running turn is its prompt and the live box,
    // which lists every step so far.
    model.setLive(true);
    const auto& steps = *qobject_cast<lapis::desktop::LiveStepsModel*>(model.liveSteps());
    require(model.rowCount() == 2 && model.runningSince() > 0 &&
                model.data(model.index(1), TranscriptModel::KindRole).toString() == "live" &&
                steps.rowCount() == 7,
            "model: running turn is its prompt and the live box");
    const auto ran = steps.index(6);
    require(steps.data(ran, TranscriptModel::TitleRole).toString() == "Ran" &&
                !steps.data(ran, TranscriptModel::AnsweredRole).toBool(),
            "model: the command waits for its half-written result");
    {
        QFile file(path);
        QByteArray rest = lines[8].mid(partial.size()) + '\n';
        for (qsizetype i = 9; i < lines.size(); ++i)
            rest += lines[i] + '\n';
        require(file.open(QIODevice::Append) && file.write(rest) > 0, "append rest");
    }
    model.readMore();
    require(model.runningSince() == 0 && model.turnCount() == 3 &&
                model.model() == "Gemini 4 Argon" && model.contextWindow() == 1'048'576,
            "model: the rest was read and every turn closed");
    require(model.data(model.index(1), TranscriptModel::KindRole).toString() == "work" &&
                model.data(model.index(1), TranscriptModel::TitleRole).toString() ==
                    "Worked for 3m 12s",
            "model: the first turn folds to Worked for");
}
// The live work box follows a file the CLI writes line by line: while a
// turn runs (the agent working) the thread holds no step rows, the box's
// rows only grow (a steering prompt starts it again), and a prompt sent
// through lapis waits as pending until the CLI records it.
void live_box_grows(const char* name, const QString& file_name, const QString& sent) {
    QTemporaryDir directory;
    QDir(directory.path()).mkpath(QStringLiteral("logs"));
    const auto path = directory.filePath(file_name);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "live: fixture opens");
    TranscriptModel model;
    model.setLive(true);
    model.setPath(path);
    model.loadNow();
    const auto& steps = *qobject_cast<lapis::desktop::LiveStepsModel*>(model.liveSteps());
    int resets = 0;
    QObject::connect(&steps, &QAbstractItemModel::modelReset, [&resets] { ++resets; });
    int running_lines = 0, most = 0, step_rows = 0, shrank = 0, prompts_before = 0;
    bool pending_seen = false, steered_seen = false;
    for (const auto& line : lines_of(fixture(name))) {
        const bool sends = !sent.isEmpty() && line.contains(sent.toUtf8()) &&
                           (line.contains("UserMessage") || line.contains("USER_INPUT"));
        if (sends) {
            model.noteSent(sent);
            pending_seen = model.pendingPrompts() == QStringList{sent};
        }
        const int before = steps.rowCount();
        file.write(line + '\n');
        file.flush();
        model.readMore();
        int prompts = 0;
        for (int row = 0; row < model.rowCount(); ++row) {
            const auto index = model.index(row);
            const auto kind = model.data(index, TranscriptModel::KindRole).toString();
            step_rows += kind == "tool" || kind == "thinking" ? 1 : 0;
            prompts += kind == "prompt" ? 1 : 0;
            if (sends && kind == "prompt" &&
                model.data(index, TranscriptModel::BodyRole).toString() == sent)
                steered_seen = model.data(index, TranscriptModel::SteeredRole).toBool() &&
                               model.data(index, TranscriptModel::WaitedRole).toInt() == 0;
        }
        if (model.runningSince() > 0) {
            ++running_lines;
            most = std::max(most, steps.rowCount());
            // Shrinking is allowed only when a new prompt came in.
            if (steps.rowCount() < before && prompts == prompts_before)
                ++shrank;
        }
        prompts_before = prompts;
    }
    const auto what = [name](const char* check) {
        static QByteArray text;
        text = QByteArray(name) + ": " + check;
        return text.constData();
    };
    require(running_lines > 3 && most >= 3, what("the box filled while turns ran"));
    require(step_rows == 0, what("the thread showed no step rows while folded or live"));
    require(shrank == 0, what("the box only grew within a part"));
    require(model.runningSince() == 0 && steps.rowCount() == 0, what("the box empties at the end"));
    require(resets <= model.turnCount() * 2 + 2, what("the box resets only between parts"));
    if (!sent.isEmpty())
        require(pending_seen && steered_seen && model.pendingPrompts().isEmpty(),
                what("a prompt sent mid-turn waits as pending, then steers, waiting no steps"));
}

// agy reads input sent while it works at its next step: right after a tool
// step of a running turn it steers that turn; long after, it starts one.
void antigravity_steering() {
    AntigravityReader reader;
    const char* const lines[] = {
        R"({"step_index":0,"type":"USER_INPUT","created_at":"2026-11-10T16:00:00Z","content":"<USER_REQUEST>go</USER_REQUEST>"})",
        R"({"step_index":1,"type":"PLANNER_RESPONSE","created_at":"2026-11-10T16:00:02Z","tool_calls":[{"name":"run_command","args":{"CommandLine":"make"}}]})",
        R"({"step_index":2,"type":"GENERIC","created_at":"2026-11-10T16:00:03Z","content":"ok"})",
        R"({"step_index":3,"type":"USER_INPUT","created_at":"2026-11-10T16:00:09Z","content":"<USER_REQUEST>use tabs</USER_REQUEST>"})",
        R"({"step_index":4,"type":"PLANNER_RESPONSE","created_at":"2026-11-10T16:00:12Z","tool_calls":[{"name":"view_file","args":{"AbsolutePath":"/w/a.cpp"}}]})",
        R"({"step_index":5,"type":"GENERIC","created_at":"2026-11-10T16:00:13Z","content":"x"})",
        R"({"step_index":6,"type":"PLANNER_RESPONSE","created_at":"2026-11-10T16:00:20Z","tool_calls":[{"name":"run_command","args":{"CommandLine":"make"}}]})",
        R"({"step_index":7,"type":"GENERIC","created_at":"2026-11-10T16:00:21Z","content":"ok"})",
        R"({"step_index":8,"type":"USER_INPUT","created_at":"2026-11-10T17:00:00Z","content":"<USER_REQUEST>later</USER_REQUEST>"})",
    };
    for (const char* line : lines)
        (void)reader.add(line);
    const auto& items = reader.items();
    require(items.size() == 6 && items[2].body == "use tabs" && items[2].steered &&
                !items[2].starts_turn && items[5].body == "later" && items[5].starts_turn &&
                !items[5].steered,
            "agy: input after a tool step steers; an hour later it starts a turn");
    const auto layout = lay_out(items, {});
    require(layout.turns.size() == 2 && layout.turns[0].parts.size() == 2 &&
                layout.turns[0].parts[1].steps == 2,
            "agy: the steering prompt splits the turn's work");
}
void write_lines(const QString& path, const QStringList& lines) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "fixture written");
    for (const auto& line : lines)
        file.write(line.toUtf8() + '\n');
}

// Made-up Codex records, in the shapes Codex 0.157.1's source gives its
// collaboration items: two subagents spawned, one reported done by a wait,
// the other by its activity; and a command still running when exec_command
// yielded, polled once, then ended.
const char* const kCodexWork[] = {
    R"({"timestamp":"2026-11-11T16:00:00.000Z","type":"session_meta","payload":{"id":"parent-thread","source":"cli"}})",
    R"({"timestamp":"2026-11-11T16:00:01.000Z","type":"event_msg","payload":{"type":"task_started","turn_id":"turn-1"}})",
    R"({"timestamp":"2026-11-11T16:00:01.100Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"UserMessage","id":"u1","content":[{"type":"text","text":"Split the parser"}]}}})",
    R"({"timestamp":"2026-11-11T16:00:02.000Z","type":"response_item","payload":{"type":"custom_tool_call","name":"exec","call_id":"c1","input":"const r = await tools.exec_command({cmd: \"npm test\", yield_time_ms: 1000});"}})",
    R"({"timestamp":"2026-11-11T16:00:03.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"c1","output":[{"type":"input_text","text":"{\"chunk_id\":\"a1\",\"wall_time_seconds\":1.0,\"session_id\":7,\"original_token_count\":3,\"output\":\"running 1\\n\"}"}]}})",
    R"({"timestamp":"2026-11-11T16:00:04.000Z","type":"event_msg","payload":{"type":"item_completed","started_at_ms":1794412804000,"item":{"type":"CollabAgentToolCall","id":"k1","tool":"spawn_agent","status":"completed","sender_thread_id":"parent-thread","receiver_thread_ids":["child-1","child-2"],"receiver_agents":[{"thread_id":"child-1","agent_nickname":"Ada","agent_role":"explorer"},{"thread_id":"child-2","agent_nickname":"Bo"}],"prompt":"Map the lexer","model":"gpt-6-codex","agents_states":{"child-1":"running","child-2":"pending_init"}}}})",
    R"({"timestamp":"2026-11-11T16:00:20.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"c2","output":[{"type":"input_text","text":"{\"chunk_id\":\"a2\",\"wall_time_seconds\":5.0,\"session_id\":7,\"original_token_count\":3,\"output\":\"running 2\\n\"}"}]}})",
    R"({"timestamp":"2026-11-11T16:01:00.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"CollabAgentToolCall","id":"k2","tool":"wait","status":"completed","sender_thread_id":"parent-thread","receiver_thread_ids":["child-1"],"agents_states":{"child-1":{"completed":"The lexer is in lex.rs"}}}}})",
    R"({"timestamp":"2026-11-11T16:01:30.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"CommandExecution","id":"e1","process_id":"7","command":["/bin/zsh","-lc","npm test"],"cwd":"/work/demo","status":"failed","exit_code":1,"aggregated_output":"1 failed","duration":{"secs":87,"nanos":0}}}})",
    R"({"timestamp":"2026-11-11T16:01:40.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"SubAgentActivity","id":"v1","kind":"completed","agent_thread_id":"child-2","agent_path":"/root/bo"}}})",
    R"({"timestamp":"2026-11-11T16:01:41.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"AgentMessage","id":"m1","content":[{"type":"text","text":"Both are back."}]}}})",
    R"({"timestamp":"2026-11-11T16:01:42.000Z","type":"event_msg","payload":{"type":"task_complete","turn_id":"turn-1","duration_ms":101000}})",
};
// The first subagent's own rollout: one command and its reply.
const char* const kCodexChild[] = {
    R"({"timestamp":"2026-11-11T16:00:05.000Z","type":"session_meta","payload":{"id":"child-1","source":{"subagent":{"thread_spawn":{"parent_thread_id":"parent-thread","depth":1}}}}})",
    R"({"timestamp":"2026-11-11T16:00:05.100Z","type":"event_msg","payload":{"type":"task_started","turn_id":"c-turn"}})",
    R"({"timestamp":"2026-11-11T16:00:05.200Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"UserMessage","id":"cu","content":[{"type":"text","text":"Map the lexer"}]}}})",
    R"({"timestamp":"2026-11-11T16:00:09.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"CommandExecution","id":"ce","process_id":"9","command":["/bin/zsh","-lc","rg lex"],"cwd":"/work/demo","status":"completed","exit_code":0,"aggregated_output":"lex.rs","duration":{"secs":1,"nanos":0}}}})",
    R"({"timestamp":"2026-11-11T16:00:50.000Z","type":"event_msg","payload":{"type":"item_completed","item":{"type":"AgentMessage","id":"cm","content":[{"type":"text","text":"The lexer is in lex.rs"}]}}})",
    R"({"timestamp":"2026-11-11T16:00:51.000Z","type":"event_msg","payload":{"type":"task_complete","turn_id":"c-turn","duration_ms":46000}})",
};

QStringList all(const char* const* lines, size_t count) {
    QStringList out;
    for (size_t i = 0; i < count; ++i)
        out << QString::fromUtf8(lines[i]);
    return out;
}

void codex_subagents_and_background() {
    QTemporaryDir root;
    const auto day = root.filePath(QStringLiteral("sessions/2026/11/11"));
    const auto parent = day + QStringLiteral("/rollout-2026-11-11T10-00-00-parent-thread.jsonl");
    write_lines(parent, all(kCodexWork, std::size(kCodexWork)));
    write_lines(day + QStringLiteral("/rollout-2026-11-11T10-00-05-child-1.jsonl"),
                all(kCodexChild, std::size(kCodexChild)));
    TranscriptModel model;
    model.setNow(
        QDateTime::fromString(QStringLiteral("2026-11-11T16:02:00.000Z"), Qt::ISODateWithMs)
            .toMSecsSinceEpoch());
    model.setPath(parent);
    model.loadNow();
    const auto& tasks = model.tasks();
    const auto& items = model.items();
    require(tasks.size() == 3, "codex: a background process and two subagents");
    if (tasks.size() != 3)
        return;
    require(tasks[0].type == Task::Type::background && tasks[0].id == "process 7" &&
                tasks[0].checks == 1 && tasks[0].status == "failed" && tasks[0].exited &&
                tasks[0].exit_code == 1 && tasks[0].duration_ms == 87000,
            "codex: the yielded command, its poll and its end");
    const auto& start = items[static_cast<size_t>(tasks[0].item)];
    require(start.title == "Started in background" && start.body == "npm test" &&
                start.result == "1 failed",
            "codex: one step for it, its output attached");
    require(std::ranges::count_if(items, [](const Item& item) { return item.title == "Ran"; }) == 0,
            "codex: its end is no step of its own");
    require(tasks[1].type == Task::Type::subagent && tasks[1].id == "child-1" &&
                tasks[1].description == "Ada" && tasks[1].agent_type == "explorer" &&
                tasks[1].status == "completed" && tasks[1].report == "The lexer is in lex.rs",
            "codex: a subagent ended by a wait's states");
    require(tasks[2].id == "child-2" && tasks[2].status == "completed",
            "codex: a subagent ended by its activity");
    require(model.running().isEmpty(), "codex: nothing runs once all have ended");
    const auto detail = model.taskDetail(tasks[1].item);
    const auto steps = detail.value(QStringLiteral("steps")).toList();
    require(detail.value(QStringLiteral("found")).toBool() && steps.size() == 1 &&
                steps[0].toMap().value(QStringLiteral("title")) == "Ran" &&
                detail.value(QStringLiteral("report")).toString() == "The lexer is in lex.rs",
            "codex: an opened subagent reads its own rollout");
}

// Made-up agy steps: invoke_subagent creating two subagents, a command agy
// left running, checked once, then finished; one subagent's message and a
// listing showing the other idle end them.
const char* const kAgyWork[] = {
    R"({"step_index":0,"source":"USER_EXPLICIT","type":"USER_INPUT","status":"DONE","created_at":"2026-11-11T16:00:00Z","content":"<USER_REQUEST>\nSplit the parser\n</USER_REQUEST>"})",
    R"({"step_index":1,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","created_at":"2026-11-11T16:00:02Z","tool_calls":[{"name":"invoke_subagent","args":{"Subagents":[{"Role":"lexer mapper","Prompt":"Map the lexer","TypeName":"research","Model":"Gemini 4 Argon"},{"Role":"error checker","Prompt":"Check errors"}],"toolSummary":"Start two subagents"}},{"name":"run_command","args":{"CommandLine":"npm test","WaitMsBeforeAsync":1000,"toolSummary":"Run tests"}}]})",
    R"({"step_index":2,"source":"MODEL","type":"GENERIC","status":"DONE","created_at":"2026-11-11T16:00:03Z","content":"Created At: 2026-11-11T10:00:02-06:00\nCompleted At: 2026-11-11T10:00:03-06:00\nCreated the following subagents:\n{\n  \"conversationId\":  \"conv-a\",\n  \"logAbsoluteUri\":  \"file:///x/conv-a/transcript.jsonl\"\n}\n{\n  \"conversationId\":  \"conv-b\",\n  \"logAbsoluteUri\":  \"file:///x/conv-b/transcript.jsonl\"\n}"})",
    R"({"step_index":3,"source":"MODEL","type":"GENERIC","status":"RUNNING","created_at":"2026-11-11T16:00:04Z","content":"Tool is running as a background task with task id: `parent/task-1`\nTask Description: npm test\nTask logs are available at: file:///nowhere/task-1.log"})",
    R"({"step_index":4,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","created_at":"2026-11-11T16:00:30Z","tool_calls":[{"name":"manage_task","args":{"Action":"status","TaskId":"parent/task-1","toolSummary":"Check tests"}}]})",
    R"({"step_index":5,"source":"MODEL","type":"GENERIC","status":"DONE","created_at":"2026-11-11T16:00:31Z","content":"Status: RUNNING\n12 passed so far"})",
    R"({"step_index":6,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","created_at":"2026-11-11T16:00:40Z","content":"Waiting on them."})",
    R"({"step_index":7,"source":"SYSTEM","type":"SYSTEM_MESSAGE","status":"DONE","created_at":"2026-11-11T16:01:00Z","content":"<SYSTEM_MESSAGE>\nTask id \"parent/task-1\" finished with result:\nThe command exited with code 0.\n</SYSTEM_MESSAGE>"})",
    R"({"step_index":8,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","created_at":"2026-11-11T16:01:05Z","content":"Tests pass."})",
    R"({"step_index":9,"source":"SYSTEM","type":"SYSTEM_MESSAGE","status":"DONE","created_at":"2026-11-11T16:02:00Z","content":"<SYSTEM_MESSAGE>\n[Message] timestamp=2026-11-11T16:02:00Z sender=conv-a priority=MESSAGE_PRIORITY_HIGH\nThe lexer is in lex.go\n</SYSTEM_MESSAGE>"})",
    R"({"step_index":10,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","created_at":"2026-11-11T16:02:05Z","tool_calls":[{"name":"manage_subagents","args":{"Action":"list","toolSummary":"List subagents"}}]})",
    R"({"step_index":11,"source":"MODEL","type":"GENERIC","status":"DONE","created_at":"2026-11-11T16:02:06Z","content":"[{\"role\":\"error checker\",\"type\":\"research\",\"conversationId\":\"conv-b\",\"state\":\"idle\"}]"})",
    R"({"step_index":12,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","created_at":"2026-11-11T16:02:10Z","content":"Both are done."})",
};
const char* const kAgyChild[] = {
    R"({"step_index":0,"source":"USER_EXPLICIT","type":"USER_INPUT","status":"DONE","created_at":"2026-11-11T16:00:03Z","content":"<USER_REQUEST>\nMap the lexer\n</USER_REQUEST>"})",
    R"({"step_index":1,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","created_at":"2026-11-11T16:00:10Z","tool_calls":[{"name":"view_file","args":{"AbsolutePath":"/work/demo/lex.go"}}]})",
    R"({"step_index":2,"source":"MODEL","type":"GENERIC","status":"DONE","created_at":"2026-11-11T16:00:11Z","content":"package lex"})",
    R"({"step_index":3,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","created_at":"2026-11-11T16:01:50Z","content":"The lexer is in lex.go"})",
};

void antigravity_subagents_and_background() {
    QTemporaryDir root;
    const auto parent =
        root.filePath(QStringLiteral("brain/parent/.system_generated/logs/transcript_full.jsonl"));
    write_lines(parent, all(kAgyWork, std::size(kAgyWork)));
    const auto child =
        root.filePath(QStringLiteral("brain/conv-b/.system_generated/logs/transcript_full.jsonl"));
    TranscriptModel model;
    model.setNow(
        QDateTime::fromString(QStringLiteral("2026-11-11T16:00:50.000Z"), Qt::ISODateWithMs)
            .toMSecsSinceEpoch());
    model.setPath(parent);
    model.loadNow();
    const auto& tasks = model.tasks();
    const auto& items = model.items();
    require(tasks.size() == 3, "agy: two subagents and a background command");
    if (tasks.size() != 3)
        return;
    require(tasks[0].type == Task::Type::subagent && tasks[0].id == "conv-a" &&
                tasks[0].description == "lexer mapper" && tasks[0].agent_type == "research" &&
                tasks[1].id == "conv-b" && tasks[1].description == "error checker",
            "agy: each subagent by its role, named by its conversation");
    require(items[static_cast<size_t>(tasks[1].item)].title == "Subagent",
            "agy: each subagent its own step");
    require(tasks[2].type == Task::Type::background && tasks[2].id == "parent/task-1" &&
                items[static_cast<size_t>(tasks[2].item)].title == "Started in background",
            "agy: a command left running");
    require(tasks[2].checks == 1, "agy: checked once");
    require(tasks[2].last_check == "The command exited with code 0.",
            "agy: the newest check's output kept, the end's last");
    require(std::ranges::count_if(
                items, [](const Item& item) { return item.quiet && item.kind == Kind::tool; }) == 1,
            "agy: the check is no row of its own");
    require(tasks[2].status == "completed" && tasks[2].exited && tasks[2].exit_code == 0,
            "agy: the finished message ends the command");
    require(tasks[0].status == "completed" && tasks[0].report == "The lexer is in lex.go",
            "agy: a subagent's message is its report and its end");
    require(tasks[1].status == "completed", "agy: listed idle, a subagent has ended");
    // Its own conversation opens to its steps.
    write_lines(child, all(kAgyChild, std::size(kAgyChild)));
    const auto detail = model.taskDetail(tasks[1].item);
    const auto steps = detail.value(QStringLiteral("steps")).toList();
    require(detail.value(QStringLiteral("found")).toBool() && steps.size() == 1 &&
                steps[0].toMap().value(QStringLiteral("title")) == "Read" &&
                detail.value(QStringLiteral("report")).toString() == "The lexer is in lex.go",
            "agy: an opened subagent reads its own conversation");
}

// An agy subagent with no message yet runs until its own conversation
// comes to rest (its turn ended with a reply).
void antigravity_subagent_rests() {
    QTemporaryDir root;
    const auto parent =
        root.filePath(QStringLiteral("brain/parent/.system_generated/logs/transcript_full.jsonl"));
    write_lines(parent, all(kAgyWork, 3));
    const auto child =
        root.filePath(QStringLiteral("brain/conv-b/.system_generated/logs/transcript_full.jsonl"));
    write_lines(child, all(kAgyChild, 3));
    TranscriptModel model;
    model.setNow(
        QDateTime::fromString(QStringLiteral("2026-11-11T16:00:30.000Z"), Qt::ISODateWithMs)
            .toMSecsSinceEpoch());
    model.setPath(parent);
    model.loadNow();
    require(model.running().size() == 2, "agy: both subagents run");
    QFile more(child);
    require(more.open(QIODevice::Append), "agy: the child appends");
    more.write(QByteArray(kAgyChild[3]) + '\n');
    more.close();
    model.tick();
    require(model.running().size() == 1, "agy: one at rest has ended");
}
// Thoughts as recorded: agy's stock opening dropped (a thought of only it
// has no text), blank-line runs one paragraph break, for agy and Codex.
void thoughts_are_clean() {
    AntigravityReader agy;
    for (
        const char* line : {
            R"({"step_index":0,"type":"USER_INPUT","created_at":"2026-11-11T16:00:00Z","content":"<USER_REQUEST>\nGo\n</USER_REQUEST>"})",
            R"({"step_index":1,"type":"PLANNER_RESPONSE","created_at":"2026-11-11T16:00:01Z","thinking":"I'm thinking through how to approach this.\n\n\n\n**Plan**\n\n\n\nFirst look.\n  \n\nThen fix.\n\n"})",
            R"({"step_index":2,"type":"PLANNER_RESPONSE","created_at":"2026-11-11T16:00:02Z","thinking":"I'm thinking through how to approach this."})",
        })
        (void)agy.add(line);
    const auto& items = agy.items();
    require(items.size() == 3 && items[1].kind == Kind::thinking &&
                items[1].body == "**Plan**\n\nFirst look.\n\nThen fix.",
            "agy: the stock opening goes, blank lines collapse");
    require(items.size() == 3 && items[2].kind == Kind::thinking && items[2].body.isEmpty() &&
                items[2].title == "Thought",
            "agy: a thought of only the stock line is a step with no text");
    CodexReader codex;
    (void)codex.add(
        R"({"timestamp":"2026-11-11T16:00:00.000Z","type":"event_msg","payload":{"type":"item_completed","started_at_ms":1794412800000,"completed_at_ms":1794412806000,"item":{"type":"Reasoning","id":"r","summary_text":["**Plan**\n\n\n\nRead first."]}}})");
    require(codex.items().size() == 1 && codex.items()[0].body == "**Plan**\n\nRead first." &&
                codex.items()[0].duration_ms == 6000,
            "codex: a summary's blank lines collapse, its time kept");
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    formats_are_told_apart();
    codex_items();
    codex_bad_lines_are_ignored();
    codex_file_is_followed();
    antigravity_items();
    antigravity_bad_lines_are_ignored();
    antigravity_file_is_followed();
    antigravity_steering();
    codex_subagents_and_background();
    antigravity_subagents_and_background();
    antigravity_subagent_rests();
    thoughts_are_clean();
    live_box_grows(kCodex, QStringLiteral("rollout-2026-11-10T10-00-00-live.jsonl"),
                   QStringLiteral("Also check the footer"));
    live_box_grows(kAntigravity, QStringLiteral("logs/transcript_full.jsonl"), {});
    if (failures == 0)
        std::puts("transcript_clis_test: PASS");
    return failures == 0 ? 0 : 1;
}
