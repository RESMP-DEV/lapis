// Renders a Claude Code conversation in the visual view, offscreen with the
// software scene graph, and saves one contact sheet: the latest items, the
// latest edit and the latest failed tool side by side. --bench also scrolls
// the whole conversation and reports frame times. No window is shown and the
// process exits when done.
//
//   lapis_transcript_capture --jsonl FILE --out SHEET.png [--bench]
#include "transcript.hpp"

#include <QColor>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QWheelEvent>
#include <algorithm>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#endif
#include <cstdio>
#include <vector>

using lapis::desktop::TranscriptModel;
using lapis::desktop::transcript::ClaudeReader;
using lapis::desktop::transcript::Kind;

namespace {
void settle() {
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

QImage shot(QQuickView& view, QObject* list, int index) {
    if (index < 0)
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
    else
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index),
                                  Q_ARG(int, 3 /* ListView.Center */));
    settle();
    // A second pass once delegates have their real heights.
    if (index >= 0)
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 3));
    else
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
    settle();
    return view.grabWindow();
}

// The bar edits as the terminal does on a Mac (lapis keeps Command as Meta),
// Return sends and Shift-Return adds a line. Arrows carry the
// keypad flag, as AppKit sends them.
int check_keys(QQuickView& view, QQuickItem* root) {
    auto* input = root->findChild<QQuickItem*>(QStringLiteral("visualComposer"));
    if (input == nullptr) {
        std::fputs("keys: no composer\n", stderr);
        return 1;
    }
    QJSValue sent = view.engine()->evaluate(QStringLiteral("(function(t) { return true })"));
    root->setProperty("submit", QVariant::fromValue(sent));
    input->forceActiveFocus();
    settle();
    int failures = 0;
    const auto key = [&view](int code, Qt::KeyboardModifiers mods, const QString& text = {}) {
        const bool arrow = code >= Qt::Key_Left && code <= Qt::Key_Down;
        if (arrow)
            mods |= Qt::KeypadModifier;
        QKeyEvent press(QEvent::KeyPress, code, mods, text);
        QKeyEvent release(QEvent::KeyRelease, code, mods, text);
        QCoreApplication::sendEvent(&view, &press);
        QCoreApplication::sendEvent(&view, &release);
        settle();
    };
    const auto expect = [&](const char* what, const QString& text, int cursor) {
        const auto got = input->property("text").toString();
        const int at = input->property("cursorPosition").toInt();
        if (got != text || (cursor >= 0 && at != cursor)) {
            std::fprintf(stderr, "keys FAIL %s: got \"%s\" at %d\n", what,
                         qPrintable(QString(got).replace(QLatin1Char('\n'), QStringLiteral("\\n"))),
                         at);
            ++failures;
        }
    };
    const auto set = [&](const QString& text) {
        input->setProperty("text", text);
        input->setProperty("cursorPosition", static_cast<int>(text.size()));
    };
    set(QStringLiteral("alpha beta.gamma\nsecond line"));
    key(Qt::Key_Left, Qt::MetaModifier);
    expect("Command-Left goes to the line start", QStringLiteral("alpha beta.gamma\nsecond line"),
           17);
    key(Qt::Key_Right, Qt::AltModifier);
    expect("Option-Right ends the word", QStringLiteral("alpha beta.gamma\nsecond line"), 23);
    key(Qt::Key_Backspace, Qt::AltModifier);
    expect("Option-Backspace deletes the word", QStringLiteral("alpha beta.gamma\n line"), 17);
    key(Qt::Key_Backspace, Qt::MetaModifier);
    expect("Command-Backspace at a line start joins it", QStringLiteral("alpha beta.gamma line"),
           16);
    key(Qt::Key_Left, Qt::AltModifier);
    expect("Option-Left stops at the word start", QStringLiteral("alpha beta.gamma line"), 11);
    key(Qt::Key_Delete, Qt::MetaModifier);
    expect("Command-Delete deletes to the line end", QStringLiteral("alpha beta."), 11);
    key(Qt::Key_Left, Qt::MetaModifier | Qt::ShiftModifier);
    if (input->property("selectedText").toString() != QStringLiteral("alpha beta.")) {
        std::fputs("keys FAIL Shift-Command-Left selects to the line start\n", stderr);
        ++failures;
    }
    key(Qt::Key_Right, Qt::MetaModifier);
    key(Qt::Key_W, Qt::ControlModifier);
    expect("Control-W deletes the word", QStringLiteral("alpha "), 6);
    key(Qt::Key_Return, Qt::ShiftModifier, QStringLiteral("\r"));
    expect("Shift-Return adds a plain newline", QStringLiteral("alpha \n"), 7);
    key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    expect("Return sends", QString(), -1);
    set(QStringLiteral("hi"));
    key(Qt::Key_Return, Qt::MetaModifier, QStringLiteral("\r"));
    expect("Command-Return sends too", QString(), -1);
    // Typing a slash command lists the best matches; Return fills one in.
    root->setProperty("harness", QStringLiteral("claude"));
    root->setProperty("slashCommands", true);
    for (const auto letter : QStringLiteral("/comp"))
        key(letter == QLatin1Char('/') ? Qt::Key_Slash : Qt::Key_A + (letter.unicode() - 'a'),
            Qt::NoModifier, QString(letter));
    const auto matches = root->property("slashMatches").toList();
    if (matches.isEmpty() ||
        matches.front().toMap().value(QStringLiteral("name")) != QStringLiteral("compact")) {
        std::fputs("keys FAIL \"/comp\" lists compact first\n", stderr);
        ++failures;
    }
    key(Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
    expect("Tab fills in the chosen command", QStringLiteral("/compact "), 9);
    set(QString());
    for (const auto letter : QStringLiteral("/comp"))
        key(letter == QLatin1Char('/') ? Qt::Key_Slash : Qt::Key_A + (letter.unicode() - 'a'),
            Qt::NoModifier, QString(letter));
    key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    expect("Return runs the chosen command", QString(), -1);
    if (root->property("pendingCommand").toString() != QStringLiteral("/compact")) {
        std::fputs("keys FAIL a sent command shows as running\n", stderr);
        ++failures;
    }
    root->setProperty("pendingCommand", QString());
    // A Codex agent's bar lists Codex's own commands.
    root->setProperty("harness", QStringLiteral("codex"));
    for (const auto letter : QStringLiteral("/stat"))
        key(letter == QLatin1Char('/') ? Qt::Key_Slash : Qt::Key_A + (letter.unicode() - 'a'),
            Qt::NoModifier, QString(letter));
    const auto codex = root->property("slashMatches").toList();
    if (codex.isEmpty() ||
        codex.front().toMap().value(QStringLiteral("name")) != QStringLiteral("status") ||
        !codex.front().toMap().value(QStringLiteral("terminal")).toBool()) {
        std::fputs("keys FAIL a Codex agent's \"/stat\" lists /status for the terminal\n", stderr);
        ++failures;
    }
    key(Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
    expect("Tab fills in Codex's command", QStringLiteral("/status "), 8);
    // Sent to Codex, whose screen the view cannot read, a panel command or
    // an unknown one shows the terminal; /compact stays in the thread.
    const auto in_terminal = [root](const char* name) {
        QVariant shown;
        QMetaObject::invokeMethod(root, "answersInTerminal", Q_RETURN_ARG(QVariant, shown),
                                  Q_ARG(QVariant, QString::fromLatin1(name)));
        return shown.toBool();
    };
    if (!in_terminal("status") || in_terminal("compact") || !in_terminal("not-a-command")) {
        std::fputs("keys FAIL Codex commands that answer in the terminal are marked\n", stderr);
        ++failures;
    }
    set(QString());
    root->setProperty("harness", QStringLiteral("claude"));
    // A dialog in the terminal would take the keys as its own: nothing is
    // sent and the message stays in the bar.
    QJSValue dialog = view.engine()->evaluate(QStringLiteral(
        "(function() { return [\"❯ /model\", \"\", \"─\".repeat(60), \"  Select model\", "
        "\"  ❯ 1. Default\", \"  Enter to set · Esc to cancel\"] })"));
    root->setProperty("screen", QVariant::fromValue(dialog));
    set(QStringLiteral("hello"));
    key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    expect("Return over a terminal dialog keeps the message", QStringLiteral("hello"), -1);
    root->setProperty("screen", QVariant());
    set(QString());
    std::puts(failures == 0 ? "keys: all checks passed" : "keys: failed");
    return failures == 0 ? 0 : 1;
}

// A made-up running Claude Code turn for --live-checks: a prompt, then
// thinking, a search, an edit with a diff and an interim reply.
const char* const kLiveTurn[] = {
    R"({"type":"user","uuid":"p1","timestamp":"2026-11-11T16:00:00.000Z","message":{"content":"Make the tests cover it"}})",
    R"({"type":"assistant","uuid":"a1","timestamp":"2026-11-11T16:00:02.000Z","message":{"content":[{"type":"thinking","thinking":"plan"}]}})",
    R"({"type":"assistant","uuid":"a2","timestamp":"2026-11-11T16:00:03.000Z","message":{"content":[{"type":"tool_use","id":"t1","name":"Grep","input":{"pattern":"greet","path":"/work/demo"}}]}})",
    R"({"type":"user","uuid":"r1","timestamp":"2026-11-11T16:00:04.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"src/greet.cpp:3"}]}})",
    R"({"type":"assistant","uuid":"a3","timestamp":"2026-11-11T16:00:05.000Z","message":{"content":[{"type":"tool_use","id":"t2","name":"Edit","input":{"file_path":"/work/demo/tests/greet_test.cpp","old_string":"a\nb","new_string":"c\nd\ne"}}]}})",
    R"({"type":"user","uuid":"r2","timestamp":"2026-11-11T16:00:06.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"t2","content":"ok"}]}})",
    R"({"type":"assistant","uuid":"a4","timestamp":"2026-11-11T16:00:07.000Z","message":{"content":[{"type":"text","text":"Building next."}]}})",
};
// What it writes next: six more steps, then the turn's end.
const char* const kLiveMore[] = {
    R"({"type":"assistant","uuid":"a5","timestamp":"2026-11-11T16:00:08.000Z","message":{"content":[{"type":"tool_use","id":"t3","name":"Bash","input":{"command":"make"}}]}})",
    R"({"type":"user","uuid":"r3","timestamp":"2026-11-11T16:00:20.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"t3","content":"built"}]}})",
    R"({"type":"assistant","uuid":"a6","timestamp":"2026-11-11T16:00:21.000Z","message":{"content":[{"type":"tool_use","id":"t4","name":"Read","input":{"file_path":"/work/demo/a.cpp"}}]}})",
    R"({"type":"assistant","uuid":"a7","timestamp":"2026-11-11T16:00:22.000Z","message":{"content":[{"type":"tool_use","id":"t5","name":"Read","input":{"file_path":"/work/demo/b.cpp"}}]}})",
    R"({"type":"assistant","uuid":"a8","timestamp":"2026-11-11T16:00:23.000Z","message":{"content":[{"type":"tool_use","id":"t6","name":"Bash","input":{"command":"ctest"}}]}})",
    R"({"type":"user","uuid":"r6","timestamp":"2026-11-11T16:00:30.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"t6","content":"passed"}]}})",
};
const char* const kLiveEnd[] = {
    R"({"type":"assistant","uuid":"a9","timestamp":"2026-11-11T16:00:31.000Z","message":{"content":[{"type":"text","text":"Done."}]}})",
    R"({"type":"system","subtype":"turn_duration","durationMs":31000})",
};

