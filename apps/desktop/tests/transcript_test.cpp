// The Claude transcript reader and its model: conversation records become
// items, tool results attach to their calls, bookkeeping is skipped, unknown
// blocks stay visible, and the model follows a file the CLI is still writing.
#include "code_highlight.hpp"
#include "slash_commands.hpp"
#include "transcript.hpp"
#include "transcript_math.hpp"

#include <QColor>
#include <QDir>

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

using lapis::desktop::TranscriptModel;
using lapis::desktop::transcript::ClaudeReader;
using lapis::desktop::transcript::Kind;

namespace {
int failures = 0;
void require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const char* const kConversation[] = {
    R"({"type":"permission-mode","permissionMode":"default"})",
    R"({"type":"user","message":{"role":"user","content":"Fix the build"}})",
    R"({"type":"user","isMeta":true,"message":{"role":"user","content":"<local-command-caveat>x</local-command-caveat>"}})",
    R"({"type":"assistant","thinkingDurationMs":12000,"message":{"content":[{"type":"thinking","thinking":"Look at CMake first.\nThen fix."}]}})",
    R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"cmake --build build","description":"Build"}}]}})",
    R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t2","name":"Edit","input":{"file_path":"/x/a.cpp","old_string":"int a;","new_string":"int a{};\nint b{};"}}]}})",
    R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","is_error":true,"content":"\u001b[31merror\u001b[0m: one\ntwo"}]}})",
    R"({"type":"assistant","isSidechain":true,"message":{"content":[{"type":"text","text":"subagent"}]}})",
    R"({"type":"assistant","message":{"content":[{"type":"server_tool_use","id":"s1","name":"web"}]}})",
    R"({"type":"system","subtype":"compact_boundary","content":"Conversation compacted"})",
    R"({"type":"user","message":{"content":"<command-name>/compact</command-name><command-args></command-args>"}})",
    R"({"type":"user","message":{"content":"<task-notification><summary>Build finished</summary></task-notification>"}})",
    R"({"type":"assistant","message":{"content":[{"type":"text","text":"Done: **fixed**."}]}})",
    R"({"type":"system","subtype":"turn_duration","durationMs":192000})",
};

void reader_folds_records() {
    ClaudeReader reader;
    for (const char* line : kConversation)
        (void)reader.add(line);
    const auto& items = reader.items();
    require(items.size() == 10, "ten items");
    if (items.size() != 10)
        return;
    require(items[0].kind == Kind::prompt && items[0].body == "Fix the build", "prompt");
    require(items[1].kind == Kind::thinking && items[1].body.startsWith("Look") &&
                items[1].title == "Thought for 12s",
            "thinking with its duration");
    require(items[2].kind == Kind::tool && items[2].title == "Ran" &&
                items[2].body == "cmake --build build" && items[2].detail == "Build",
            "bash step");
    require(items[2].after_step && !items[1].after_step, "steps after steps draw tight");
    require(items[2].answered && items[2].failed && items[2].result == "error: one\ntwo" &&
                items[2].result_lines == 2,
            "result attached, escapes stripped");
    require(items[3].title == "Edited" && items[3].diff.size() == 3 &&
                items[3].diff[0].sign == '-' && items[3].diff[2].text == "int b{};" &&
                items[3].added == 2 && items[3].removed == 1,
            "edit diff and counts");
    require(!items[3].answered, "edit still waiting");
    require(items[4].kind == Kind::other && items[4].title == "server_tool_use",
            "unknown block kept");
    require(items[5].kind == Kind::divider, "compaction divider");
    require(items[6].kind == Kind::command && items[6].title == "/compact", "slash command");
    require(items[7].kind == Kind::notice && items[7].title == "Build finished", "notification");
    require(items[8].kind == Kind::reply && items[8].body == "Done: **fixed**.", "reply");
    require(items[9].kind == Kind::turn && items[9].title == "Worked for 3m 12s", "turn length");
}

void result_reports_changed_card() {
    ClaudeReader reader;
    (void)reader.add(kConversation[4]);
    (void)reader.add(kConversation[5]);
    require(reader.add(kConversation[6]) == 0, "a result reports the card it changed");
    require(reader.add("{not json") == -1, "bad line ignored");
    require(reader.add(kConversation[0]) == -1, "bookkeeping changes nothing");
}

// The bar's numbers follow what changed them, not just the next answer:
// compaction resets the context to the summary, /model and /effort apply now.
void status_follows_commands() {
    ClaudeReader reader;
    (void)reader.add(
        R"({"type":"assistant","effort":"high","message":{"model":"claude-opus-5-5","usage":{"input_tokens":1,"cache_read_input_tokens":380000},"content":[{"type":"text","text":"hi"}]}})");
    require(reader.status().context_tokens == 380001, "context from the answer");
    (void)reader.add(
        R"({"type":"system","subtype":"compact_boundary","compactMetadata":{"preTokens":381210,"postTokens":15513}})");
    require(reader.status().context_tokens == 15513, "compaction leaves the summary's size");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<local-command-stdout>Set model to \u001b[1mFable 5.1\u001b[22m and saved as your default for new sessions</local-command-stdout>"}})");
    require(reader.status().model == "claude-fable-5-1", "a /model change applies at once");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<local-command-stdout>Set model to `Opus 5.5 (1M context) (default)` and saved as your default</local-command-stdout>"}})");
    require(reader.status().model == "claude-opus-5-5[1m]", "a display name with its window");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<local-command-stdout>Kept model as `Opus 5.5`</local-command-stdout>"}})");
    require(reader.status().model == "claude-opus-5-5[1m]", "a kept model changes nothing");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<local-command-stdout>Set effort level to xhigh (this session only): Deeper</local-command-stdout>"}})");
    require(reader.status().effort == "xhigh", "an /effort change applies at once");
    require(reader.items().size() == 2, "command output adds no items");
}

// A slash command shows what it printed, escapes stripped; the same record
// shapes arrive as system local_command records around a dialog.
void command_output_shows() {
    ClaudeReader reader;
    (void)reader.add(
        R"({"type":"user","message":{"content":"<command-name>/model</command-name>\n <command-message>model</command-message>\n <command-args></command-args>"}})");
    require(
        reader.add(
            R"({"type":"user","message":{"content":"<local-command-stdout>Set model to `Fable 5.2` and saved as your default for new sessions</local-command-stdout>"}})") ==
            0,
        "output reports the command it changed");
    require(reader.items().size() == 1 && reader.items()[0].kind == Kind::command &&
                reader.items()[0].title == "/model" &&
                reader.items()[0].result ==
                    "Set model to `Fable 5.2` and saved as your default for new sessions",
            "output attaches to its command");
    require(reader.status().model == "claude-fable-5-2", "the model change still applies");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<command-name>/compact</command-name><command-args></command-args>"}})");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<local-command-stdout>\u001b[2mCompacted (ctrl+o to see full summary)\u001b[22m\n  line two</local-command-stdout>"}})");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<local-command-stderr>warned</local-command-stderr>"}})");
    require(reader.items()[1].result ==
                    "Compacted (ctrl+o to see full summary)\n  line two\nwarned" &&
                reader.items()[1].result_lines == 3,
            "escapes stripped, many lines kept, stderr after stdout");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<command-name>/clear</command-name>"}})");
    require(
        reader.add(
            R"({"type":"user","message":{"content":"<local-command-stdout></local-command-stdout>"}})") ==
                -1 &&
            reader.items()[2].result.isEmpty(),
        "empty output attaches nothing");
    (void)reader.add(R"({"type":"assistant","message":{"content":[{"type":"text","text":"hi"}]}})");
    require(
        reader.add(
            R"({"type":"user","message":{"content":"<local-command-stdout>late</local-command-stdout>"}})") ==
            -1,
        "output after other items attaches nowhere");
    const QString huge = QString(9000, QLatin1Char('x'));
    (void)reader.add(
        R"({"type":"user","message":{"content":"<command-name>/context</command-name>"}})");
    (void)reader.add(
        QStringLiteral(
            R"({"type":"user","message":{"content":"<local-command-stdout>%1</local-command-stdout>"}})")
            .arg(huge)
            .toUtf8());
    require(reader.items().back().result.size() <=
                lapis::desktop::transcript::result_preview_chars + 1,
            "long output is clipped");

    ClaudeReader system;
    (void)system.add(
        R"({"type":"system","subtype":"local_command","content":"<command-name>/model</command-name>\n <command-message>model</command-message>\n <command-args>opus</command-args>"})");
    require(
        system.add(
            R"({"type":"system","subtype":"local_command","content":"<local-command-stdout>Kept model as `Fable 5.2`</local-command-stdout>"})") ==
            0,
        "system command output reports its command");
    require(system.items().size() == 1 && system.items()[0].kind == Kind::command &&
                system.items()[0].title == "/model" && system.items()[0].body == "opus" &&
                system.items()[0].result == "Kept model as `Fable 5.2`",
            "a system local_command record reads like a user one");
    (void)system.add(
        R"({"type":"system","subtype":"local_command","content":"<command-name>/effort</command-name>"})");
    (void)system.add(
        R"({"type":"system","subtype":"local_command","content":"<local-command-stdout>Set effort level to max (this session only)</local-command-stdout>"})");
    require(system.status().effort == "max", "system command output sets the status too");

    TranscriptModel model;
    const auto roles = model.roleNames();
    require(roles.value(TranscriptModel::ResultRole) == "result",
            "command output is the result role");
}

// Bash mode: "!" commands with their output, never a turn of their own.
void bash_mode_commands() {
    ClaudeReader reader;
    (void)reader.add(R"({"type":"user","message":{"content":"<bash-input>open .</bash-input>"}})");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<bash-stdout>(Bash completed with no output)</bash-stdout><bash-stderr></bash-stderr>"}})");
    (void)reader.add(R"({"type":"user","message":{"content":"<bash-input>ls nope</bash-input>"}})");
    (void)reader.add(
        R"({"type":"user","message":{"content":"<bash-stdout>a</bash-stdout><bash-stderr>ls: nope: No such file</bash-stderr>"}})");
    const auto& items = reader.items();
    require(items.size() == 2 && items[0].kind == Kind::command && items[0].title == "!" &&
                items[0].body == "open ." && items[0].result == "(Bash completed with no output)" &&
                !items[0].starts_turn,
            "bash input is a ! command with its output");
    require(items[1].result == "a\nls: nope: No such file" && !items[1].starts_turn,
            "stdout then stderr");
    require(lapis::desktop::transcript::lay_out(items, {}).turns.empty(),
            "bash mode starts no turn");
}

