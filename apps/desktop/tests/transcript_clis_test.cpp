// The visual view's readers for CLIs other than Claude Code, against the
// synthetic conversations in tests/visual: each record type becomes the
// thread's items, turns fold and are timed, the toolbar's model, effort and
// context follow, unknown records are ignored or kept as other blocks, and a
// file still being written is followed line by line.
#include "transcript.hpp"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <cstdio>

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
                items[10].result == "Flags are parsed in main()." &&
                items[10].duration_ms == 5000,
            "Codex: an MCP tool call");
    require(items[11].title == "Searched the web for" && items[11].body == "gtest flag parsing" &&
                items[11].result.contains("https://example.com/flags"),
            "Codex: a web search and its results");
    require(items[12].title == "Waited" && items[12].body == "30s", "Codex: a sleep");
    require(items[13].title == "Viewed image" &&
                items[13].body == "/work/demo/docs/screenshot.png",
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
                items[17].body == "Also check the footer",
            "Codex: a message sent mid-turn stays in that turn");
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
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    formats_are_told_apart();
    codex_items();
    codex_bad_lines_are_ignored();
    codex_file_is_followed();
    if (failures == 0)
        std::puts("transcript_clis_test: PASS");
    return failures == 0 ? 0 : 1;
}