// Items named `name` shown (visible, with some height) in the thread's
// rows. Rows that left wait in the list's reuse pool, so only the rows the
// list holds for its model's indexes count.
std::vector<QQuickItem*> named(QQuickItem* root, const char* name) {
    std::vector<QQuickItem*> out;
    const auto walk = [&out, name](auto&& self, QQuickItem* item) -> void {
        if (!item->isVisible())
            return;
        if (item->objectName() == QLatin1String(name) && item->height() > 0)
            out.push_back(item);
        for (auto* child : item->childItems())
            self(self, child);
    };
    auto* list = root->property("list").value<QQuickItem*>();
    const int count = list->property("count").toInt();
    for (int row = 0; row < count; ++row) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, row));
        if (item != nullptr)
            walk(walk, item);
    }
    return out;
}

void click(QQuickView& view, QQuickItem* item) {
    const auto at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, at, at, view.mapToGlobal(at.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, at, at, view.mapToGlobal(at.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&view, &press);
    QCoreApplication::sendEvent(&view, &release);
    settle();
}

// The live work box: a fixed height as steps arrive, scrolled by the wheel
// without growing, a step opened and closed by clicks, and no diff drawn
// until its step is opened, live or in an opened "Worked for".
int check_live(QQuickView& view, QQuickItem* root, TranscriptModel& model, QFile& file) {
    int failures = 0;
    const auto expect = [&failures](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "live FAIL %s\n", what);
            ++failures;
        }
    };
    auto* list = root->property("list").value<QQuickItem*>();
    const auto end = [&] {
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        settle();
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        settle();
    };
    root->setProperty("working", true);
    end();
    auto boxes = named(root, "liveBox");
    expect(boxes.size() == 1, "one live box while the turn runs");
    if (boxes.size() != 1)
        return 1;
    const auto height = boxes.front()->height();
    expect(named(root, "diffCard").empty(), "no diff drawn before a step is opened");
    expect(named(root, "step").empty(), "no step rows in the thread while it runs");
    for (const char* line : kLiveMore)
        file.write(QByteArray(line) + '\n');
    file.flush();
    model.readMore();
    end();
    boxes = named(root, "liveBox");
    auto* inner = boxes.empty() ? nullptr : boxes.front()->findChild<QQuickItem*>("liveList");
    expect(boxes.size() == 1 && boxes.front()->height() == height,
           "the box keeps its height as steps arrive");
    expect(inner != nullptr && model.liveStepsModel().rowCount() == 8 &&
               inner->property("atYEnd").toBool(),
           "the box lists every step and follows the newest");
    if (inner == nullptr)
        return 1;
    // The wheel scrolls back through the turn inside the box.
    const auto before = inner->property("contentY").toReal();
    const auto centre =
        boxes.front()->mapToScene(QPointF(boxes.front()->width() / 2, boxes.front()->height() / 2));
    QWheelEvent wheel(centre, view.mapToGlobal(centre.toPoint()), QPoint(0, 60), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&view, &wheel);
    settle();
    expect(inner->property("contentY").toReal() < before && !inner->property("follow").toBool() &&
               boxes.front()->height() == height,
           "the wheel scrolls back in the box, which does not grow");
    // Clicking the edit opens it under the box with its diff; again closes.
    QQuickItem* edit_line = nullptr;
    const auto lines = [&] {
        std::vector<QQuickItem*> out;
        for (auto* child : inner->property("contentItem").value<QQuickItem*>()->childItems())
            if (child->property("item").isValid())
                out.push_back(child);
        return out;
    };
    QMetaObject::invokeMethod(inner, "positionViewAtBeginning");
    settle();
    for (auto* line : lines())
        if (line->property("title").toString() == QLatin1String("Edited"))
            edit_line = line;
    expect(edit_line != nullptr, "the edit is a line of the box");
    if (edit_line == nullptr)
        return 1;
    click(view, edit_line);
    expect(root->property("liveOpenItem").toInt() == edit_line->property("item").toInt() &&
               named(root, "diffCard").size() == 1 && boxes.front()->height() == height,
           "clicking a step opens its diff under the box");
    click(view, edit_line);
    expect(root->property("liveOpenItem").toInt() == -1 && named(root, "diffCard").empty(),
           "clicking it again closes it");
    // The turn ends: the box goes, the work folds, its steps open one by one.
    for (const char* line : kLiveEnd)
        file.write(QByteArray(line) + '\n');
    file.flush();
    model.readMore();
    root->setProperty("working", false);
    end();
    expect(named(root, "liveBox").empty(), "the box goes when the turn ends");
    int work = -1;
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.data(model.index(row), TranscriptModel::KindRole).toString() ==
            QLatin1String("work"))
            work = row;
    expect(work >= 0, "the finished turn folds to Worked for");
    if (work < 0)
        return 1;
    model.toggleWork(work);
    end();
    const auto steps = named(root, "step");
    expect(steps.size() >= 6 && named(root, "diffCard").empty(),
           "an opened Worked for lists one-line steps, no diff");
    for (auto* step : steps)
        if (step->property("edit").toBool())
            step->setProperty("open", true);
    settle();
    expect(named(root, "diffCard").size() == 1, "an opened edit step shows its diff");
    std::puts(failures == 0 ? "live: all checks passed" : "live: failed");
    return failures == 0 ? 0 : 1;
}