// System notices once dropped, interruptions, and attachments sent with text.
void notices_and_attachments() {
    // A background task's wake-up starts a turn but has no row of its own.
    ClaudeReader woken;
    (void)woken.add(
        R"({"type":"user","message":{"content":"<task-notification><summary>Agent \"x\" finished</summary></task-notification>"}})");
    (void)woken.add(
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"w1","name":"Bash","input":{"command":"ls"}}]}})");
    (void)woken.add(
        R"({"type":"assistant","message":{"content":[{"type":"text","text":"Done."}]}})");
    (void)woken.add(R"({"type":"system","subtype":"turn_duration","durationMs":5000})");
    const auto woke = lapis::desktop::transcript::lay_out(woken.items(), {});
    require(woke.turns.size() == 1 && woke.rows.size() == 2 && woke.rows[0].item == -1 &&
                woke.rows[1].item == 2,
            "a woken turn folds to its work line and reply, with no notice");
    ClaudeReader reader;
    (void)reader.add(
        R"({"type":"system","subtype":"informational","content":"Compacted while idle · turn off with x"})");
    (void)reader.add(
        R"({"type":"system","subtype":"model_consent_fallback","content":"Switched to Sonnet 5.5 for this session"})");
    (void)reader.add(R"({"type":"system","subtype":"informational","content":""})");
    (void)reader.add(
        R"({"type":"system","subtype":"scheduled_task_fire","content":"Claude resuming /loop wakeup"})");
    (void)reader.add(R"({"type":"system","subtype":"model_refusal","content":"Refused"})");
    (void)reader.add(
        R"({"type":"user","message":{"content":[{"type":"text","text":"[Request interrupted by user for tool use]"}]}})");
    (void)reader.add(R"({"type":"user","message":{"content":"[Request interrupted by user]"}})");
    const auto& items = reader.items();
    require(items.size() == 6, "six notices, the empty one skipped");
    if (items.size() != 6)
        return;
    require(items[0].kind == Kind::notice && items[0].title.startsWith("Compacted while idle") &&
                !items[0].starts_turn,
            "informational notice");
    require(items[1].kind == Kind::notice && items[1].title.startsWith("Switched to Sonnet") &&
                !items[1].starts_turn,
            "model fallback notice");
    require(items[2].kind == Kind::notice && items[2].title == "Claude resuming /loop wakeup" &&
                items[2].starts_turn,
            "a scheduled wakeup starts a turn");
    require(items[3].failed && items[3].title == "Refused", "refusals still show");
    require(items[4].kind == Kind::notice && items[4].title == "Interrupted" &&
                !items[4].starts_turn && items[5].title == "Interrupted" && !items[5].starts_turn,
            "an interruption is a notice, not a prompt");

    ClaudeReader images;
    (void)images.add(
        R"({"type":"user","uuid":"u1","message":{"content":[{"type":"image","source":{}},{"type":"text","text":"[Image #2] is this good?"}]}})");
    require(images.items().size() == 1 && images.items()[0].body == "[Image #2] is this good?",
            "text sent with an image is the only prompt");
    (void)images.add(
        R"({"type":"user","uuid":"u2","message":{"content":[{"type":"image","source":{}},{"type":"document","source":{}}]}})");
    require(images.items().size() == 3 && images.items()[1].body == "[image]" &&
                images.items()[2].body == "[document]" && images.items()[2].kind == Kind::prompt,
            "an attachment alone stands as the prompt, documents too");
    require(lapis::desktop::transcript::lay_out(images.items(), {}).turns.size() == 2,
            "one turn per record");
}

// Claude Code's input box against a dialog drawn in its place.
void dialogs_are_seen() {
    using lapis::desktop::transcript::dialog_showing;
    const QString rule(66, QChar(0x2500));
    const QStringList idle{QStringLiteral("  Switch with /model."),
                           QString(),
                           rule,
                           QStringLiteral("❯ "),
                           rule,
                           QStringLiteral("  Haiku 5.5 · …/folder        ◐ medium · /effort"),
                           QStringLiteral("  ctx -- · eff medium · 0s   "),
                           QStringLiteral("  ⏵⏵ auto mode on (shift+tab to cycle)")};
    require(!dialog_showing(idle), "an idle input box is no dialog");
    const QStringList draft{QStringLiteral("✻ Working… (esc to interrupt)"),
                            QString(),
                            rule + QStringLiteral("   "),
                            QStringLiteral("! git status"),
                            QStringLiteral("  and more"),
                            rule,
                            QStringLiteral("  ⏵⏵ auto mode on")};
    require(!dialog_showing(draft), "a running turn with a multi-line draft is no dialog");
    require(!dialog_showing({QStringLiteral("$ ls"), QStringLiteral("a b")}), "no rule: no dialog");
    const QStringList picker{QStringLiteral("❯ /model"),
                             QString(),
                             rule,
                             QStringLiteral("  Select model"),
                             QStringLiteral("    1. Default (recommended)  Sonnet 5.5"),
                             QStringLiteral("  ❯ 5. Haiku ✔                Haiku 5.5"),
                             QString(),
                             QStringLiteral("  Enter to set as default · Esc to cancel")};
    require(dialog_showing(picker), "a model picker is a dialog");
    const QStringList search{
        QStringLiteral("❯ /resume"),
        QString(),
        rule,
        QStringLiteral("  Resume session"),
        QStringLiteral("  ╭") + QString(40, QChar(0x2500)) + QStringLiteral("╮"),
        QStringLiteral("  │ ⌕ Search…  │"),
        QStringLiteral("  ╰") + QString(40, QChar(0x2500)) + QStringLiteral("╯"),
        QStringLiteral("    Ctrl+A to show all projects · Esc to cancel")};
    require(dialog_showing(search), "a search with a boxed field is a dialog");
    const QStringList effort{rule, QStringLiteral("  Effort"),
                             QStringLiteral("                        ") +
                                 QString(10, QChar(0x2500)) + QStringLiteral("▲") +
                                 QString(30, QChar(0x2500)),
                             QStringLiteral("                        low     medium     high"),
                             QStringLiteral("  ←/→ to adjust · Esc to cancel")};
    require(dialog_showing(effort), "an effort slider is a dialog");
    QStringList settings{rule, QStringLiteral("  Settings")};
    for (int i = 0; i < 20; ++i)
        settings << QStringLiteral("  Option %1   on").arg(i);
    settings << QStringLiteral("  Esc to close");
    require(dialog_showing(settings), "a long settings list is a dialog");
    // A rule above a dialog that does not open on a prompt row is no input box.
    const QStringList theme{rule,
                            QStringLiteral("  Theme"),
                            QStringLiteral("  ❯ ✔ Dark mode"),
                            rule,
                            QStringLiteral("  ") + QString(30, QChar(0x254C)),
                            QStringLiteral("  1 function x() {}")};
    require(dialog_showing(theme), "two rules around a non-prompt row are a dialog");
}

// A prompt sent while the agent works shows at once, queued; the running
// turn taking it in keeps it in place, and one sent on as the next turn is
// shown once, as that turn's prompt.
void queued_prompts_show() {
    ClaudeReader reader;
    const char* const lines[] = {
        R"({"type":"user","uuid":"u1","message":{"content":"Build it"}})",
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make"}}]}})",
        R"({"type":"queue-operation","operation":"enqueue","content":"<task-notification>x</task-notification>"})",
        R"({"type":"queue-operation","operation":"enqueue","content":"also add tests"})",
        R"({"type":"queue-operation","operation":"enqueue","content":"and docs"})",
    };
    for (const char* line : lines)
        (void)reader.add(line);
    require(reader.items().size() == 4 && reader.items()[2].queued &&
                reader.items()[2].body == "also add tests" && !reader.items()[2].starts_turn,
            "a prompt sent mid-turn shows at once, queued, inside the turn");
    // The task notice leaves first (a dequeue names no text), then the turn
    // takes in "also add tests"; "and docs" goes on as the next turn.
    (void)reader.add(R"({"type":"queue-operation","operation":"dequeue"})");
    require(reader.items()[2].queued, "a dequeue takes the oldest entry, the hidden task notice");
    // Two more steps before the turn reads it.
    (void)reader.add(
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t2","name":"Read","input":{"file_path":"/x/a.cpp"}}]}})");
    (void)reader.add(
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t3","name":"Bash","input":{"command":"ctest"}}]}})");
    (void)reader.add(
        R"({"type":"queue-operation","operation":"remove","content":"also add tests","reason":"absorbed_mid_turn"})");
    (void)reader.add(
        R"({"type":"attachment","attachment":{"type":"queued_command","prompt":"also add tests","commandMode":"prompt"}})");
    const auto& items = reader.items();
    require(items.size() == 7 && items[2].dropped && !items[2].queued && items[6].steered &&
                items[6].body == "also add tests" && items[6].waited == 2 &&
                !items[6].starts_turn && !items[6].queued,
            "read by the running turn: it moves to where it was read, after 2 more steps");
    // Still running: the prompt, a "Worked for" row for the steps before
    // the steering prompt, the steering prompt, the live box, then the
    // prompt still queued.
    using lapis::desktop::transcript::lay_out;
    const auto running = lay_out(items, {}, true);
    const auto shape = [&items](const lapis::desktop::transcript::Layout& layout) {
        QStringList out;
        for (const auto& row : layout.rows)
            out << (row.live       ? QStringLiteral("live")
                    : row.item < 0 ? QStringLiteral("work")
                                   : items[static_cast<size_t>(row.item)].body);
        return out.join(QLatin1Char('|'));
    };
    require(shape(running) == "Build it|work|also add tests|live|and docs",
            "running: prompt, work so far, steering prompt, live box, queued prompt last");
    require(running.turns.size() == 1 && running.turns[0].parts.size() == 2 &&
                running.turns[0].parts[0].steps == 3 && running.turns[0].parts[1].steps == 0,
            "the steering prompt splits the turn's work in two parts");
    (void)reader.add(R"({"type":"queue-operation","operation":"dequeue"})");
    (void)reader.add(R"({"type":"user","uuid":"u2","message":{"content":"and docs"}})");
    require(items[3].dropped, "sent on as a turn: its queued copy goes");
    const auto layout = lay_out(items, {});
    int docs = 0;
    for (const auto& row : layout.rows)
        if (row.item >= 0 && items[static_cast<size_t>(row.item)].body == "and docs")
            ++docs;
    require(docs == 1 && layout.turns.size() == 2, "shown once, as the next turn's prompt");
    // Folded, each prompt shows once: the steered turn keeps its steering
    // prompt after its work, and the next turn folds on its own.
    (void)reader.add(
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t4","name":"Edit","input":{"file_path":"/x/t.cpp","old_string":"a","new_string":"b"}}]}})");
    (void)reader.add(
        R"({"type":"assistant","message":{"content":[{"type":"text","text":"done"}]}})");
    (void)reader.add(R"({"type":"system","subtype":"turn_duration","durationMs":90061000})");
    const auto folded = lay_out(items, {});
    require(shape(folded) == "Build it|work|also add tests|and docs|work|done",
            "folded turns show every prompt once, in order");
    require(lapis::desktop::transcript::duration_text(90061000) == "1d 1h", "days for long turns");
}

// Several prompts queued while the agent works keep their order, below the
// live box, and each moves into the thread where the turn reads it.
void several_queued_prompts() {
    ClaudeReader reader;
    const char* const lines[] = {
        R"({"type":"user","uuid":"u1","message":{"content":"Go"}})",
        R"({"type":"queue-operation","operation":"enqueue","content":"first"})",
        R"({"type":"queue-operation","operation":"enqueue","content":"second"})",
        R"({"type":"queue-operation","operation":"enqueue","content":"third"})",
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make"}}]}})",
        R"({"type":"queue-operation","operation":"remove","content":"first","reason":"absorbed_mid_turn"})",
    };
    for (const char* line : lines)
        (void)reader.add(line);
    const auto& items = reader.items();
    const auto layout = lapis::desktop::transcript::lay_out(items, {}, true);
    QStringList shown;
    for (const auto& row : layout.rows)
        shown << (row.live       ? QStringLiteral("live")
                  : row.item < 0 ? QStringLiteral("work")
                                 : items[static_cast<size_t>(row.item)].body +
                                       (items[static_cast<size_t>(row.item)].queued ? "?" : ""));
    require(shown.join('|') == "Go|work|first|live|second?|third?",
            "queued prompts keep their order under the live box");
    require(items.back().waited == 1, "the first was read after one step");
}

// Command-click finds the path under the pointer, and the live tail cuts the
// screen at the agent's input box.
void paths_and_live_lines() {
    using lapis::desktop::transcript::live_lines;
    using lapis::desktop::transcript::path_at;
    QTemporaryDir directory;
    QDir(directory.path()).mkpath(QStringLiteral("src"));
    QFile file(directory.filePath(QStringLiteral("src/a.cpp")));
    require(file.open(QIODevice::WriteOnly), "path fixture");
    file.close();
    const auto absolute = QDir::cleanPath(QFileInfo(file.fileName()).absoluteFilePath());
    const auto text = QStringLiteral("Fixed `src/a.cpp:42` and see %1, then run it.").arg(absolute);
    require(path_at(text, 9, directory.path()) == absolute + ":42",
            "relative path with its line, in backticks");
    require(path_at(text, static_cast<int>(text.indexOf(absolute)) + 3, {}) == absolute,
            "absolute path, trailing comma dropped");
    require(path_at(text, 2, directory.path()).isEmpty(), "a plain word is not a path");
    require(path_at(QStringLiteral("see src/missing.cpp"), 6, directory.path()).isEmpty(),
            "missing files are not offered");

    const QStringList screen{
        QStringLiteral("⏺ Reading the build files"),
        QStringLiteral("  ⎿  Read CMakeLists.txt"),
        QStringLiteral(""),
        QStringLiteral("✻ Thinking… (esc to interrupt)"),
        QStringLiteral("────────────────────────────────────────"),
        QStringLiteral("❯ "),
        QStringLiteral("────────────────────────────────────────"),
        QStringLiteral("  ⏵⏵ bypass permissions on"),
    };
    const auto live = live_lines(screen);
    require(live.size() == 4 && live.front().startsWith(QStringLiteral("⏺")) &&
                live.back().startsWith(QStringLiteral("✻")),
            "the screen above the input box, the status line kept");

    // The screen still shows the turns before this one: a finished reply
    // with a table, its steps, the prompt that started this turn.
    const QString rule(60, QChar(0x2500));
    const QStringList earlier{
        QStringLiteral("● Bash(make check)"),
        QStringLiteral("  ⎿  ok"),
        QStringLiteral("● The fix works: builds are faster."),
        QStringLiteral("  ┌───────┬────────┐"),
        QStringLiteral("  │ Model │ Before │"),
        QStringLiteral("  └───────┴────────┘"),
        QStringLiteral("  TL;DR: faster."),
        QStringLiteral("✻ Waiting for 3 background agents to finish"),
    };
    const QStringList box{rule, QStringLiteral("❯ "), rule, QStringLiteral("  ⏵⏵ auto mode")};
    const auto compacting = earlier +
                            QStringList{QStringLiteral("❯ /compact"), QString(),
                                        QStringLiteral("✻ Compacting conversation… "
                                                       "(2m 16s · ↓ 9.0k tokens)")} +
                            box;
    require(lapis::desktop::transcript::compacting_shown(compacting) &&
                live_lines(compacting).isEmpty(),
            "while compacting the tail is empty, never the screen");
    require(!lapis::desktop::transcript::compacting_shown(screen), "thinking is not compacting");
    const auto running = earlier +
                         QStringList{QStringLiteral("❯ run the tests"),
                                     QString(),
                                     QStringLiteral("● Running the tests now."),
                                     QStringLiteral("● Bash(ctest)"),
                                     QStringLiteral("  ⎿  Running…"),
                                     QStringLiteral("✶ Working… (12s)")} +
                         box;
    const auto tail = live_lines(running);
    require(tail.size() == 3 && tail.front() == QStringLiteral("● Bash(ctest)") &&
                !tail.join(QLatin1Char('\n')).contains(QStringLiteral("Before")),
            "only the step in progress after this turn's prompt, no earlier reply");
    // A turn woken by a background task has no prompt on screen; the reply
    // the thread already shows is not repeated, only the status under it.
    const auto woken = earlier + QStringList{QStringLiteral("✻ Working… (3s)")} + box;
    const auto quiet = live_lines(woken, {QStringLiteral("The **fix** works: builds are faster.")});
    require(quiet == QStringList{QStringLiteral("✻ Working… (3s)")},
            "a reply already in the thread stays out of the tail");
}

// Tool output reads as the terminal shows it: no notes Claude Code appends
// for the model, no escape sequences of any kind, no binary control bytes.
void results_are_clean() {
    ClaudeReader reader;
    for (
        const char* line : {
            R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"r1","name":"Read","input":{"file_path":"/x/a.txt"}},{"type":"tool_use","id":"r2","name":"Bash","input":{"command":"probe"}},{"type":"tool_use","id":"r3","name":"Bash","input":{"command":"grep"}}]}})",
            R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"r1","content":"<system-reminder>Already in context.</system-reminder>"}]}})",
            R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"r2","content":"\u001b]10;?\u001b\\\u001b[>4;0mready\u0000\u0001 now\r\nok\n<system-reminder>Rate limited.</system-reminder>"}]}})",
            R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"r3","content":"a.py: \"<system-reminder>\",\nb.py: done"}]}})",
        })
        (void)reader.add(line);
    const auto& items = reader.items();
    require(items.size() == 3, "three steps");
    if (items.size() != 3)
        return;
    require(items[0].result.isEmpty(), "a result that is only a note for the model is empty");
    require(items[1].result == "ready now\nok", "escapes, controls and the appended note go");
    require(items[2].result.contains("<system-reminder>"), "a tag inside real output stays");
    (void)reader.add(
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"r4","name":"Read","input":{"file_path":"/x/note.md"}}]}})");
    (void)reader.add(
        R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"r4","content":"<system-reminder>This file is 3 days old.</system-reminder>\n1\t---\n2\tname: note"}]}})");
    require(items.size() == 4 && items[3].result == "1\t---\n2\tname: note",
            "a note before the output goes too");

    // A pasted image: the prompt names it, then a note gives where it was saved.
    ClaudeReader pasted;
    (void)pasted.add(
        R"({"type":"user","message":{"content":[{"type":"text","text":"[Image #1] is this right?"},{"type":"image","source":{"type":"base64","media_type":"image/png","data":"AAAA"}}]}})");
    const int changed = pasted.add(
        R"({"type":"user","isMeta":true,"message":{"content":[{"type":"text","text":"[Image: source: /tmp/shots/1.png]"}]}})");
    require(pasted.items().size() == 1 && changed == 0 &&
                pasted.items()[0].detail == QStringLiteral("/tmp/shots/1.png"),
            "a pasted image's saved path joins its prompt");

    // Pasted text: the prompt shows it without Claude Code's tags, and a
    // queued prompt that starts with a paste is still the person's.
    ClaudeReader text;
    (void)text.add(
        R"({"type":"user","message":{"content":"Look at this:\n<pasted_content id=\"ab12\">\nline one\nline two\n</pasted_content>\nwhy?"}})");
    (void)text.add(
        R"({"type":"queue-operation","operation":"enqueue","content":"<pasted_content id=\"cd34\">\nerror: x\n</pasted_content> fix it"})");
    require(text.items().size() == 2 &&
                text.items()[0].body == QStringLiteral("Look at this:\nline one\nline two\nwhy?") &&
                text.items()[1].queued && text.items()[1].body == QStringLiteral("error: x fix it"),
            "pastes show as their text, queued ones too");
}

