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
    require(items[3].title == "Edited" && items[3].diff.size() == 3 && items[3].diff[0].sign == '-' &&
                items[3].diff[2].text == "int b{};" && items[3].added == 2 && items[3].removed == 1,
            "edit diff and counts");
    require(!items[3].answered, "edit still waiting");
    require(items[4].kind == Kind::other && items[4].title == "server_tool_use", "unknown block kept");
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
    const auto parts = pieces(QStringLiteral("Intro **bold**\n\n```cpp\nint a;\n\nint b;\n```\nAfter\n~~~~\nx\n"));
    require(parts.size() == 4, "prose, code, prose, code");
    require(!parts[0].code && parts[0].text == QStringLiteral("Intro **bold**"), "prose kept as markdown");
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
    reader.add(R"({"type":"assistant","uuid":"a-1","message":{"content":[{"type":"text","text":"Hello"}]}})");
    require(reader.items().size() == 2 && reader.items()[0].record == QStringLiteral("u-1") &&
                reader.items()[1].record == QStringLiteral("a-1"),
            "items carry their record uuid");
}
// Copied text pastes cleanly: Qt's separators become newlines, invisible
// characters go, trailing spaces and blank edges are trimmed.
void copies_are_clean() {
    using lapis::desktop::transcript::clean_copy;
    const QString raw = QStringLiteral("\n  One two   Three​ - four\t\r\nfive￼\n\n");
    require(clean_copy(raw) == QStringLiteral("  One two\nThree\n- four\nfive"), "copy text is clean");

    // A selection across a laid-out reply keeps its shape as plain text.
    QTextDocument document;
    document.setMarkdown(QStringLiteral("## Plan\n\nRun `make` first.\n\n- one\n- two\n\n1. first\n2. second\n\nDone."));
    const auto all = lapis::desktop::transcript::selection_text(document, 0, document.characterCount() - 1);
    const bool shaped = all == QStringLiteral("Plan\n\nRun make first.\n\n- one\n- two\n\n1. first\n2. second\n\nDone.");
    if (!shaped)
        std::fprintf(stderr, "got: %s\n", qPrintable(QString(all).replace(QLatin1Char('\n'), QStringLiteral("\\n"))));
    require(shaped, "selection keeps paragraphs, bullets and numbers");
    const int middle = static_cast<int>(document.toPlainText().indexOf(QStringLiteral("make")));
    require(lapis::desktop::transcript::selection_text(document, middle, middle + 4) == QStringLiteral("make"),
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
    file.write(R"({"type":"user","uuid":"p1","timestamp":"2026-11-07T10:00:00.000Z","message":{"content":"Fix it"}})" "\n"
               R"({"type":"assistant","uuid":"a1","effort":"high","message":{"model":"claude-opus-5-5","usage":{"input_tokens":10,"cache_read_input_tokens":250000,"cache_creation_input_tokens":90,"output_tokens":20},"content":[{"type":"thinking","thinking":"Look"}]}})" "\n"
               R"({"type":"assistant","uuid":"a2","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make"}}]}})" "\n"
               R"({"type":"user","uuid":"r1","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"}]}})" "\n"
               R"({"type":"assistant","uuid":"a3","message":{"content":[{"type":"text","text":"Fixed."}]}})" "\n"
               R"({"type":"system","subtype":"turn_duration","durationMs":65000})" "\n"
               R"({"type":"user","uuid":"p2","message":{"content":"Now test"}})" "\n"
               R"({"type":"assistant","uuid":"a4","message":{"content":[{"type":"tool_use","id":"t2","name":"Bash","input":{"command":"ctest"}}]}})" "\n");
    file.flush();
    TranscriptModel model;
    model.setPath(path);
    model.loadNow();
    const auto kind = [&model](int row) { return model.data(model.index(row), TranscriptModel::KindRole).toString(); };
    require(model.rowCount() == 5 && kind(0) == QStringLiteral("prompt") && kind(1) == QStringLiteral("work") &&
                kind(2) == QStringLiteral("reply") && kind(3) == QStringLiteral("prompt") &&
                kind(4) == QStringLiteral("tool"),
            "finished turn folds; running turn shows its step");
    require(model.data(model.index(1), TranscriptModel::TitleRole).toString() == QStringLiteral("Worked for 1m 5s") &&
                model.data(model.index(1), TranscriptModel::StepsRole).toInt() == 2 &&
                model.data(model.index(1), TranscriptModel::DetailRole).toString() == QStringLiteral("ran a command"),
            "work row says how long and what it did");
    model.toggleWork(1);
    require(model.rowCount() == 7 && kind(2) == QStringLiteral("thinking") && kind(3) == QStringLiteral("tool") &&
                kind(4) == QStringLiteral("reply") &&
                model.data(model.index(3), TranscriptModel::InWorkRole).toBool() &&
                model.data(model.index(1), TranscriptModel::ExpandedRole).toBool(),
            "opening shows thinking and steps above the reply");
    model.toggleWork(1);
    require(model.rowCount() == 5, "folding hides them again");
    require(model.model() == QStringLiteral("claude-opus-5-5") && model.effort() == QStringLiteral("high") &&
                model.contextTokens() == 250120 && model.contextWindow() == 1000000,
            "status: model, effort, context use and window");
    // The running turn finishes: it folds too, without a reload.
    int removed = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [] { require(false, "no reset on append"); });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, [&removed] { ++removed; });
    file.write(R"({"type":"user","uuid":"r2","message":{"content":[{"type":"tool_result","tool_use_id":"t2","content":"pass"}]}})" "\n"
               R"({"type":"assistant","uuid":"a5","message":{"content":[{"type":"text","text":"All pass."}]}})" "\n"
               R"({"type":"system","subtype":"turn_duration","durationMs":3000})" "\n");
    file.flush();
    model.readMore();
    require(model.rowCount() == 6 && kind(4) == QStringLiteral("work") && kind(5) == QStringLiteral("reply") &&
                removed == 1,
            "a turn that finishes folds in place");
    require(lapis::desktop::transcript::context_window(QStringLiteral("claude-opus-4-5"), 1000) == 200000 &&
                lapis::desktop::transcript::context_window(QStringLiteral("claude-opus-4-5[1m]"), 0) == 1000000,
            "context windows follow the model");
}
// Code colors by its language: the role at a column of a line.
void code_is_highlighted() {
    namespace code = lapis::desktop::code;
    const auto role_at = [](const QString& language, const QString& line, int column, int state = 0) {
        for (const auto& span : code::tokenize(line, code::language(language), state))
            if (column >= span.start && column < span.start + span.length)
                return span.role;
        return code::Role::plain;
    };
    const QString cpp = QStringLiteral("int count = run(\"x\", 42); // done");
    require(role_at(QStringLiteral("cpp"), cpp, 0) == code::Role::type, "C++ type");
    require(role_at(QStringLiteral("cpp"), cpp, 4) == code::Role::plain, "C++ variable stays plain");
    require(role_at(QStringLiteral("cpp"), cpp, 12) == code::Role::function, "C++ call");
    require(role_at(QStringLiteral("cpp"), cpp, 17) == code::Role::string, "C++ string");
    require(role_at(QStringLiteral("cpp"), cpp, 21) == code::Role::number, "C++ number");
    require(role_at(QStringLiteral("cpp"), cpp, 27) == code::Role::comment, "C++ comment");
    require(role_at(QStringLiteral("cpp"), QStringLiteral("#include <vector>"), 1) == code::Role::meta,
            "preprocessor line");
    int state = 0;
    (void)code::tokenize(QStringLiteral("x = \"\"\"start"), code::language(QStringLiteral("py")), state);
    require(state != 0 && role_at(QStringLiteral("py"), QStringLiteral("still inside"), 2, state) == code::Role::string,
            "a triple-quoted string carries to the next line");
    require(role_at(QStringLiteral("py"), QStringLiteral("def wrap(text):"), 5) == code::Role::function &&
                role_at(QStringLiteral("py"), QStringLiteral("def wrap(text):"), 0) == code::Role::keyword,
            "Python def and its name");
    const QString shell = QStringLiteral("cmake --build $DIR | rg error");
    require(role_at(QStringLiteral("bash"), shell, 0) == code::Role::function &&
                role_at(QStringLiteral("bash"), shell, 7) == code::Role::parameter &&
                role_at(QStringLiteral("bash"), shell, 15) == code::Role::property &&
                role_at(QStringLiteral("bash"), shell, 22) == code::Role::function,
            "shell command, flag, variable and piped command");
    require(role_at(QStringLiteral("json"), QStringLiteral("{\"size\": 0, \"all\": false}"), 2) == code::Role::property &&
                role_at(QStringLiteral("json"), QStringLiteral("{\"size\": 0, \"all\": false}"), 20) == code::Role::constant,
            "JSON key and constant");
    require(role_at(QStringLiteral("diff"), QStringLiteral("+added"), 2) == code::Role::added &&
                role_at(QStringLiteral("diff"), QStringLiteral("-gone"), 2) == code::Role::removed,
            "diff lines");
    require(role_at(QStringLiteral("unknown-lang"), QStringLiteral("x = \"s\" # note"), 5) == code::Role::string,
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
    const auto text = QStringLiteral("See ![chart](%1) and the report at %2.\nMissing: /nope/x.png\n"
                                     "Clip: [clip](<%3>) and `%2` again.")
                          .arg(chart, page, clip);
    const auto found = lapis::desktop::transcript::find_media(text);
    require(found.size() == 3 && found[0].path == chart && found[0].kind == QStringLiteral("image") &&
                found[1].path == clip && found[1].kind == QStringLiteral("video") && found[2].path == page &&
                found[2].kind == QStringLiteral("html"),
            "Markdown targets (spaces inside <>) then bare paths, each once; missing files are skipped");
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
    require(find(QStringLiteral("team:ship")).value(QStringLiteral("description")) == QStringLiteral("Ship the release") &&
                find(QStringLiteral("notes")).value(QStringLiteral("description")) == QStringLiteral("Keep notes"),
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
                text.contains(QStringLiteral("text.color: eeeeee")) && text.contains(QStringLiteral("'7aa2f7'")) &&
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