// Messages Qt and QML log while the view is checked.
QStringList& logged() {
    static QStringList messages;
    return messages;
}

// This process's memory footprint now, in MB, as Activity Monitor counts it.
double footprint_mb() {
#ifdef __APPLE__
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS)
        return static_cast<double>(info.phys_footprint) / (1024.0 * 1024.0);
#endif
    return 0;
}

// A record's type for counting, never its text: "type", or "type/subtype"
// with the CLI's own subtype field (subtype, operation, payload type,
// attachment type).
QString record_type(const QJsonObject& record) {
    auto type = record.value(QLatin1String("type")).toString();
    if (type.isEmpty())
        type = record.value(QLatin1String("source")).toString(); // agy steps
    if (record.value(QLatin1String("isSidechain")).toBool())
        type.prepend(QStringLiteral("subagent "));
    for (const char* field : {"subtype", "operation"})
        if (const auto sub = record.value(QLatin1String(field)).toString(); !sub.isEmpty())
            return type + QLatin1Char('/') + sub;
    for (const char* field : {"payload", "attachment"})
        if (const auto sub = record.value(QLatin1String(field))
                                 .toObject()
                                 .value(QLatin1String("type"))
                                 .toString();
            !sub.isEmpty())
            return type + QLatin1Char('/') + sub;
    // A message by its shape: its blocks' types, or a leading tag's name.
    const auto content = record.value(QLatin1String("message")).toObject().value("content");
    if (content.isString()) {
        const auto text = content.toString();
        static const QRegularExpression tag(QStringLiteral(R"(^<([a-z_-]{1,40})>)"));
        const auto match = tag.match(text);
        return type +
               (record.value(QLatin1String("isMeta")).toBool() ? QStringLiteral(" meta")
                                                               : QString()) +
               (match.hasMatch()           ? QStringLiteral(" <%1>").arg(match.captured(1))
                : text.trimmed().isEmpty() ? QStringLiteral(" empty text")
                                           : QStringLiteral(" text"));
    }
    if (content.isArray()) {
        QStringList blocks;
        for (const auto& block : content.toArray())
            blocks << block.toObject().value(QLatin1String("type")).toString();
        blocks.removeDuplicates();
        return type + QStringLiteral(" [") + blocks.join(QLatin1Char(',')) + QLatin1Char(']');
    }
    return type.isEmpty() ? QStringLiteral("(no type)") : type;
}