// Claude Code writes a /compact as the person's text when it starts and its
// command record (with the output) after the compaction: one command row
// between turns, no turn of its own, and "compacting" while it runs.
void compact_runs_between_turns() {
    const char* const lines[] = {
        R"({"type":"user","uuid":"a1","timestamp":"2026-01-01T10:00:00.000Z","message":{"content":"Build it"}})",
        R"({"type":"assistant","uuid":"a2","timestamp":"2026-01-01T10:00:05.000Z","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make"}}]}})",
        R"({"type":"assistant","uuid":"a3","timestamp":"2026-01-01T10:00:09.000Z","message":{"content":[{"type":"text","text":"Built."}]}})",
        R"({"type":"user","uuid":"a4","timestamp":"2026-01-01T10:05:00.000Z","message":{"content":"/compact"}})",
    };
    const char* const after[] = {
        R"({"type":"system","subtype":"compact_boundary","uuid":"a5","timestamp":"2026-01-01T10:07:00.000Z","compactMetadata":{"postTokens":9000}})",
        R"({"type":"user","uuid":"a6","isCompactSummary":true,"timestamp":"2026-01-01T10:07:00.000Z","message":{"content":"This session is being continued from a previous conversation."}})",
        R"({"type":"user","uuid":"a7","isMeta":true,"timestamp":"2026-01-01T10:05:00.000Z","message":{"content":"<local-command-caveat>x</local-command-caveat>"}})",
        R"({"type":"user","uuid":"a8","timestamp":"2026-01-01T10:05:00.000Z","message":{"content":"<command-name>/compact</command-name><command-args></command-args>"}})",
        R"({"type":"user","uuid":"a9","timestamp":"2026-01-01T10:07:00.000Z","message":{"content":"<local-command-stdout>\u001b[2mCompacted\u001b[22m</local-command-stdout>"}})",
        R"({"type":"user","uuid":"b1","timestamp":"2026-01-01T10:08:00.000Z","message":{"content":"Next"}})",
    };
    ClaudeReader reader;
    for (const char* line : lines)
        (void)reader.add(line);
    const auto& items = reader.items();
    require(items.back().kind == Kind::command && items.back().title == "/compact",
            "a started /compact is a command, not a prompt");
    require(reader.status().compacting_since > 0, "compacting while it runs");
    auto layout = lapis::desktop::transcript::lay_out(items, {});
    require(layout.turns.size() == 1 && layout.turns.front().closed,
            "the compaction closes the turn before it and starts none");
    for (const char* line : after)
        (void)reader.add(line);
    require(reader.status().compacting_since == 0, "compacting ends with its records");
    int commands = 0, dividers = 0;
    for (const auto& item : items) {
        commands += item.kind == Kind::command ? 1 : 0;
        dividers += item.kind == Kind::divider ? 1 : 0;
    }
    require(commands == 1 && dividers == 1, "one /compact row and one divider");
    require(items[3].kind == Kind::command && items[3].result == "Compacted",
            "its output joins the row it started");
    layout = lapis::desktop::transcript::lay_out(items, {});
    require(layout.turns.size() == 2 && layout.turns.back().start == 5,
            "the next prompt starts the next turn");
}

void model_follows_file() {
    QTemporaryDir dir;
    const auto path = dir.filePath("s.jsonl");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "write fixture");
    file.write(QByteArray(kConversation[1]) + '\n' + kConversation[4] + '\n');
    file.flush();
    TranscriptModel model;
    model.setPath(path);
    model.loadNow();
    require(model.rowCount() == 2, "loaded two items");
    int changed = 0, inserted = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&changed] { ++changed; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&inserted] { ++inserted; });
    // The CLI writes the result in two pieces; only a whole line counts.
    const QByteArray result = QByteArray(kConversation[6]) + '\n';
    file.write(result.left(20));
    file.flush();
    model.readMore();
    require(changed == 0 && inserted == 0, "half a line waits");
    file.write(result.mid(20) + kConversation[12] + '\n');
    file.flush();
    model.readMore();
    require(changed == 1 && inserted == 1 && model.rowCount() == 3,
            "result updates the card, reply appends");
    require(model.items()[1].failed, "card shows failure");
    file.resize(0);
    file.seek(0);
    file.write(QByteArray(kConversation[12]) + '\n');
    file.flush();
    model.readMore();
    require(model.rowCount() == 1, "a rewritten file reloads");
}
// A reply splits at fenced code so each block can be copied whole; an
// unclosed fence (a reply still streaming) still reads as code.
void reply_splits_at_code() {
    using lapis::desktop::transcript::pieces;
    const auto parts =
        pieces(QStringLiteral("Intro **bold**\n\n```cpp\nint a;\n\nint b;\n```\nAfter\n~~~~\nx\n"));
    require(parts.size() == 4, "prose, code, prose, code");
    require(!parts[0].code && parts[0].text == QStringLiteral("Intro **bold**"),
            "prose kept as markdown");
    require(parts[1].code && parts[1].language == QStringLiteral("cpp") &&
                parts[1].text == QStringLiteral("int a;\n\nint b;"),
            "code keeps its blank line and language");
    require(!parts[2].code && parts[2].text == QStringLiteral("After"), "prose after code");
    require(parts[3].code && parts[3].text == QStringLiteral("x\n"), "unclosed fence is code");
    require(pieces(QStringLiteral("  \n")).empty(), "blank reply has no pieces");
}

