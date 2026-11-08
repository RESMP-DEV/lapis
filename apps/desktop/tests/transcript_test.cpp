// The Claude transcript reader and its model: conversation records become
// items, tool results attach to their calls, bookkeeping is skipped, unknown
// blocks stay visible, and the model follows a file the CLI is still writing.
#include "transcript.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
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
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    reader_folds_records();
    result_reports_changed_card();
    model_follows_file();
    reply_splits_at_code();
    items_keep_their_record();
    if (failures == 0)
        std::puts("transcript: all checks passed");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
