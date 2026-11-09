// The Claude transcript reader and its model: conversation records become
// items, tool results attach to their calls, bookkeeping is skipped, unknown
// blocks stay visible, and the model follows a file the CLI is still writing.
#include "code_highlight.hpp"
#include "slash_commands.hpp"
#include "transcript.hpp"

#include <QColor>
#include <QDir>

#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTextDocument>
#include <cstdio>
#include <cstdlib>

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
    (void)reader.add(
        R"({"type":"queue-operation","operation":"remove","content":"also add tests","reason":"absorbed_mid_turn"})");
    require(!reader.items()[2].queued && !reader.items()[2].dropped,
            "taken in by the running turn");
    (void)reader.add(R"({"type":"queue-operation","operation":"dequeue"})");
    (void)reader.add(R"({"type":"user","uuid":"u2","message":{"content":"and docs"}})");
    require(reader.items()[3].dropped, "sent on as a turn: its queued copy goes");
    const auto layout = lapis::desktop::transcript::lay_out(reader.items(), {});
    int docs = 0;
    for (const auto& row : layout.rows)
        if (row.item >= 0 && reader.items()[static_cast<size_t>(row.item)].body == "and docs")
            ++docs;
    require(docs == 1 && layout.turns.size() == 2, "shown once, as the next turn's prompt");
    // Folded, the turn still shows the prompt sent into it.
    (void)reader.add(
        R"({"type":"assistant","message":{"content":[{"type":"text","text":"done"}]}})");
    (void)reader.add(R"({"type":"system","subtype":"turn_duration","durationMs":90061000})");
    const auto folded = lapis::desktop::transcript::lay_out(reader.items(), {});
    bool kept = false;
    for (const auto& row : folded.rows)
        kept = kept || row.item == 2;
    require(kept, "a folded turn keeps the prompt sent into it in view");
    require(lapis::desktop::transcript::duration_text(90061000) == "1d 1h", "days for long turns");
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
    require(model.data(model.index(1), TranscriptModel::FailedRole).toBool(), "card shows failure");
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
                kind(3) == QStringLiteral("prompt") && kind(4) == QStringLiteral("tool"),
            "finished turn folds; running turn shows its step");
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
                kind(5) == QStringLiteral("reply") && removed == 1,
            "a turn that finishes folds in place");
    require(lapis::desktop::transcript::context_window(QStringLiteral("claude-opus-4-5"), 1000) ==
                    200000 &&
                lapis::desktop::transcript::context_window(QStringLiteral("claude-opus-4-5[1m]"),
                                                           0) == 1000000,
            "context windows follow the model");
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
    require(role_at(QStringLiteral("unknown-lang"), QStringLiteral("x = \"s\" # note"), 5) ==
                code::Role::string,
            "an unknown language still gets strings");
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
    paths_and_live_lines();
    model_follows_file();
    reply_splits_at_code();
    items_keep_their_record();
    copies_are_clean();
    switching_back_is_warm();
    turns_fold();
    code_is_highlighted();
    media_is_found();
    slash_commands_are_listed();
    chart_style_is_written();
    if (failures == 0)
        std::puts("transcript: all checks passed");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