// Each item names the record it came from, so a fork can cut there.
void items_keep_their_record() {
    ClaudeReader reader;
    reader.add(R"({"type":"user","uuid":"u-1","message":{"content":"Hi"}})");
    reader.add(
        R"({"type":"assistant","uuid":"a-1","message":{"content":[{"type":"text","text":"Hello"}]}})");
    require(reader.items().size() == 2 && reader.items()[0].record == QStringLiteral("u-1") &&
                reader.items()[1].record == QStringLiteral("a-1"),
            "items carry their record uuid");
}
// Copied text pastes cleanly: Qt's separators become newlines, invisible
// characters go, trailing spaces and blank edges are trimmed.
void copies_are_clean() {
    using lapis::desktop::transcript::clean_copy;
    const QString raw = QStringLiteral("\n  One two   Three​ - four\t\r\nfive￼\n\n");
    require(clean_copy(raw) == QStringLiteral("  One two\nThree\n- four\nfive"),
            "copy text is clean");

    // A selection across a laid-out reply keeps its shape as plain text.
    QTextDocument document;
    document.setMarkdown(QStringLiteral(
        "## Plan\n\nRun `make` first.\n\n- one\n- two\n\n1. first\n2. second\n\nDone."));
    const auto all =
        lapis::desktop::transcript::selection_text(document, 0, document.characterCount() - 1);
    const bool shaped =
        all ==
        QStringLiteral("Plan\n\nRun make first.\n\n- one\n- two\n\n1. first\n2. second\n\nDone.");
    if (!shaped)
        std::fprintf(stderr, "got: %s\n",
                     qPrintable(QString(all).replace(QLatin1Char('\n'), QStringLiteral("\\n"))));
    require(shaped, "selection keeps paragraphs, bullets and numbers");
    const int middle = static_cast<int>(document.toPlainText().indexOf(QStringLiteral("make")));
    require(lapis::desktop::transcript::selection_text(document, middle, middle + 4) ==
                QStringLiteral("make"),
            "part of a line copies as just that part");
}

// Switching back to a conversation shows it at once, without a reload.
void switching_back_is_warm() {
    QTemporaryDir directory;
    const auto write = [&](const char* name, int replies) {
        QFile file(directory.filePath(QString::fromLatin1(name)));
        require(file.open(QIODevice::WriteOnly), "fixture opens");
        for (int i = 0; i < replies; ++i)
            file.write(QByteArray(kConversation[12]) + '\n');
        return file.fileName();
    };
    const auto a = write("a.jsonl", 3);
    const auto b = write("b.jsonl", 1);
    TranscriptModel model;
    model.setPath(a);
    model.loadNow();
    model.setPath(b);
    model.loadNow();
    require(model.rowCount() == 1, "second conversation shown");
    model.setPath(a);
    require(model.rowCount() == 3 && !model.loading(), "first conversation is back at once");
    model.setPath({});
    require(model.rowCount() == 0, "no conversation shows nothing");
}
// A finished turn folds to prompt, "Worked for", final reply; opening it
// shows the work between; a turn still running shows everything.
void turns_fold() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("t.jsonl"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "turn fixture opens");
    file.write(
        R"({"type":"user","uuid":"p1","timestamp":"2026-11-07T10:00:00.000Z","message":{"content":"Fix it"}})"
        "\n"
        R"({"type":"assistant","uuid":"a1","effort":"high","message":{"model":"claude-opus-5-5","usage":{"input_tokens":10,"cache_read_input_tokens":250000,"cache_creation_input_tokens":90,"output_tokens":20},"content":[{"type":"thinking","thinking":"Look"}]}})"
        "\n"
        R"({"type":"assistant","uuid":"a2","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make"}}]}})"
        "\n"
        R"({"type":"user","uuid":"r1","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"}]}})"
        "\n"
        R"({"type":"assistant","uuid":"a3","message":{"content":[{"type":"text","text":"Fixed."}]}})"
        "\n"
        R"({"type":"system","subtype":"turn_duration","durationMs":65000})"
        "\n"
        R"({"type":"user","uuid":"p2","message":{"content":"Now test"}})"
        "\n"
        R"({"type":"assistant","uuid":"a4","message":{"content":[{"type":"tool_use","id":"t2","name":"Bash","input":{"command":"ctest"}}]}})"
        "\n");
    file.flush();
    TranscriptModel model;
    model.setPath(path);
    model.loadNow();
    const auto kind = [&model](int row) {
        return model.data(model.index(row), TranscriptModel::KindRole).toString();
    };
    require(model.rowCount() == 5 && kind(0) == QStringLiteral("prompt") &&
                kind(1) == QStringLiteral("work") && kind(2) == QStringLiteral("reply") &&
                kind(3) == QStringLiteral("prompt") && kind(4) == QStringLiteral("work"),
            "finished turn folds; an open turn with the agent idle folds too");
    model.setLive(true);
    const auto& steps = *qobject_cast<lapis::desktop::LiveStepsModel*>(model.liveSteps());
    require(model.rowCount() == 5 && kind(4) == QStringLiteral("live") && steps.rowCount() == 1,
            "while the agent works, the open turn ends in the live box with its step");
    require(model.data(model.index(1), TranscriptModel::TitleRole).toString() ==
                    QStringLiteral("Worked for 1m 5s") &&
                model.data(model.index(1), TranscriptModel::StepsRole).toInt() == 2 &&
                model.data(model.index(1), TranscriptModel::DetailRole).toString() ==
                    QStringLiteral("ran a command"),
            "work row says how long and what it did");
    model.toggleWork(1);
    require(model.rowCount() == 7 && kind(2) == QStringLiteral("thinking") &&
                kind(3) == QStringLiteral("tool") && kind(4) == QStringLiteral("reply") &&
                model.data(model.index(3), TranscriptModel::InWorkRole).toBool() &&
                model.data(model.index(1), TranscriptModel::ExpandedRole).toBool(),
            "opening shows thinking and steps above the reply");
    model.toggleWork(1);
    require(model.rowCount() == 5, "folding hides them again");
    require(model.model() == QStringLiteral("claude-opus-5-5") &&
                model.effort() == QStringLiteral("high") && model.contextTokens() == 250120 &&
                model.contextWindow() == 1000000,
            "status: model, effort, context use and window");
    // The running turn finishes: it folds too, without a reload.
    int removed = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset,
                     [] { require(false, "no reset on append"); });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, [&removed] { ++removed; });
    file.write(
        R"({"type":"user","uuid":"r2","message":{"content":[{"type":"tool_result","tool_use_id":"t2","content":"pass"}]}})"
        "\n"
        R"({"type":"assistant","uuid":"a5","message":{"content":[{"type":"text","text":"All pass."}]}})"
        "\n"
        R"({"type":"system","subtype":"turn_duration","durationMs":3000})"
        "\n");
    file.flush();
    model.readMore();
    require(model.rowCount() == 6 && kind(4) == QStringLiteral("work") &&
                kind(5) == QStringLiteral("reply") && removed == 1 && steps.rowCount() == 0,
            "a turn that finishes folds in place and the live box empties");
    require(lapis::desktop::transcript::context_window(QStringLiteral("claude-opus-4-5"), 1000) ==
                    200000 &&
                lapis::desktop::transcript::context_window(QStringLiteral("claude-opus-4-5[1m]"),
                                                           0) == 1000000,
            "context windows follow the model");
}
// The live work box follows a Claude Code file as it grows: one row per
// step, appended (never reset) while the turn runs, a result redrawing its
// row; a prompt read mid-turn lands in the thread above the box; the thread
// itself gains no step rows, and an edit's diff stays out of it.
void live_box_follows_claude() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("live.jsonl"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "live fixture opens");
    const auto append = [&file](const char* line) {
        file.write(QByteArray(line) + '\n');
        file.flush();
    };
    append(
        R"({"type":"user","uuid":"p1","timestamp":"2026-11-07T10:00:00.000Z","message":{"content":"Go"}})");
    TranscriptModel model;
    model.setLive(true);
    model.setPath(path);
    model.loadNow();
    const auto& steps = *qobject_cast<lapis::desktop::LiveStepsModel*>(model.liveSteps());
    const auto kinds = [&model] {
        QStringList out;
        for (int row = 0; row < model.rowCount(); ++row)
            out << model.data(model.index(row), TranscriptModel::KindRole).toString();
        return out.join('|');
    };
    require(kinds() == "prompt|live" && steps.rowCount() == 0, "a new turn: prompt and live box");
    int resets = 0, inserted = 0, redrawn = 0;
    QObject::connect(&steps, &QAbstractItemModel::modelReset, [&resets] { ++resets; });
    QObject::connect(&steps, &QAbstractItemModel::rowsInserted, [&inserted] { ++inserted; });
    QObject::connect(&steps, &QAbstractItemModel::dataChanged, [&redrawn] { ++redrawn; });
    const char* const work[] = {
        R"({"type":"assistant","uuid":"a1","timestamp":"2026-11-07T10:00:02.000Z","message":{"content":[{"type":"thinking","thinking":"plan"}]}})",
        R"({"type":"assistant","uuid":"a2","timestamp":"2026-11-07T10:00:03.000Z","message":{"content":[{"type":"tool_use","id":"t1","name":"Edit","input":{"file_path":"/x/a.cpp","old_string":"a","new_string":"b\nc"}}]}})",
        R"({"type":"user","uuid":"r1","timestamp":"2026-11-07T10:00:04.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"}]}})",
        R"({"type":"queue-operation","operation":"enqueue","timestamp":"2026-11-07T10:00:05.000Z","content":"use tabs"})",
        R"({"type":"assistant","uuid":"a3","timestamp":"2026-11-07T10:00:06.000Z","message":{"content":[{"type":"text","text":"Building now."}]}})",
        R"({"type":"assistant","uuid":"a4","timestamp":"2026-11-07T10:00:07.000Z","message":{"content":[{"type":"tool_use","id":"t2","name":"Bash","input":{"command":"make"}}]}})",
    };
    for (const char* line : work) {
        append(line);
        model.readMore();
    }
    require(steps.rowCount() == 4 && resets == 0 && inserted == 4 && redrawn >= 1,
            "each step appends a row; a result redraws its row");
    require(kinds() == "prompt|live|prompt", "no step rows in the thread; the queued prompt below");
    const auto edit = steps.entry(1);
    require(edit.value("title").toString() == "Edited" && edit.value("added").toInt() == 2 &&
                edit.value("diff").toList().size() == 3 && edit.value("answered").toBool(),
            "the box's edit row carries its counts and diff for when it is opened");
    require(steps.rowOf(4) == 2 && model.items()[4].kind == Kind::reply,
            "the interim reply is a row of the box, found by its item");
    append(
        R"({"type":"queue-operation","operation":"remove","timestamp":"2026-11-07T10:00:20.000Z","content":"use tabs","reason":"absorbed_mid_turn"})");
    model.readMore();
    require(kinds() == "prompt|work|prompt|live" && steps.rowCount() == 0,
            "read mid-turn: the steps before fold, the prompt lands, the box starts again");
    const int steered = 2;
    require(model.data(model.index(steered), TranscriptModel::SteeredRole).toBool() &&
                model.data(model.index(steered), TranscriptModel::WaitedRole).toInt() == 1 &&
                !model.data(model.index(steered), TranscriptModel::QueuedRole).toBool(),
            "the steering prompt is marked and counts the step it waited");
    append(
        R"({"type":"user","uuid":"r2","timestamp":"2026-11-07T10:00:30.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"t2","content":"ok"}]}})");
    append(
        R"({"type":"assistant","uuid":"a5","timestamp":"2026-11-07T10:00:31.000Z","message":{"content":[{"type":"tool_use","id":"t3","name":"Read","input":{"file_path":"/x/a.cpp"}}]}})");
    model.readMore();
    require(steps.rowCount() == 1, "the box lists the steps after the steering prompt");
    append(
        R"({"type":"assistant","uuid":"a6","timestamp":"2026-11-07T10:00:40.000Z","message":{"content":[{"type":"text","text":"Done with tabs."}]}})");
    append(R"({"type":"system","subtype":"turn_duration","durationMs":40000})");
    model.readMore();
    require(kinds() == "prompt|work|prompt|work|reply" && steps.rowCount() == 0,
            "finished: prompt, work, steering prompt, work, reply");
    require(model.data(model.index(1), TranscriptModel::TitleRole).toString() == "Worked for 20s" &&
                model.data(model.index(3), TranscriptModel::TitleRole).toString() ==
                    "Worked for 20s",
            "each part is timed from its prompt");
    model.toggleWork(3);
    require(kinds() == "prompt|work|prompt|work|tool|reply",
            "the second part opens on its own, one row per step");
    model.toggleWork(3);
    model.toggleWork(1);
    require(kinds() == "prompt|work|thinking|tool|reply|tool|prompt|work|reply",
            "the first part opens to its steps and interim reply; the diff stays in the step");
}