// Structural checks over a conversation: numbers and type names only,
// never its text. Reads the file read-only.
struct Check {
    int problems{};
    void line(const char* name, qint64 value, bool problem = false) {
        std::printf("  %-34s %lld%s\n", name, static_cast<long long>(value),
                    problem && value > 0 ? "   <-- problem" : "");
        if (problem && value > 0)
            ++problems;
    }
};

int check_structure(const QString& path, const TranscriptModel& model, Check& check) {
    using lapis::desktop::transcript::make_reader;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "could not read the file\n");
        return 2;
    }
    check.line("file bytes", file.size());
    // A reader of its own, fed line by line, to see which records it took
    // nothing from; timed on the reading alone.
    auto reader = make_reader(model.format());
    qint64 records = 0, parse_errors = 0, duplicates = 0;
    QSet<QString> ids;
    QMap<QString, int> ignored;
    double parse_ns = 0;
    while (!file.atEnd()) {
        const auto line = file.readLine();
        if (line.trimmed().isEmpty())
            continue;
        ++records;
        QElapsedTimer timer;
        timer.start();
        const int changed = reader->add(line);
        parse_ns += static_cast<double>(timer.nsecsElapsed());
        const auto document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            ++parse_errors;
            continue;
        }
        const auto record = document.object();
        const auto id = record.value(QLatin1String("uuid")).toString();
        if (!id.isEmpty()) {
            if (ids.contains(id))
                ++duplicates;
            ids.insert(id);
        }
        if (changed < 0)
            ignored[record_type(record)] += 1;
    }
    check.line("records", records);
    check.line("unparsable lines", parse_errors, true);
    check.line("duplicate record ids", duplicates);
    std::printf("  %-34s %.0f\n", "reader ms (parse + fold)", parse_ns / 1e6);
    std::printf("  records that made no item, by type:\n");
    for (auto entry = ignored.cbegin(); entry != ignored.cend(); ++entry)
        std::printf("    %7d %s\n", entry.value(), qPrintable(entry.key()));

    // What the items say.
    const auto& items = model.items();
    QMap<QString, int> kinds;
    qint64 empty = 0, open_fences = 0, image_refs = 0, images_gone = 0, other = 0;
    QMap<QString, int> markup;
    static const QRegularExpression image_ref(QStringLiteral(R"(\[Image #\d+\])"));
    static const QRegularExpression box_row(QStringLiteral("^\\s*[┌├└│╭╰║╔╚].*[┐┤┘│╮╯║╗╝]\\s*$"));
    const QStringList tags{QStringLiteral("<system-reminder>"),
                           QStringLiteral("<task-notification>"),
                           QStringLiteral("<local-command-stdout>"),
                           QStringLiteral("<local-command-stderr>"),
                           QStringLiteral("<local-command-caveat>"),
                           QStringLiteral("<command-name>"),
                           QStringLiteral("<command-message>"),
                           QStringLiteral("<command-args>"),
                           QStringLiteral("<bash-input>"),
                           QStringLiteral("<bash-stdout>"),
                           QStringLiteral("<user-prompt-submit-hook>"),
                           QStringLiteral("<tool-use-error>"),
                           QStringLiteral("<pasted_content")};
    for (const auto& item : items) {
        const auto kind = lapis::desktop::transcript::kind_name(item.kind);
        kinds[kind] += 1;
        if (item.kind == Kind::other)
            ++other;
        const bool text_kind = item.kind == Kind::prompt || item.kind == Kind::reply ||
                               item.kind == Kind::notice || item.kind == Kind::command ||
                               item.kind == Kind::divider;
        if (text_kind && item.body.trimmed().isEmpty() && item.title.trimmed().isEmpty())
            ++empty;
        for (const auto* text : {&item.title, &item.body, &item.result}) {
            if (item.kind == Kind::tool && text != &item.result)
                continue; // a command or path the agent wrote
            // A step's command or output can hold any text (a grep of these
            // very files); only what the CLI itself adds counts there.
            for (const auto& tag : tags)
                if ((item.kind != Kind::tool || tag == QLatin1String("<system-reminder>")) &&
                    text->contains(tag))
                    markup[kind + QStringLiteral(" ") + tag] += 1;
            if (text->contains(QChar(0x1b)))
                markup[kind + QStringLiteral(" escape character")] += 1;
        }
        if (item.kind == Kind::reply || item.kind == Kind::prompt) {
            bool fenced = false;
            int box = 0;
            for (const auto& row : item.body.split(QLatin1Char('\n'))) {
                if (row.trimmed().startsWith(QLatin1String("```")))
                    fenced = !fenced;
                else if (!fenced && box_row.match(row).hasMatch())
                    ++box;
            }
            if (fenced)
                ++open_fences;
            if (box >= 2)
                markup[kind + QStringLiteral(" box-drawn rows outside code")] += 1;
        }
        if (item.kind == Kind::prompt && image_ref.match(item.body).hasMatch()) {
            ++image_refs;
            if (model.media(item.body + QLatin1Char('\n') + item.detail).isEmpty())
                ++images_gone;
        }
    }
    std::printf("  items by kind:");
    for (auto entry = kinds.cbegin(); entry != kinds.cend(); ++entry)
        std::printf(" %s %d", qPrintable(entry.key()), entry.value());
    std::printf("\n");
    check.line("turns", model.turnCount());
    check.line("rows", model.rowCount());
    check.line("other (unknown) blocks", other);
    check.line("items with empty text", empty, true);
    check.line("replies with an unclosed fence", open_fences);
    check.line("prompts naming [Image #N]", image_refs);
    check.line("  of which no image file to show", images_gone);
    for (auto entry = markup.cbegin(); entry != markup.cend(); ++entry) {
        std::printf("  raw markup: %s %d   <-- problem\n", qPrintable(entry.key()), entry.value());
        ++check.problems;
    }
    // Turns the CLI never ended with a duration record (closed only by the
    // next turn), and turns with no final reply; the last may be running.
    const auto layout = lapis::desktop::transcript::lay_out(items, {});
    qint64 unended = 0, replyless = 0, last_open = 0;
    for (size_t t = 0; t < layout.turns.size(); ++t) {
        const auto& turn = layout.turns[t];
        if (t + 1 == layout.turns.size()) {
            last_open = turn.closed ? 0 : 1;
            continue;
        }
        if (items[static_cast<size_t>(turn.last)].kind != Kind::turn)
            ++unended;
        if (turn.final_reply < 0)
            ++replyless;
    }
    check.line("turns ended by no duration record", unended);
    check.line("turns with no final reply", replyless);
    check.line("last turn open", last_open);
    return 0;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty())
        return 0;
    std::ranges::sort(values);
    const auto at = static_cast<size_t>(p * static_cast<double>(values.size() - 1));
    return values[at];
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic"); // as the app sets it
    // The view writes a chart style under ~/.lapis; a capture keeps it in a
    // throwaway home (or LAPIS_CAPTURE_HOME, to look at what it wrote).
    QTemporaryDir home;
    const auto chosen_home = qgetenv("LAPIS_CAPTURE_HOME");
    qputenv("HOME", chosen_home.isEmpty() ? home.path().toUtf8() : chosen_home);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"jsonl", "Conversation file (Claude Code, Codex or Antigravity)", "file"});
    parser.addOption({"format",
                      "Read the file as this CLI's: claude, codex or antigravity (default: by "
                      "its name)",
                      "name"});
    parser.addOption(
        {"work", "Fold and open the N-th \"Worked for\" row (default: the latest)", "n", "0"});
    parser.addOption({"census", "Print how many items of each kind and step the file makes "
                                "(counts and step names only), then exit"});
    parser.addOption({"out", "Contact sheet PNG", "file"});
    parser.addOption({"width", "Panel width", "px", "900"});
    parser.addOption({"height", "Panel height", "px", "1200"});
    parser.addOption({"font-size", "Text size", "px", "15"});
    parser.addOption({"theme", "Visual theme name", "name", "terminal"});
    parser.addOption({"font", "Reading font name", "name", "system"});
    parser.addOption({"code-font", "Code font name", "name", "system"});
    parser.addOption(
        {"terminal-background", "Background the terminal theme matches", "color", "#0f131c"});
    parser.addOption({"suggestion", "A Tab suggestion shown in the empty bar", "text", ""});
    parser.addOption({"bench", "Scroll the whole conversation and report frame times"});
    parser.addOption({"step", "Pixels scrolled per benchmark step", "px", "240"});
    parser.addOption({"keys", "Check the bar's Mac line editing and send keys, then exit"});
    parser.addOption({"live-checks",
                      "Check the live work box against a made-up running turn (fixed height, "
                      "steps opening, diffs only when opened), then exit"});
    parser.addOption({"panel", "Save only the panel whose caption starts with this", "caption"});
    parser.addOption(
        {"live", "Show a running turn whose terminal screen is this text file", "file"});
    parser.addOption(
        {"play", "Press play on the audio and video shown (it plays aloud) and report progress"});
    parser.addOption({"wait", "Milliseconds to let the view settle (media to open) before shooting",
                      "ms", "400"});
    parser.addOption({"check",
                      "Check the conversation without its text: structure, raw markup the view "
                      "left, load time and memory, QML warnings, and the first, last and three "
                      "middle screens laid out; exits 1 on a problem"});
    parser.addOption(
        {"out-dir", "With --check, save the five screens as PNGs in this folder", "folder"});
    parser.addOption({"list", "Print each row's index, kind and shape (no text), then exit"});
    parser.addOption({"rows",
                      "Shoot only these rows (indexes from --list, comma separated; N+ opens "
                      "the \"Worked for\" row N)",
                      "list"});
    parser.process(app);
    const bool keys = parser.isSet("keys");
    const bool live_checks = parser.isSet("live-checks");
    const bool checking = parser.isSet("check");
    // --live-checks writes its own made-up running turn and appends to it.
    QFile live_file(home.filePath(QStringLiteral("live.jsonl")));
    if (live_checks) {
        if (!live_file.open(QIODevice::WriteOnly)) {
            std::fputs("could not write the live fixture\n", stderr);
            return 2;
        }
        for (const char* line : kLiveTurn)
            live_file.write(QByteArray(line) + '\n');
        live_file.flush();
    }
    if (!keys && !live_checks &&
        (!parser.isSet("jsonl") ||
         (!parser.isSet("out") && !parser.isSet("census") && !parser.isSet("list") && !checking))) {
        std::fputs("--jsonl and --out are required\n", stderr);
        return 2;
    }
    if (checking)
        qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& text) {
            if (type != QtDebugMsg && type != QtInfoMsg &&
                !text.startsWith(QLatin1String("Populating font family aliases")))
                logged() << text;
        });

    QElapsedTimer load_timer;
    load_timer.start();
    TranscriptModel model;
    if (parser.isSet("format")) {
        const auto format = lapis::desktop::transcript::format_named(parser.value("format"));
        if (!format) {
            std::fputs("--format takes claude, codex or antigravity\n", stderr);
            return 2;
        }
        model.setFormat(format);
    }
    model.setPath(live_checks ? live_file.fileName() : parser.value("jsonl"));
    model.loadNow();
    const auto load_ms = load_timer.elapsed();
    std::printf("format %s\n", qPrintable(model.formatName()));
    Check check;
    if (checking) {
        // setPath also starts the app's own background read; let it end.
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        std::printf("check\n  %-34s %lld\n  %-34s %.0f\n", "model load ms (read + lay out)",
                    static_cast<long long>(load_ms), "memory after load MB", footprint_mb());
        if (const int failed = check_structure(parser.value("jsonl"), model, check); failed != 0)
            return failed;
    }
    if (parser.isSet("list")) {
        // Rows by shape: a step's verb, a divider's or command's fixed name,
        // and marks for tables, code, pasted images and folded steps.
        for (int row = 0; row < model.rowCount(); ++row) {
            const auto index = model.index(row);
            const auto kind = model.data(index, TranscriptModel::KindRole).toString();
            const auto body = model.data(index, TranscriptModel::BodyRole).toString();
            QStringList marks;
            if (kind == QLatin1String("work"))
                marks << QStringLiteral("%1 steps")
                             .arg(model.data(index, TranscriptModel::StepsRole).toInt());
            if (kind == QLatin1String("tool") || kind == QLatin1String("divider") ||
                kind == QLatin1String("command"))
                marks << model.data(index, TranscriptModel::TitleRole).toString().left(40);
            if (body.contains(QLatin1String("|---")) || body.contains(QLatin1String("| ---")))
                marks << QStringLiteral("table");
            if (body.contains(QLatin1String("```")))
                marks << QStringLiteral("code");
            if (kind == QLatin1String("prompt") &&
                !model.data(index, TranscriptModel::DetailRole).toString().isEmpty())
                marks << QStringLiteral("pasted image");
            std::printf("%6d %-8s %s\n", row, qPrintable(kind), qPrintable(marks.join(", ")));
        }
        return 0;
    }
    if (parser.isSet("census")) {
        // What the reader made of a file, without any of its text: kinds,
        // and for steps, dividers and unknown blocks their fixed names.
        QMap<QString, int> counts;
        for (const auto& item : model.items()) {
            const auto kind = lapis::desktop::transcript::kind_name(item.kind);
            const bool named = item.kind == Kind::tool || item.kind == Kind::divider ||
                               item.kind == Kind::other || item.kind == Kind::turn;
            const auto title = item.kind == Kind::turn ? QStringLiteral("Worked for") : item.title;
            counts[named ? kind + QStringLiteral(" ") + title : kind] += 1;
            if (item.failed)
                counts[kind + QStringLiteral(" failed")] += 1;
        }
        for (auto count = counts.cbegin(); count != counts.cend(); ++count)
            std::printf("%6d %s\n", count.value(), qPrintable(count.key()));
        std::printf("rows %d, turns %d\n", model.rowCount(), model.turnCount());
        return 0;
    }

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(parser.value("width").toInt(), parser.value("height").toInt());
    view.setSource(QUrl::fromLocalFile(QStringLiteral(LAPIS_TRANSCRIPT_QML)));
    if (view.status() != QQuickView::Ready || view.rootObject() == nullptr) {
        for (const auto& error : view.errors())
            std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
        return 1;
    }
    auto* root = view.rootObject();
    root->setProperty("sizeSetting", parser.value("font-size").toInt());
    root->setProperty("themeName", parser.value("theme"));
    root->setProperty("fontName", parser.value("font"));
    root->setProperty("codeFontName", parser.value("code-font"));
    root->setProperty("terminalBackground", QColor(parser.value("terminal-background")));
    root->setProperty("suggestion", parser.value("suggestion"));
    // The choices the app passes for a Claude Code agent, so the menus'
    // chevrons draw.
    root->setProperty("models", QVariantList{QVariantMap{
                                    {QStringLiteral("id"), QStringLiteral("claude-opus-5-5")},
                                    {QStringLiteral("name"), QStringLiteral("Opus 5.5")}}});
    root->setProperty("efforts", QStringList{QStringLiteral("low"), QStringLiteral("medium"),
                                             QStringLiteral("high"), QStringLiteral("xhigh"),
                                             QStringLiteral("max")});
    // The offscreen platform reports generic names ("Sans Serif") whose
    // fallback spaces digits apart; name the Mac's interface fonts instead,
    // as the real window's platform resolves them.
    const auto first_of = [](QStringList wanted, QFontDatabase::SystemFont fallback) {
        const auto families = QFontDatabase::families();
        for (const auto& family : wanted)
            if (families.contains(family))
                return family;
        return QFontDatabase::systemFont(fallback).family();
    };
    const auto sans =
        first_of({QStringLiteral("SF Pro Text"), QStringLiteral("SF Pro"),
                  QStringLiteral(".AppleSystemUIFont"), QStringLiteral("Helvetica Neue")},
                 QFontDatabase::GeneralFont);
    const auto mono =
        first_of({QStringLiteral("SF Mono"), QStringLiteral("Menlo")}, QFontDatabase::FixedFont);
    root->setProperty("systemFamily", sans);
    root->setProperty("terminalFamily", mono);
    std::printf("fonts: sans %s, mono %s\n", qPrintable(sans), qPrintable(mono));
    root->setProperty("model", QVariant::fromValue(static_cast<QObject*>(&model)));
    auto* list = root->property("list").value<QObject*>();
    if (parser.isSet("live")) {
        // A turn in progress: Stop in place of Send, and the screen's rows
        // above the input box streaming under "Working for".
        QFile screen(parser.value("live"));
        if (!screen.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "could not read %s\n", qPrintable(parser.value("live")));
            return 2;
        }
        const auto rows = QString::fromUtf8(screen.readAll()).split(QLatin1Char('\n'));
        root->setProperty("interrupt", QVariant::fromValue(view.engine()->evaluate(
                                           QStringLiteral("(function () {})"))));
        // The screen goes in as the app gives it, so the view's own timer
        // reads the tail from it.
        auto screen_of = view.engine()->evaluate(
            QStringLiteral("(function (rows) { return function () { return rows } })"));
        root->setProperty(
            "screen", QVariant::fromValue(screen_of.call({view.engine()->toScriptValue(rows)})));
        root->setProperty("working", true);
    }
    view.show(); // offscreen: nothing reaches the screen
    settle();
    // Long enough for the view's short timers (the chart style it writes).
    const int wait_ms = std::max(400, parser.value("wait").toInt());
    for (QElapsedTimer waited; !waited.isValid() || waited.elapsed() < wait_ms;) {
        if (!waited.isValid())
            waited.start();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
#ifdef __APPLE__
        // The offscreen platform's event loop never drains the main dispatch
        // queue, where macOS's player reports a file opened; the app's
        // Cocoa loop does. Run it briefly so captures see what the app sees.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
#endif
    }
    if (keys)
        return check_keys(view, root);
    if (live_checks)
        return check_live(view, root, model, live_file);
    if (checking) {
        // The first screen, three evenly spaced, and the last, each timed
        // from the jump to the frame drawn.
        root->setProperty("following", false);
        const int rows = model.rowCount();
        const std::vector<std::pair<const char*, int>> positions{{"first", 0},
                                                                 {"quarter", rows / 4},
                                                                 {"half", rows / 2},
                                                                 {"three quarters", rows * 3 / 4},
                                                                 {"last", -1}};
        const auto folder = parser.value("out-dir");
        for (const auto& [name, row] : positions) {
            QElapsedTimer timer;
            timer.start();
            const auto image = rows == 0 ? view.grabWindow() : shot(view, list, row);
            std::printf("  screen %-15s row %6d  %5lld ms\n", name, row < 0 ? rows - 1 : row,
                        static_cast<long long>(timer.elapsed()));
            if (!folder.isEmpty())
                (void)image.save(QDir(folder).filePath(
                    QStringLiteral("%1.png").arg(QString::fromUtf8(name).replace(' ', '-'))));
        }
        // Every "Worked for" row opened and folded again, a few spread out.
        std::vector<int> work_rows;
        for (int row = 0; row < model.rowCount(); ++row)
            if (model.data(model.index(row), TranscriptModel::KindRole).toString() ==
                QLatin1String("work"))
                work_rows.push_back(row);
        QElapsedTimer opened;
        opened.start();
        int toggled = 0;
        for (size_t i = 0; i < work_rows.size(); i += std::max<size_t>(1, work_rows.size() / 5)) {
            const int at = work_rows[i];
            model.toggleWork(at);
            (void)shot(view, list, at + 1);
            model.toggleWork(at);
            ++toggled;
        }
        std::printf("  %-34s %d in %lld ms\n", "turns opened and folded", toggled,
                    static_cast<long long>(opened.elapsed()));
        std::printf("  %-34s %.0f\n", "memory after views MB", footprint_mb());
        check.line("Qt/QML warnings", logged().size(), true);
        // Warnings name QML files, lines and what failed there.
        QSet<QString> seen;
        for (const auto& message : logged()) {
            const auto first = message.left(220);
            if (!seen.contains(first)) {
                seen.insert(first);
                std::printf("    %s\n", qPrintable(first));
            }
        }
        std::printf("problems %d\n", check.problems);
        return check.problems == 0 ? 0 : 1;
    }
    if (parser.isSet("live")) {
        // What the running turn shows: its live box's steps, and compacting.
        std::printf("live: box shown %d, %d steps, compacting %d\n",
                    root->property("liveShown").toBool() ? 1 : 0, model.liveStepsModel().rowCount(),
                    root->property("compacting").toBool() ? 1 : 0);
    }
    // What the system player found in each audio or video file shown.
    // Delegates hang off the view's items, not its QObject children.
    std::vector<QQuickItem*> players;
    const auto find = [&players](auto&& self, QQuickItem* item) -> void {
        if (item->objectName() == QLatin1String("playback"))
            players.push_back(item);
        for (auto* child : item->childItems())
            self(self, child);
    };
    find(find, qobject_cast<QQuickItem*>(root));
    for (auto* player : players) {
        const auto media = player->property("media").toMap();
        const auto frame = player->property("frame").toSizeF();
        std::printf("playback %s: %d ms, frame %dx%d, %d frames decoded, status %d %s\n",
                    qPrintable(media.value(QStringLiteral("name")).toString()),
                    player->property("duration").toInt(), static_cast<int>(frame.width()),
                    static_cast<int>(frame.height()), player->property("frames").toInt(),
                    player->property("status").toInt(),
                    qPrintable(player->property("problem").toString()));
    }
    if (!players.empty() && parser.isSet("play")) {
        // Press play on each and see it advance. It plays aloud, as the app
        // would (muting could hide a broken output), so only when asked.
        for (auto* player : players)
            QMetaObject::invokeMethod(player, "toggle");
        for (QElapsedTimer played; !played.isValid() || played.elapsed() < 1500;) {
            if (!played.isValid())
                played.start();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
#ifdef __APPLE__
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
#endif
        }
        for (auto* player : players) {
            std::printf(
                "after play %s: playing %d, %d frames decoded\n",
                qPrintable(
                    player->property("media").toMap().value(QStringLiteral("name")).toString()),
                player->property("playing").toBool(), player->property("frames").toInt());
            QMetaObject::invokeMethod(player, "toggle");
        }
    }

    // Panels: the latest items, the latest edit, the latest failed tool.
    int edit = -1, failure = -1;
    for (int row = model.rowCount() - 1; row >= 0 && (edit < 0 || failure < 0); --row) {
        const auto index = model.index(row);
        const auto kind = model.data(index, TranscriptModel::KindRole).toString();
        if (kind != QLatin1String("tool"))
            continue;
        const auto title = model.data(index, TranscriptModel::TitleRole).toString();
        if (edit < 0 && title == QLatin1String("Edited"))
            edit = row;
        if (failure < 0 && model.data(index, TranscriptModel::FailedRole).toBool())
            failure = row;
    }
    struct Panel {
        QString caption;
        QImage image;
    };
    std::vector<Panel> panels{{QStringLiteral("Latest"), shot(view, list, -1)}};
    if (parser.isSet("rows")) {
        // Only the rows asked for (from --list), each centred; "N+" opens
        // the turn of the "Worked for" row N first.
        root->setProperty("following", false);
        panels.clear();
        for (const auto& wanted : parser.value("rows").split(QLatin1Char(','))) {
            const bool open = wanted.endsWith(QLatin1Char('+'));
            const int row = QStringView(wanted).left(wanted.size() - (open ? 1 : 0)).toInt();
            if (open)
                model.toggleWork(row);
            panels.push_back({QStringLiteral("Row %1%2").arg(row).arg(open ? " opened" : ""),
                              shot(view, list, open ? row + 1 : row)});
            if (open)
                model.toggleWork(row);
        }
        edit = failure = -1;
    } else if (parser.isSet("live")) {
        // The box's latest edit (else its latest step) opened under it.
        const auto& steps = model.liveStepsModel();
        int open = -1;
        for (int row = steps.rowCount() - 1; row >= 0 && open < 0; --row)
            if (steps.data(steps.index(row), TranscriptModel::TitleRole).toString() ==
                QLatin1String("Edited"))
                open = steps.data(steps.index(row), TranscriptModel::ItemRole).toInt();
        if (open < 0 && steps.rowCount() > 0)
            open = steps.data(steps.index(steps.rowCount() - 1), TranscriptModel::ItemRole).toInt();
        if (open >= 0) {
            root->setProperty("liveOpenItem", open);
            panels.push_back({QStringLiteral("Live step opened"), shot(view, list, -1)});
            root->setProperty("liveOpenItem", -1);
        }
    }
    // Following would pull every later panel back to the end.
    root->setProperty("following", false);
    if (edit >= 0)
        panels.push_back({QStringLiteral("Latest edit"), shot(view, list, edit)});
    if (failure >= 0)
        panels.push_back({QStringLiteral("Latest failed tool"), shot(view, list, failure)});
    // The latest folded turn, closed and then opened.
    // --work N picks the N-th "Worked for" row from the top instead.
    int work = -1;
    std::vector<int> work_rows;
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.data(model.index(row), TranscriptModel::KindRole).toString() ==
            QLatin1String("work"))
            work_rows.push_back(row);
    if (!work_rows.empty() && !parser.isSet("rows")) {
        const int wanted = parser.value("work").toInt();
        work = wanted >= 1 && std::cmp_less_equal(wanted, work_rows.size())
                   ? work_rows[static_cast<size_t>(wanted - 1)]
                   : work_rows.back();
    }
    if (work >= 0) {
        panels.push_back({QStringLiteral("Latest turn, folded"), shot(view, list, work)});
        model.toggleWork(work);
        panels.push_back({QStringLiteral("Latest turn, opened"), shot(view, list, work + 2)});
        model.toggleWork(work);
    }
    root->setProperty("following", true);
    if (parser.isSet("panel")) {
        // One panel alone, by the start of its caption.
        const auto wanted = parser.value("panel");
        const auto hit = std::ranges::find_if(
            panels, [&wanted](const Panel& panel) { return panel.caption.startsWith(wanted); });
        if (hit == panels.end()) {
            std::fprintf(stderr, "no panel \"%s\"\n", qPrintable(wanted));
            return 1;
        }
        Panel only = *hit;
        panels = {only};
    }

    const int gap = 24, caption = 44;
    const auto panel_size = panels.front().image.size();
    QImage sheet(static_cast<int>(panels.size()) * (panel_size.width() + gap) + gap,
                 panel_size.height() + caption + gap, QImage::Format_RGB32);
    sheet.fill(QColor("#050505"));
    QPainter painter(&sheet);
    QFont font;
    font.setPixelSize(16);
    painter.setFont(font);
    for (size_t i = 0; i < panels.size(); ++i) {
        const int x = gap + static_cast<int>(i) * (panel_size.width() + gap);
        painter.setPen(QColor("#8e8e8e"));
        painter.drawText(QRect(x, 0, panel_size.width(), caption), Qt::AlignVCenter | Qt::AlignLeft,
                         panels[i].caption);
        painter.drawImage(x, caption, panels[i].image);
        painter.setPen(QColor("#222222"));
        painter.drawRect(x - 1, caption - 1, panel_size.width() + 1, panel_size.height() + 1);
    }
    painter.end();
    if (!sheet.save(parser.value("out"))) {
        std::fprintf(stderr, "could not write %s\n", qPrintable(parser.value("out")));
        return 1;
    }
    std::printf("items %d  load %lld ms  sheet %s\n", model.rowCount(),
                static_cast<long long>(load_ms), qPrintable(parser.value("out")));

    if (parser.isSet("bench")) {
        // Scroll top to bottom a step at a time; each step polishes, renders
        // and reads the frame back, so times are upper bounds for software.
        const qreal step_px = parser.value("step").toDouble();
        root->setProperty("following", false);
        QMetaObject::invokeMethod(list, "positionViewAtBeginning");
        settle();
        // Each step is timed in parts: the scroll itself (where the list makes
        // the items coming into view), pending events, and the software render
        // with its readback, which a GPU window does not pay.
        std::vector<double> frames, scrolls, renders;
        QElapsedTimer total;
        total.start();
        const auto ms = [](const QElapsedTimer& timer) {
            return static_cast<double>(timer.nsecsElapsed()) / 1e6;
        };
        for (int step = 0; step < 4000; ++step) {
            const bool at_end = list->property("atYEnd").toBool();
            if (at_end)
                break;
            QElapsedTimer frame;
            frame.start();
            list->setProperty("contentY", list->property("contentY").toReal() + step_px);
            QCoreApplication::processEvents();
            scrolls.push_back(ms(frame));
            if (scrolls.back() > 80 && parser.isSet("bench")) {
                // Name the items that just came into view, to see what is slow.
                const qreal bottom =
                    list->property("contentY").toReal() + list->property("height").toReal();
                int at = -1;
                QMetaObject::invokeMethod(list, "indexAt", Q_RETURN_ARG(int, at),
                                          Q_ARG(qreal, 10.0), Q_ARG(qreal, bottom - 10));
                if (at >= 0) {
                    const auto index = model.index(at);
                    std::printf(
                        "  slow step %.0f ms at item %d: %s %s, body %lld chars, result %lld "
                        "chars\n",
                        scrolls.back(), at,
                        qPrintable(model.data(index, TranscriptModel::KindRole).toString()),
                        qPrintable(model.data(index, TranscriptModel::TitleRole).toString()),
                        static_cast<long long>(
                            model.data(index, TranscriptModel::BodyRole).toString().size()),
                        static_cast<long long>(
                            model.data(index, TranscriptModel::ResultRole).toString().size()));
                }
            }
            QElapsedTimer render;
            render.start();
            (void)view.grabWindow();
            renders.push_back(ms(render));
            frames.push_back(ms(frame));
        }
        const auto line = [](const char* name, const std::vector<double>& values) {
            std::printf("  %-22s p50 %6.2f  p95 %6.2f  p99 %6.2f  max %7.2f ms\n", name,
                        percentile(values, 0.50), percentile(values, 0.95),
                        percentile(values, 0.99), percentile(values, 1.0));
        };
        std::printf("bench: %zu steps of %.0f px in %lld ms (offscreen, software scene graph)\n",
                    frames.size(), step_px, static_cast<long long>(total.elapsed()));
        line("scroll + item creation", scrolls);
        line("render + readback", renders);
        line("whole step", frames);
    }
    return 0;
}