// Code colors by its language: the role at a column of a line.
void code_is_highlighted() {
    namespace code = lapis::desktop::code;
    const auto role_at = [](const QString& language, const QString& line, int column,
                            int state = 0) {
        for (const auto& span : code::tokenize(line, code::language(language), state))
            if (column >= span.start && column < span.start + span.length)
                return span.role;
        return code::Role::plain;
    };
    const QString cpp = QStringLiteral("int count = run(\"x\", 42); // done");
    require(role_at(QStringLiteral("cpp"), cpp, 0) == code::Role::type, "C++ type");
    require(role_at(QStringLiteral("cpp"), cpp, 4) == code::Role::plain,
            "C++ variable stays plain");
    require(role_at(QStringLiteral("cpp"), cpp, 12) == code::Role::function, "C++ call");
    require(role_at(QStringLiteral("cpp"), cpp, 17) == code::Role::string, "C++ string");
    require(role_at(QStringLiteral("cpp"), cpp, 21) == code::Role::number, "C++ number");
    require(role_at(QStringLiteral("cpp"), cpp, 27) == code::Role::comment, "C++ comment");
    require(role_at(QStringLiteral("cpp"), QStringLiteral("#include <vector>"), 1) ==
                code::Role::meta,
            "preprocessor line");
    int state = 0;
    (void)code::tokenize(QStringLiteral("x = \"\"\"start"), code::language(QStringLiteral("py")),
                         state);
    require(state != 0 && role_at(QStringLiteral("py"), QStringLiteral("still inside"), 2, state) ==
                              code::Role::string,
            "a triple-quoted string carries to the next line");
    require(role_at(QStringLiteral("py"), QStringLiteral("def wrap(text):"), 5) ==
                    code::Role::function &&
                role_at(QStringLiteral("py"), QStringLiteral("def wrap(text):"), 0) ==
                    code::Role::keyword,
            "Python def and its name");
    const QString shell = QStringLiteral("cmake --build $DIR | rg error");
    require(role_at(QStringLiteral("bash"), shell, 0) == code::Role::function &&
                role_at(QStringLiteral("bash"), shell, 7) == code::Role::parameter &&
                role_at(QStringLiteral("bash"), shell, 15) == code::Role::property &&
                role_at(QStringLiteral("bash"), shell, 22) == code::Role::function,
            "shell command, flag, variable and piped command");
    require(role_at(QStringLiteral("json"), QStringLiteral("{\"size\": 0, \"all\": false}"), 2) ==
                    code::Role::property &&
                role_at(QStringLiteral("json"), QStringLiteral("{\"size\": 0, \"all\": false}"),
                        20) == code::Role::constant,
            "JSON key and constant");
    require(role_at(QStringLiteral("diff"), QStringLiteral("+added"), 2) == code::Role::added &&
                role_at(QStringLiteral("diff"), QStringLiteral("-gone"), 2) == code::Role::removed,
            "diff lines");
    // Text is never colored: an unlabelled, text, log or unknown fence.
    const QString text = QStringLiteral("| run [ok] | 42 ms | x = \"s\" # note // {v1} 'q' -> 3.5");
    for (const char* name : {"", "text", "txt", "plaintext", "log", "output", "unknown-lang"}) {
        int plain_state = 0;
        require(
            code::tokenize(text, code::language(QString::fromLatin1(name)), plain_state).empty() &&
                plain_state == 0,
            "a plain fence has no colored spans");
    }
    require(role_at(QStringLiteral("py"), text, 13) == code::Role::number,
            "a known language still colors the same line");
}

// A message's local media: Markdown targets and bare paths that exist.
void media_is_found() {
    QTemporaryDir directory;
    const auto touch = [&](const char* name) {
        QFile file(directory.filePath(QString::fromLatin1(name)));
        require(file.open(QIODevice::WriteOnly), "media fixture");
        file.write("x");
        return file.fileName();
    };
    const auto chart = touch("chart.png");
    const auto clip = touch("clip one.mp4");
    const auto page = touch("report.html");
    const auto text =
        QStringLiteral("See ![chart](%1) and the report at %2.\nMissing: /nope/x.png\n"
                       "Clip: [clip](<%3>) and `%2` again.")
            .arg(chart, page, clip);
    const auto found = lapis::desktop::transcript::find_media(text);
    require(found.size() == 3 && found[0].path == chart &&
                found[0].kind == QStringLiteral("image") && found[1].path == clip &&
                found[1].kind == QStringLiteral("video") && found[2].path == page &&
                found[2].kind == QStringLiteral("html"),
            "Markdown targets (spaces inside <>) then bare paths, each once; missing files are "
            "skipped");
    const auto webm = touch("talk.webm");
    const auto other = lapis::desktop::transcript::find_media(webm);
    require(other.size() == 1 && other[0].kind == QStringLiteral("file"),
            "a video macOS cannot play is a card for its app");
}

// A path standing alone as a paragraph shows as its picture in that place;
// one inside a sentence, a list item or a fence stays text and its picture
// goes under the reply, once.
void media_shows_in_place() {
    using lapis::desktop::transcript::pieces;
    QTemporaryDir directory;
    const auto picture = [&](const QString& name, bool clear) {
        QImage image(40, 20, clear ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        image.fill(clear ? Qt::transparent : Qt::darkBlue);
        const auto path = directory.filePath(name);
        require(image.save(path), "picture fixture");
        return path;
    };
    const auto first = picture(QStringLiteral("first.png"), false);
    const auto second = picture(QStringLiteral("second one.png"), true);
    const auto is_media = [](const auto& piece, const QString& path) {
        return piece.media.path == path && piece.media.kind == QStringLiteral("image");
    };

    // Prose, picture, prose; the raw path is not prose any more.
    bool missing = false;
    auto parts = pieces(QStringLiteral("Before it:\n\n%1\n\n**After**").arg(first), &missing);
    require(parts.size() == 3 && parts[0].text == QStringLiteral("Before it:") &&
                is_media(parts[1], first) && parts[1].text == first &&
                parts[2].text == QStringLiteral("**After**") && !missing,
            "a lone path is its picture between the paragraphs around it");

    // Backticks, a Markdown image (spaces inside <>), a bare link, two in one reply.
    parts = pieces(
        QStringLiteral("`%1`\n\nmiddle\n\n![a diagram](<%2>)\n\n[it](%1)").arg(first, second));
    require(parts.size() == 4 && is_media(parts[0], first) && parts[1].text == "middle" &&
                is_media(parts[2], second) && is_media(parts[3], first),
            "backticked, image and link forms each show in place, in order");

    // In a sentence, in a list item, indented, in a fence, or wrapped into a
    // paragraph: text, not a picture.
    const auto text_only = QStringLiteral("See %1 for it.\n\n- %1\n\n   %1\n\n```\n%1\n```\n\n"
                                          "Lines run on\n%1")
                               .arg(first);
    parts = pieces(text_only);
    require(std::none_of(parts.begin(), parts.end(),
                         [](const auto& piece) { return !piece.media.path.isEmpty(); }),
            "a path that is not its own paragraph stays text");
    require(parts.size() == 3 && parts[1].code && parts[1].text == first,
            "the fenced path stays in its code piece");

    // A file not there yet stays text, and says so, so it is looked for again.
    missing = false;
    const auto later = directory.filePath(QStringLiteral("later.png"));
    parts = pieces(QStringLiteral("Soon:\n\n%1").arg(later), &missing);
    require(parts.size() == 1 && parts[0].text.endsWith(later) && missing,
            "a missing file stays text and is marked missing");

    // The model: in-place pieces, and only the rest under the reply.
    TranscriptModel model;
    const auto reply =
        QStringLiteral("Look:\n\n%1\n\nAs %1 and [this](<%2>) show.").arg(first, second);
    const auto shown = model.pieces(reply);
    require(shown.size() == 3 && shown[1].toMap().value("media").toMap().value("path") == first &&
                shown[0].toMap().value("media").isNull(),
            "the model's pieces carry the media");
    const auto under = model.replyMedia(reply);
    require(under.size() == 1 && under[0].toMap().value("path") == second,
            "under the reply only what is not shown in place");
    require(under[0].toMap().value("alpha").toBool(), "alpha marks a clear picture");
    require(!shown[1].toMap().value("media").toMap().value("alpha").toBool(),
            "an opaque picture has no alpha");
    // The missing file appears; the next layout of that reply finds it.
    const auto pending = QStringLiteral("Now:\n\n%1").arg(later);
    require(model.pieces(pending).size() == 1, "missing at first");
    QImage(4, 4, QImage::Format_RGB32).save(later);
    require(model.pieces(pending).size() == 2, "found when the reply is laid out again");

    // More than 8: every lone path shows in place, and nothing repeats under it.
    QStringList many;
    for (int i = 0; i < 10; ++i)
        many << picture(QStringLiteral("many%1.png").arg(i), false);
    const auto lots = many.join(QStringLiteral("\n\n"));
    parts = pieces(lots);
    require(parts.size() == 10 && is_media(parts[9], many[9]), "ten lone paths, ten pictures");
    require(model.replyMedia(lots).isEmpty(), "none repeat under the reply");
}

// Claude Code's own commands plus the person's commands and skills.
void slash_commands_are_listed() {
    QTemporaryDir home;
    QDir(home.path()).mkpath(QStringLiteral(".claude/commands/team"));
    QDir(home.path()).mkpath(QStringLiteral(".claude/skills/notes"));
    QFile command(home.filePath(QStringLiteral(".claude/commands/team/ship.md")));
    require(command.open(QIODevice::WriteOnly), "command fixture");
    command.write("---\ndescription: \"Ship the release\"\n---\nDo it.\n");
    command.close();
    QFile skill(home.filePath(QStringLiteral(".claude/skills/notes/SKILL.md")));
    require(skill.open(QIODevice::WriteOnly), "skill fixture");
    skill.write("---\nname: notes\ndescription: Keep notes\n---\n");
    skill.close();
    const auto list = lapis::desktop::claude_slash_commands(home.path(), {});
    const auto find = [&list](const QString& name) {
        for (const auto& entry : list)
            if (entry.toMap().value(QStringLiteral("name")) == name)
                return entry.toMap();
        return QVariantMap{};
    };
    require(!find(QStringLiteral("compact")).isEmpty() && !find(QStringLiteral("model")).isEmpty(),
            "Claude Code's own commands are listed");
    require(find(QStringLiteral("team:ship")).value(QStringLiteral("description")) ==
                    QStringLiteral("Ship the release") &&
                find(QStringLiteral("notes")).value(QStringLiteral("description")) ==
                    QStringLiteral("Keep notes"),
            "the person's commands and skills are listed with their descriptions");
}

QVariantMap command_named(const QVariantList& list, const QString& name) {
    for (const auto& entry : list)
        if (entry.toMap().value(QStringLiteral("name")) == name)
            return entry.toMap();
    return {};
}

void write_file(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "fixture written");
}

// Codex's and Antigravity's own commands, the skills agy offers as commands,
// and nothing for a CLI lapis does not know.
void other_cli_slash_commands_are_listed() {
    using lapis::desktop::slash_commands;
    QTemporaryDir home;
    QTemporaryDir repo;
    // Codex 0.157.1 reads no prompt files: one left from an older Codex is
    // not offered.
    write_file(home.filePath(QStringLiteral(".codex/prompts/old.md")), "Old prompt\n");
    const auto codex = slash_commands(QStringLiteral("codex"), home.path(), repo.path());
    const auto status = command_named(codex, QStringLiteral("status"));
    require(!status.isEmpty() && status.value(QStringLiteral("terminal")).toBool() &&
                !command_named(codex, QStringLiteral("model")).isEmpty(),
            "Codex's own commands are listed, a panel one marked for the terminal");
    require(command_named(codex, QStringLiteral("compact")).value(QStringLiteral("terminal")) ==
                QVariant(false),
            "Codex's /compact runs as a turn in the thread");
    require(command_named(codex, QStringLiteral("old")).isEmpty() &&
                command_named(codex, QStringLiteral("prompts:old")).isEmpty() &&
                command_named(codex, QStringLiteral("quit")).isEmpty() &&
                command_named(codex, QStringLiteral("exit")).isEmpty(),
            "Codex lists no prompt files and no command that ends the session");

    // agy: the project's skills from the folder up to its repository root,
    // then the person's, without those hidden from the slash menu.
    QDir(repo.path()).mkpath(QStringLiteral(".git"));
    const auto folder = repo.filePath(QStringLiteral("sub/dir"));
    QDir().mkpath(folder);
    write_file(repo.filePath(QStringLiteral(".agents/skills/ship/SKILL.md")),
               "---\nname: ship\ndescription: >-\n  Ship the\n  release\n---\nDo it.\n");
    write_file(repo.filePath(QStringLiteral(".agents/skills/quiet/SKILL.md")),
               "---\nname: quiet\ndescription: Hidden\ndisable-slash-command: true\n---\n");
    write_file(home.filePath(QStringLiteral(".gemini/config/skills/notes/SKILL.md")),
               "---\nname: notes\ndescription: Keep notes\n---\n");
    write_file(home.filePath(QStringLiteral(".gemini/config/skills/ship/SKILL.md")),
               "---\nname: ship\ndescription: The person's ship\n---\n");
    const auto agy = slash_commands(QStringLiteral("agy"), home.path(), folder);
    require(command_named(agy, QStringLiteral("resume")).value(QStringLiteral("terminal")) ==
                    QVariant(true) &&
                !command_named(agy, QStringLiteral("compact")).isEmpty() &&
                command_named(agy, QStringLiteral("exit")).isEmpty(),
            "agy's own commands are listed, a picker marked for the terminal");
    const auto ship = command_named(agy, QStringLiteral("ship"));
    require(ship.value(QStringLiteral("description")) == QStringLiteral("Ship the release") &&
                ship.value(QStringLiteral("source")) == QStringLiteral("project") &&
                ship.value(QStringLiteral("terminal")) == QVariant(false),
            "a project skill is listed with its folded description and wins over the person's");
    require(command_named(agy, QStringLiteral("notes")).value(QStringLiteral("source")) ==
                    QStringLiteral("user") &&
                command_named(agy, QStringLiteral("quiet")).isEmpty(),
            "the person's skills are listed, a hidden one is not");

    require(slash_commands(QStringLiteral("omp"), home.path(), folder).isEmpty() &&
                slash_commands(QString(), home.path(), folder).isEmpty(),
            "a CLI lapis does not know gets no commands");
    require(!command_named(slash_commands(QStringLiteral("claude"), home.path(), {}),
                           QStringLiteral("compact"))
                 .isEmpty(),
            "Claude Code's list is chosen by its harness id");
}

// Charts follow the theme: the style lapis writes for matplotlib.
void chart_style_is_written() {
    QTemporaryDir home;
    const auto saved = qgetenv("HOME");
    qputenv("HOME", home.path().toUtf8());
    TranscriptModel model;
    model.writeChartStyle({{QStringLiteral("background"), QColor("#101010")},
                           {QStringLiteral("text"), QColor("#eeeeee")},
                           {QStringLiteral("muted"), QColor("#888888")},
                           {QStringLiteral("grid"), QColor("#333333")},
                           {QStringLiteral("light"), false}});
    QFile rc(home.filePath(QStringLiteral(".lapis/visual/matplotlibrc")));
    const bool opened = rc.open(QIODevice::ReadOnly);
    const auto text = opened ? QString::fromUtf8(rc.readAll()) : QString();
    require(text.contains(QStringLiteral("figure.facecolor: 101010")) &&
                text.contains(QStringLiteral("text.color: eeeeee")) &&
                text.contains(QStringLiteral("'7aa2f7'")) &&
                QFile::exists(home.filePath(QStringLiteral(".lapis/visual/palette.json"))),
            "matplotlib style and palette follow the colors");
    qputenv("HOME", saved);
}

// Code inside a sentence is quiet: a faint tint and dimmer text, not the
// code well. A link keeps its color, and a code block the pieces leave in
// prose (fenced inside a quote) does not turn into inline code.
void inline_code_is_quiet() {
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nTextEdit {}", QUrl());
    std::unique_ptr<QObject> edit(component.create());
    auto* document = edit ? edit->property("textDocument").value<QQuickTextDocument*>() : nullptr;
    require(document != nullptr, "a TextEdit gives a document to style");
    if (document == nullptr)
        return;
    const QColor well("#111111"), tint("#202020"), dim("#cbcbcb"), link("#339cff");
    TranscriptModel().style(
        document,
        QStringLiteral("Call `load_settings` or see [the `Settings` "
                       "struct](https://example.com).\n\n> ```\n> quoted();\n> ```"),
        {{QStringLiteral("sans"), QStringLiteral("Helvetica")},
         {QStringLiteral("mono"), QStringLiteral("Menlo")},
         {QStringLiteral("size"), 15},
         {QStringLiteral("codeSize"), 13},
         {QStringLiteral("codeColor"), well},
         {QStringLiteral("inlineCodeColor"), tint},
         {QStringLiteral("inlineCodeTextColor"), dim},
         {QStringLiteral("linkColor"), link}});
    const auto format_of = [&](const QString& text) {
        const auto found =
            document->textDocument()->find(text, 0, QTextDocument::FindCaseSensitively);
        QTextCursor at(document->textDocument());
        at.setPosition(found.selectionStart() + 1);
        return at.charFormat();
    };
    const auto plain = format_of(QStringLiteral("load_settings"));
    require(plain.fontFixedPitch() && plain.background().color() == tint &&
                plain.foreground().color() == dim,
            "inline code takes the inline tint and text, not the code well");
    const auto linked = format_of(QStringLiteral("Settings"));
    require(linked.background().color() == tint && linked.foreground().color() == link,
            "inline code in a link keeps the link color");
    const auto block = format_of(QStringLiteral("quoted"));
    require(block.background().color() != tint && block.foreground().color() != dim,
            "a code block left in prose does not take the inline colors");
}

// Math is found in prose and nowhere else: money, shell variables, code
// spans, fences, link targets and escaped dollars stay text.
void math_is_found() {
    namespace math = lapis::desktop::transcript::math;
    const auto sources = [](const QString& text) {
        QStringList out;
        for (const auto& span : math::find(text))
            out << (span.display ? QStringLiteral("D:") : QStringLiteral("I:")) + span.source;
        return out.join(QLatin1Char('|'));
    };
    const struct {
        const char* text;
        const char* spans;
    } cases[] = {
        {"plain text with no math", ""},
        {"The model is $y = mx + b$ here.", "I:y = mx + b"},
        {"a $x$ b $$y$$ c \\(z\\) d \\[w\\]", "I:x|D:y|I:z|D:w"},
        {"$$\nE = mc^2\n$$", "D:\nE = mc^2\n"},
        {"It costs $5 and $10 a month.", ""},
        {"Between $5-$10 per run.", ""},
        {"Add $HOME/bin and $PATH to it.", ""},
        {"Use $HOME/bin:$PATH.", ""},
        {"Set ${HOME}/x and $y$.", "I:y"},
        {"Print $PATH$ now", ""},
        {"US$5 and 4$ later", ""},
        {"US$ 3 or so", ""},
        {"That was $42.50 for the cables and more US$", ""},
        {"An escaped \\$5 and \\$x$ stay", ""},
        {"Code `$x$` then $y$", "I:y"},
        {"Code ``a ` $x$`` then $y$", "I:y"},
        {"> ```\n> $x$\n> ```\n$y$", "I:y"},
        {"See [the $a$ page](https://example.com/$b$) for $c$", "I:a|I:c"},
        {"Not across $a\n\nb$ a blank line", ""},
        {"Across $a +\nb$ one line break", "I:a +\nb"},
        {"The $x$-axis and $10^3$ runs", "I:x|I:10^3"},
        {"A dollar $ alone", ""},
        {"Empty $$ $$ display", ""},
        {"Escaped inside $a \\$ b$", "I:a \\$ b"},
    };
    for (const auto& item : cases) {
        const auto found = sources(QString::fromUtf8(item.text));
        if (found != QString::fromUtf8(item.spans))
            std::fprintf(stderr, "  %s -> %s\n", item.text, qPrintable(found));
        require(found == QString::fromUtf8(item.spans), "math spans are found by the rule");
    }
    require(!math::may_contain(QStringLiteral("no math (here) [at all]")) &&
                math::prepare(QStringLiteral("no math $5")).spans.empty() &&
                math::prepare(QStringLiteral("no math $5")).markdown ==
                    QStringLiteral("no math $5"),
            "text without math is left as it is");
}

// LaTeX becomes readable text with super- and subscripts; what cannot be
// set well falls back to its source.
void math_is_rendered() {
    namespace math = lapis::desktop::transcript::math;
    const struct {
        const char* latex;
        const char* shown;
    } cases[] = {
        {"x^2", "x^{2}"},
        {"x^{n+1}", "x^{n+1}"},
        {"a_{ij}", "a_{ij}"},
        {"x_i^2", "x_{i}^{2}"},
        {"\\alpha + \\beta", "α + β"},
        {"\\Omega \\varepsilon \\infty \\partial \\nabla", "Ωε∞∂∇"},
        {"a \\pm b \\times c \\cdot d", "a ± b × c · d"},
        {"x \\leq y \\ne z \\approx w", "x ≤ y ≠ z ≈ w"},
        {"f: A \\to B", "f: A → B"},
        {"x \\in \\mathbb{R}^n", "x ∈ ℝ^{n}"},
        {"\\forall x \\exists y", "∀x∃y"},
        {"\\frac{1}{2}", "1/2"},
        {"\\frac{a+b}{c+d}", "(a + b)/(c + d)"},
        {"\\frac{x^2}{2}", "x^{2}/2"},
        {"\\sqrt{x}", "√x"},
        {"\\sqrt{n+1}", "√(n + 1)"},
        {"\\sqrt[3]{x}", "^{3}√x"},
        {"\\binom{n}{k}", "C(n, k)"},
        {"\\sum_{i=1}^{n} x_i", "∑_{i=1}^{n}\u2009x_{i}"},
        {"e^{-x}", "e^{−x}"},
        {"-1", "−1"},
        {"\\sin\\theta + \\log(x)", "sin\u2009θ + log(x)"},
        {"\\left( \\frac{a}{b} \\right)", "(a/b)"},
        {"\\left. x \\right|", "x|"},
        {"\\text{if } x > 0", "if x > 0"},
        {"\\operatorname{softmax}(z)", "softmax(z)"},
        {"\\mathbf{W}", "W"},
        {"\\hat{y}", "y\u0302"},
        {"\\langle u, v \\rangle", "⟨u, v⟩"},
        {"\\lfloor x \\rfloor", "⌊x⌋"},
        {"\\|x\\|_2", "‖x‖_{2}"},
        {"1,157,600 \\times 3", "1,157,600 × 3"},
        {"10\\% \\{a\\}", "10%{a}"},
        {"a \\, b \\quad c", "a\u2009b\u2003c"},
        {"x \\not= y", "x ≠ y"},
        {"\\ldots \\cdots", "…⋯"},
        {"\\ell \\hbar", "ℓℏ"},
        {"\\foo x", "foo\u2009x"},
        {"e^{x_i}", "e^{x_i}"},
    };
    for (const auto& item : cases) {
        const auto rendered = math::render(QString::fromUtf8(item.latex));
        const auto shown = math::plain(rendered);
        if (!rendered.ok || shown != QString::fromUtf8(item.shown))
            std::fprintf(stderr, "  %s -> %s (ok %d)\n", item.latex, qPrintable(shown),
                         rendered.ok);
        require(rendered.ok && shown == QString::fromUtf8(item.shown), "LaTeX is set as text");
    }
    for (const char* latex : {
             "\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}",
             "a &= b \\\\ c &= d",
             "\\frac{\\frac{\\frac{a}{b}}{c}}{d}",
             "\\overset{a}{b}",
             "\\frac{a}",
             "x^",
             "{a",
             "a}",
         })
        require(!math::render(QString::fromUtf8(latex)).ok, "what cannot be set shows as source");
    const auto letters = math::render(QStringLiteral("x + 2\\sin y"));
    require(letters.ok && letters.runs.size() >= 3 && letters.runs.front().italic,
            "variables are italic");
    bool upright_sin = false;
    for (const auto& run : letters.runs)
        upright_sin = upright_sin || (run.text.contains(QStringLiteral("sin")) && !run.italic);
    require(upright_sin, "function names are upright");
}

// In a styled reply, markdown never touches math, scripts are real
// super- and subscripts, display math is its own centred paragraph, and a
// matrix shows as source in the inline code style. Copying a whole span
// gives its source back.
void math_is_styled() {
    namespace math = lapis::desktop::transcript::math;
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nTextEdit {}", QUrl());
    std::unique_ptr<QObject> edit(component.create());
    auto* target = edit ? edit->property("textDocument").value<QQuickTextDocument*>() : nullptr;
    require(target != nullptr, "a TextEdit gives a document to style");
    if (target == nullptr)
        return;
    const QColor tint("#202020"), dim("#cbcbcb");
    const QVariantMap style{{QStringLiteral("sans"), QStringLiteral("Helvetica")},
                            {QStringLiteral("mono"), QStringLiteral("Menlo")},
                            {QStringLiteral("size"), 15},
                            {QStringLiteral("codeSize"), 13},
                            {QStringLiteral("inlineCodeColor"), tint},
                            {QStringLiteral("inlineCodeTextColor"), dim}};
    auto& document = *target->textDocument();
    const auto format_at = [&](int position) {
        QTextCursor at(&document);
        at.setPosition(position + 1);
        return at.charFormat();
    };

    TranscriptModel().style(target,
                            QStringLiteral("Sum $x_i + y_i$ and $a*b*c$ and **bold $z^2$** here.\n"
                                           "$$\n\\frac{a}{b} = c\n$$\nthen after.\n\n"
                                           "$$\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}$$"),
                            style);
    const auto text = document.toPlainText();
    require(text.contains(QStringLiteral("Sum xi + yi and a ∗ b ∗ c and bold z2 here.")),
            "markdown does not read math's underscores and asterisks");
    const auto sub = format_at(static_cast<int>(text.indexOf(QStringLiteral("xi")) + 1));
    require(sub.verticalAlignment() == QTextCharFormat::AlignSubScript,
            "a subscript is a real subscript");
    const auto x = format_at(static_cast<int>(text.indexOf(QStringLiteral("xi"))));
    require(x.fontItalic() && x.verticalAlignment() == QTextCharFormat::AlignNormal,
            "a variable is italic on the baseline");
    const auto squared = format_at(static_cast<int>(text.indexOf(QStringLiteral("z2")) + 1));
    require(squared.verticalAlignment() == QTextCharFormat::AlignSuperScript &&
                squared.fontWeight() >= QFont::DemiBold,
            "a superscript in bold text is raised and stays bold");
    const auto equation = document.findBlock(static_cast<int>(text.indexOf(QStringLiteral("a/b"))));
    require(equation.text() == QStringLiteral("a/b = c") &&
                equation.blockFormat().alignment().testFlag(Qt::AlignHCenter) &&
                equation.blockFormat().hasProperty(math::display_property),
            "display math is a centred paragraph of its own");
    require(equation.previous().text().endsWith(QStringLiteral("here.")) &&
                equation.next().text() == QStringLiteral("then after."),
            "the text around display math keeps its own paragraphs");
    const auto matrix_at = static_cast<int>(text.indexOf(QStringLiteral("\\begin{pmatrix}")));
    const auto matrix = format_at(matrix_at + 2);
    require(matrix_at >= 0 && !text.contains(QStringLiteral("$$")) && matrix.fontFixedPitch() &&
                matrix.background().color() == tint,
            "a matrix shows as its source, without dollars, in the inline code style");

    // A whole span copies as written; part of one as shown.
    const auto start = static_cast<int>(text.indexOf(QStringLiteral("Sum")));
    const auto whole = lapis::desktop::transcript::selection_text(
        document, start, static_cast<int>(text.indexOf(QStringLiteral(" and a"))));
    require(whole == QStringLiteral("Sum $x_i + y_i$"), "a whole span copies as its source");
    const auto part = lapis::desktop::transcript::selection_text(document, start, start + 6);
    require(part == QStringLiteral("Sum xi"), "part of a span copies as shown");
    const auto display = lapis::desktop::transcript::selection_text(
        document, equation.position(), equation.position() + equation.length() - 1);
    require(display == QStringLiteral("$$\n\\frac{a}{b} = c\n$$"),
            "display math copies as its source");

    // Without math, styling is what it always was.
    TranscriptModel().style(target, QStringLiteral("Costs $5 and $10, see `$x$`."), style);
    require(document.toPlainText() == QStringLiteral("Costs $5 and $10, see $x$."),
            "text without math is styled as before");
}
// Made-up Claude Code records for subagents and background work, timed from
// 2026-11-11T16:00:00Z; %1 is the folder their files live in.
const char* const kWork[] = {
    R"({"type":"user","uuid":"p1","timestamp":"2026-11-11T16:00:00.000Z","message":{"content":"Split the parser"}})",
    R"({"type":"assistant","uuid":"a1","timestamp":"2026-11-11T16:00:01.000Z","message":{"content":[{"type":"tool_use","id":"g1","name":"Agent","input":{"description":"Map the lexer","subagent_type":"Explore","model":"sonnet","run_in_background":true,"prompt":"Find the lexer"}}]}})",
    R"({"type":"user","uuid":"r1","timestamp":"2026-11-11T16:00:02.000Z","toolUseResult":{"isAsync":true,"status":"async_launched","agentId":"agent1","resolvedModel":"claude-sonnet-5-5"},"message":{"content":[{"type":"tool_result","tool_use_id":"g1","content":"Async agent launched successfully."}]}})",
    R"({"type":"assistant","uuid":"a2","timestamp":"2026-11-11T16:00:03.000Z","message":{"content":[{"type":"tool_use","id":"g2","name":"Agent","input":{"description":"Check messages","prompt":"Read the errors","run_in_background":"true"}}]}})",
    R"({"type":"user","uuid":"r2","timestamp":"2026-11-11T16:00:04.000Z","toolUseResult":{"isAsync":true,"status":"async_launched","agentId":"agent2"},"message":{"content":[{"type":"tool_result","tool_use_id":"g2","content":"Async agent launched successfully."}]}})",
    R"({"type":"assistant","uuid":"a3","timestamp":"2026-11-11T16:00:05.000Z","message":{"content":[{"type":"tool_use","id":"b1","name":"Bash","input":{"command":"npm test --watch","description":"Watch tests","run_in_background":true}}]}})",
    R"({"type":"user","uuid":"r3","timestamp":"2026-11-11T16:00:06.000Z","toolUseResult":{"backgroundTaskId":"bg1","stdout":""},"message":{"content":[{"type":"tool_result","tool_use_id":"b1","content":"Command running in background with ID: bg1. Output is being written to: %1/bg1.output You will be notified."}]}})",
    R"({"type":"assistant","uuid":"a4","timestamp":"2026-11-11T16:00:07.000Z","message":{"content":[{"type":"tool_use","id":"c1","name":"BashOutput","input":{"bash_id":"bg1"}}]}})",
    R"({"type":"user","uuid":"r4","timestamp":"2026-11-11T16:00:08.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"c1","content":"PASS lexer"}]}})",
    R"({"type":"assistant","uuid":"a5","timestamp":"2026-11-11T16:00:09.000Z","message":{"content":[{"type":"tool_use","id":"c2","name":"Read","input":{"file_path":"%1/bg1.output"}}]}})",
    R"({"type":"user","uuid":"r5","timestamp":"2026-11-11T16:00:10.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"c2","content":"PASS parser"}]}})",
    R"({"type":"assistant","uuid":"a6","timestamp":"2026-11-11T16:00:11.000Z","message":{"content":[{"type":"tool_use","id":"m1","name":"Monitor","input":{"description":"CI status","command":"gh run watch","timeout_ms":600000}}]}})",
    R"({"type":"user","uuid":"r6","timestamp":"2026-11-11T16:00:12.000Z","toolUseResult":{"taskId":"mon1","timeoutMs":600000,"persistent":false},"message":{"content":[{"type":"tool_result","tool_use_id":"m1","content":"Monitor started"}]}})",
    R"({"type":"assistant","uuid":"a7","timestamp":"2026-11-11T16:00:13.000Z","message":{"content":[{"type":"text","text":"Started them."}]}})",
    R"({"type":"system","subtype":"turn_duration","durationMs":13000,"timestamp":"2026-11-11T16:00:13.500Z"})",
};
// Later: the monitor's event, the command's and the first subagent's ends.
const char* const kWorkEnds[] = {
    R"({"type":"user","uuid":"n1","timestamp":"2026-11-11T16:01:00.000Z","message":{"content":"<task-notification>\n<task-id>mon1</task-id>\n<summary>Monitor event: \"CI status\"</summary>\n<event>build 3 passed</event>\n</task-notification>"}})",
    R"({"type":"user","uuid":"n2","timestamp":"2026-11-11T16:02:00.000Z","message":{"content":"<task-notification>\n<task-id>bg1</task-id>\n<tool-use-id>b1</tool-use-id>\n<status>failed</status>\n<summary>Background command \"Watch tests\" failed with exit code 2</summary>\n</task-notification>"}})",
    R"({"type":"user","uuid":"n3","timestamp":"2026-11-11T16:03:00.000Z","message":{"content":"<task-notification>\n<task-id>agent1</task-id>\n<tool-use-id>g1</tool-use-id>\n<status>completed</status>\n<summary>Agent \"Map the lexer\" finished</summary>\n<result>The lexer is in lex.cpp.</result>\n<usage><subagent_tokens>48210</subagent_tokens><tool_uses>23</tool_uses><duration_ms>178000</duration_ms></usage>\n</task-notification>"}})",
    R"({"type":"assistant","uuid":"a8","timestamp":"2026-11-11T16:03:05.000Z","message":{"content":[{"type":"text","text":"The map is in."}]}})",
    R"({"type":"system","subtype":"turn_duration","durationMs":5000,"timestamp":"2026-11-11T16:03:05.500Z"})",
};
// The second subagent's own file: its prompt, two steps, an interim note.
const char* const kSubagent[] = {
    R"({"type":"user","isSidechain":true,"agentId":"agent2","uuid":"s1","timestamp":"2026-11-11T16:00:04.000Z","message":{"content":"Read the errors"}})",
    R"({"type":"assistant","isSidechain":true,"agentId":"agent2","uuid":"s2","timestamp":"2026-11-11T16:00:05.000Z","message":{"content":[{"type":"tool_use","id":"x1","name":"Grep","input":{"pattern":"error("}}]}})",
    R"({"type":"user","isSidechain":true,"agentId":"agent2","uuid":"s3","timestamp":"2026-11-11T16:00:06.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"x1","content":"a.cpp:3"}]}})",
    R"({"type":"assistant","isSidechain":true,"agentId":"agent2","uuid":"s4","timestamp":"2026-11-11T16:00:07.000Z","message":{"content":[{"type":"tool_use","id":"x2","name":"Bash","input":{"command":"ctest"}}]}})",
    R"({"type":"user","isSidechain":true,"agentId":"agent2","uuid":"s5","timestamp":"2026-11-11T16:00:09.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"x2","content":"ok"}]}})",
    R"({"type":"assistant","isSidechain":true,"agentId":"agent2","uuid":"s6","timestamp":"2026-11-11T16:00:10.000Z","message":{"content":[{"type":"text","text":"Errors are in a.cpp."}]}})",
};

qint64 at(const char* stamp) {
    return QDateTime::fromString(QString::fromLatin1(stamp), Qt::ISODateWithMs).toMSecsSinceEpoch();
}

void append(const QString& path, const QStringList& lines) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    require(file.open(QIODevice::Append), "fixture appends");
    for (const auto& line : lines)
        file.write(line.toUtf8() + '\n');
}

QStringList filled(const char* const* lines, size_t count, const QString& folder) {
    QStringList out;
    for (size_t i = 0; i < count; ++i)
        out << QString::fromUtf8(lines[i]).replace(QStringLiteral("%1"), folder);
    return out;
}

// The running items, by the step that started them.
QList<int> running_items(const TranscriptModel& model) {
    QList<int> out;
    for (const auto& entry : model.running())
        out << entry.toMap().value(QStringLiteral("item")).toInt();
    return out;
}

void subagents_and_background_work() {
    using lapis::desktop::transcript::Task;
    QTemporaryDir folder;
    const auto path = folder.filePath(QStringLiteral("session.jsonl"));
    append(path, filled(kWork, std::size(kWork), folder.path()));
    TranscriptModel model;
    model.setNow(at("2026-11-11T16:00:30.000Z"));
    model.setPath(path);
    model.loadNow();
    const auto& items = model.items();
    const auto& tasks = model.tasks();
    require(tasks.size() == 4, "two subagents, a command and a monitor are tasks");
    if (tasks.size() != 4)
        return;
    require(tasks[0].type == Task::Type::subagent && tasks[0].id == "agent1" &&
                tasks[0].description == "Map the lexer" && tasks[0].agent_type == "Explore" &&
                tasks[0].model == "claude-sonnet-5-5",
            "a subagent's description, type and model");
    require(tasks[1].id == "agent2", "\"true\" as text runs in the background too");
    require(tasks[2].type == Task::Type::background && tasks[2].id == "bg1" &&
                tasks[2].description == "npm test --watch" &&
                tasks[2].file == folder.filePath(QStringLiteral("bg1.output")),
            "a background command and its output file");
    require(tasks[3].type == Task::Type::monitor && tasks[3].id == "mon1" &&
                tasks[3].expires_ms == at("2026-11-11T16:00:11.000Z") + 600000,
            "a monitor and its expiry");
    const auto& launch = items[static_cast<size_t>(tasks[0].item)];
    require(launch.title == "Subagent" && launch.body == "Map the lexer" && launch.result.isEmpty(),
            "a subagent is one step, the launch notice dropped");
    require(items[static_cast<size_t>(tasks[2].item)].title == "Started in background",
            "a background command's step");
    // Its output checks fold into it.
    require(tasks[2].checks == 2 && tasks[2].last_check == "PASS parser",
            "BashOutput and a Read of its output file are checks");
    int quiet = 0;
    for (const auto& item : items)
        quiet += item.quiet ? 1 : 0;
    require(quiet == 2, "the checks are no rows of their own");
    require(model.turnCount() == 1, "one turn");
    QString summary;
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.data(model.index(row), TranscriptModel::KindRole).toString() == "work") {
            summary = model.data(model.index(row), TranscriptModel::DetailRole).toString();
            require(model.data(model.index(row), TranscriptModel::StepsRole).toInt() == 4,
                    "four steps: two subagents, a command, a monitor");
        }
    require(summary == "ran 2 commands, started 2 agents", "the summary counts them");
    // Everything runs, though the turn ended: the agent waits on them.
    require(running_items(model).size() == 4, "all four run");
    require(model.waitingSince() == static_cast<double>(at("2026-11-11T16:00:13.500Z")),
            "waiting since the turn ended");
    const auto first = model.running().first().toMap();
    require(first.value("type").toString() == "subagent" &&
                first.value("description").toString() == "Map the lexer" &&
                first.value("running").toBool() &&
                static_cast<qint64>(first.value("since").toDouble()) ==
                    at("2026-11-11T16:00:01.000Z"),
            "a running subagent's entry");

    // The second subagent's own file: read on open, counted while it runs.
    const auto own = folder.filePath(QStringLiteral("session/subagents/agent-agent2.jsonl"));
    append(own, filled(kSubagent, std::size(kSubagent), folder.path()));
    {
        // Last written when its last record was.
        QFile file(own);
        require(file.open(QIODevice::ReadWrite) &&
                    file.setFileTime(QDateTime::fromMSecsSinceEpoch(at("2026-11-11T16:00:10.000Z")),
                                     QFileDevice::FileModificationTime),
                "the subagent's file time is set");
    }
    model.tick();
    QVariantMap second;
    for (const auto& entry : model.running())
        if (entry.toMap().value("item").toInt() == tasks[1].item)
            second = entry.toMap();
    require(second.value("steps").toInt() == 2, "a running subagent counts its own steps");
    auto detail = model.taskDetail(tasks[1].item);
    const auto steps = detail.value("steps").toList();
    require(detail.value("found").toBool() && steps.size() == 3 &&
                steps[0].toMap().value("title") == "Searched for" &&
                steps[1].toMap().value("title") == "Ran" &&
                steps[2].toMap().value("kind") == "reply",
            "an opened subagent lists its steps; while it runs its note is one");

    // Ends land on their steps; nothing new is shown for them.
    const int rows_before = model.rowCount();
    model.setNow(at("2026-11-11T16:03:30.000Z"));
    append(path, filled(kWorkEnds, std::size(kWorkEnds), folder.path()));
    model.readMore();
    require(tasks[3].checks == 1 && tasks[3].last_check == "build 3 passed",
            "a monitor's event is a check");
    require(tasks[2].status == "failed" && tasks[2].exited && tasks[2].exit_code == 2,
            "the command failed with its exit code");
    require(tasks[0].status == "completed" && tasks[0].steps == 23 && tasks[0].tokens == 48210 &&
                tasks[0].duration_ms == 178000 && tasks[0].report == "The lexer is in lex.cpp.",
            "the subagent's end: its steps, tokens, time and report");
    require(model.rowCount() == rows_before + 1,
            "woken turns add only the reply, no finished rows");
    const auto still = running_items(model);
    require(still.size() == 2 && still.contains(tasks[1].item) && still.contains(tasks[3].item),
            "finished ones leave the running list");
    const auto map = model.data(model.index(0), TranscriptModel::TaskRole);
    require(!map.isValid() || map.isNull(), "a prompt has no task");
    // The monitor's own expiry ends it; a subagent silent too long has ended.
    model.setNow(at("2026-11-11T16:11:00.000Z"));
    model.tick();
    require(running_items(model) == QList<int>{tasks[1].item}, "a monitor stops at its expiry");
    model.setNow(at("2026-11-11T18:00:00.000Z"));
    model.tick();
    require(model.running().isEmpty(), "a subagent with no end, silent an hour, is not running");
    const auto stale = model.itemEntry(tasks[1].item).value("task").toMap();
    require(stale.value("stale").toBool() && !stale.value("running").toBool(),
            "it shows as ended with no end recorded");

    // Reloaded mid-run, the same work runs.
    QTemporaryDir again;
    const auto copy = again.filePath(QStringLiteral("session.jsonl"));
    append(copy, filled(kWork, std::size(kWork), again.path()));
    TranscriptModel reloaded;
    reloaded.setNow(at("2026-11-11T16:00:40.000Z"));
    reloaded.setPath(copy);
    reloaded.loadNow();
    require(running_items(reloaded).size() == 4, "a reload finds the same work running");
}

// SendMessage runs a finished subagent again; a command moved to the
// background starts a task; a new session ends the commands left open.
void tasks_resume_and_end() {
    using lapis::desktop::transcript::Task;
    ClaudeReader reader;
    for (
        const char* line : {
            R"({"type":"user","timestamp":"2026-11-11T16:00:00.000Z","message":{"content":"Go"}})",
            R"({"type":"assistant","timestamp":"2026-11-11T16:00:01.000Z","message":{"content":[{"type":"tool_use","id":"g1","name":"Agent","input":{"description":"Review"}}]}})",
            R"({"type":"user","timestamp":"2026-11-11T16:00:02.000Z","toolUseResult":{"status":"async_launched","agentId":"ag"},"message":{"content":[{"type":"tool_result","tool_use_id":"g1","content":"launched"}]}})",
            R"({"type":"user","timestamp":"2026-11-11T16:01:00.000Z","message":{"content":"<task-notification><task-id>ag</task-id><status>completed</status><summary>Agent \"Review\" finished</summary></task-notification>"}})",
            R"({"type":"assistant","timestamp":"2026-11-11T16:02:00.000Z","message":{"content":[{"type":"tool_use","id":"s1","name":"SendMessage","input":{"to":"ag","message":"more"}}]}})",
            R"({"type":"user","timestamp":"2026-11-11T16:02:01.000Z","toolUseResult":{"success":true,"resumedAgentId":"ag"},"message":{"content":[{"type":"tool_result","tool_use_id":"s1","content":"sent"}]}})",
            R"({"type":"assistant","timestamp":"2026-11-11T16:03:00.000Z","message":{"content":[{"type":"tool_use","id":"b1","name":"Bash","input":{"command":"make all"}}]}})",
            R"({"type":"user","timestamp":"2026-11-11T16:05:00.000Z","toolUseResult":{"backgroundTaskId":"bgx","timedOutAfterMs":120000},"message":{"content":[{"type":"tool_result","tool_use_id":"b1","content":"moved to the background"}]}})",
            R"({"type":"user","timestamp":"2026-11-11T17:00:00.000Z","message":{"content":"<task-notification><status>stopped</status><summary>1 background shell command task didn't finish before the previous session ended</summary></task-notification>"}})",
        })
        (void)reader.add(line);
    const auto& tasks = reader.tasks();
    require(tasks.size() == 2, "a subagent and a command moved to the background");
    if (tasks.size() != 2)
        return;
    require(tasks[0].status.isEmpty() && tasks[0].ended_ms == 0,
            "SendMessage runs a finished subagent again");
    require(tasks[1].type == Task::Type::background && tasks[1].id == "bgx" &&
                reader.items()[static_cast<size_t>(tasks[1].item)].title ==
                    "Started in background" &&
                tasks[1].started_ms == at("2026-11-11T16:03:00.000Z"),
            "a command that ran long becomes a background task from its start");
    require(tasks[1].status == "stopped", "a new session ends the commands left open");
}
// A Claude Code thought: trimmed, blank-line runs one break, its time kept.
void thoughts_are_clean() {
    ClaudeReader reader;
    (void)reader.add(
        R"({"type":"assistant","thinkingDurationMs":6400,"message":{"content":[{"type":"thinking","thinking":"\n\nRead the parser.\n\n\n\nThen split it.\n\n"}]}})");
    const auto& items = reader.items();
    require(items.size() == 1 && items[0].body == "Read the parser.\n\nThen split it." &&
                items[0].duration_ms == 6400 && items[0].title == "Thought for 6s",
            "a Claude thought is clean and timed");
}
} // namespace

int main(int argc, char** argv) {
    // Text layout needs a GUI application; offscreen keeps it off the screen
    // and the Dock.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    reader_folds_records();
    result_reports_changed_card();
    status_follows_commands();
    command_output_shows();
    bash_mode_commands();
    notices_and_attachments();
    dialogs_are_seen();
    queued_prompts_show();
    several_queued_prompts();
    paths_and_live_lines();
    compact_runs_between_turns();
    results_are_clean();
    model_follows_file();
    reply_splits_at_code();
    items_keep_their_record();
    copies_are_clean();
    switching_back_is_warm();
    turns_fold();
    live_box_follows_claude();
    code_is_highlighted();
    media_is_found();
    media_shows_in_place();
    slash_commands_are_listed();
    other_cli_slash_commands_are_listed();
    chart_style_is_written();
    inline_code_is_quiet();
    math_is_found();
    math_is_rendered();
    math_is_styled();
    subagents_and_background_work();
    tasks_resume_and_end();
    thoughts_are_clean();
    if (failures == 0)
        std::puts("transcript: all checks passed");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
