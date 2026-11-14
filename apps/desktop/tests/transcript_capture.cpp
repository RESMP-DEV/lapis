// Renders a Claude Code conversation in the visual view, offscreen with the
// software scene graph, and saves one contact sheet: the latest items, the
// latest edit and the latest failed tool side by side. --bench also scrolls
// the whole conversation and reports frame times. No window is shown and the
// process exits when done.
//
//   lapis_transcript_capture --jsonl FILE --out SHEET.png [--bench]
//   lapis_transcript_capture --jsonl FILE --send PROMPT --then RECORDS [--out PREFIX]
#include "media_actions.hpp"
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
#include <QMimeData>
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
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#endif
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <sys/resource.h>
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

int check_rows(QQuickItem* list, const QString& when, QStringList& what);

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
    // Each moment also checks that no row of the thread overlaps the next.
    QStringList overlaps;
    int moment = 0;
    const auto end = [&] {
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        settle();
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        settle();
        // Rows and columns lay out when a frame is made; check what it draws.
        (void)view.grabWindow();
        check_rows(list, QStringLiteral("moment %1").arg(++moment), overlaps);
        settle(); // what the frame's layout queued (the box following its end)
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
    end();
    for (const auto& overlap : overlaps)
        std::fprintf(stderr, "live FAIL rows: %s\n", qPrintable(overlap));
    expect(overlaps.empty(), "no row of the thread overlaps the next");
    std::puts(failures == 0 ? "live: all checks passed" : "live: failed");
    return failures == 0 ? 0 : 1;
}

// --media-checks: a made-up reply with a see-through diagram standing alone
// and a PDF named in a sentence, written by write_media_fixture.
void write_media_fixture(const QString& folder, QFile& file) {
    QImage diagram(360, 180, QImage::Format_ARGB32);
    diagram.fill(Qt::transparent);
    QPainter paint(&diagram);
    paint.setRenderHint(QPainter::Antialiasing);
    paint.setPen(QPen(QColor("#e8e8e8"), 3)); // drawn for a dark page
    paint.drawRoundedRect(QRectF(20, 50, 110, 70), 10, 10);
    paint.drawRoundedRect(QRectF(230, 50, 110, 70), 10, 10);
    paint.drawLine(QPointF(130, 85), QPointF(228, 85));
    paint.drawLine(QPointF(216, 75), QPointF(228, 85));
    paint.drawLine(QPointF(216, 95), QPointF(228, 85));
    paint.end();
    const auto picture = QDir(folder).filePath(QStringLiteral("diagram.png"));
    (void)diagram.save(picture);
    QFile pdf(QDir(folder).filePath(QStringLiteral("report.pdf")));
    if (pdf.open(QIODevice::WriteOnly))
        pdf.write("%PDF-1.4\n");
    // Named from the home folder (the capture's own), as agents often do.
    const auto reply = QStringLiteral("The flow, as drawn:\n\n~/diagram.png\n\nThe full write-up "
                                      "is in ~/report.pdf for later.");
    const QJsonObject prompt{{"type", "user"},
                             {"uuid", "p1"},
                             {"timestamp", "2026-11-11T16:00:00.000Z"},
                             {"message", QJsonObject{{"content", "Draw the flow"}}}};
    const QJsonObject answer{
        {"type", "assistant"},
        {"uuid", "a1"},
        {"timestamp", "2026-11-11T16:00:02.000Z"},
        {"message",
         QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", reply}}}}}}};
    for (const auto& record : {prompt, answer})
        file.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
    file.write(R"({"type":"system","subtype":"turn_duration","durationMs":2000})"
               "\n");
    file.flush();
}

void press(QQuickView& view, QQuickItem* item, QPointF offset, Qt::MouseButton button,
           Qt::KeyboardModifiers modifiers, QEvent::Type type) {
    const auto at = item->mapToScene(offset);
    // A move names no button of its own, only the one held.
    QMouseEvent event(type, at, at, view.mapToGlobal(at.toPoint()),
                      type == QEvent::MouseMove ? Qt::NoButton : button,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button),
                      modifiers);
    QCoreApplication::sendEvent(&view, &event);
}

// Media in the thread: a click opens the file, a drag carries it out (and
// is not also a click), and a right-click or Control-click opens its menu,
// whose actions reach the file. Recorders stand in for the clipboard, the
// Finder and the drag, so nothing leaves the process.
int check_media(QQuickView& view, QQuickItem* root, const QString& capture) {
    int failures = 0;
    const auto expect = [&failures](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "media FAIL %s\n", what);
            ++failures;
        }
    };
    lapis::desktop::MediaActions actions;
    std::vector<std::pair<QString, QString>> launched;
    actions.setLauncherForTesting([&launched](const QString& action, const QString& path) {
        launched.emplace_back(action, path);
        return true;
    });
    std::unique_ptr<QMimeData> copied;
    actions.setClipboardForTesting(
        [&copied](std::unique_ptr<QMimeData> data) { copied = std::move(data); });
    std::unique_ptr<QMimeData> dragged;
    int drags = 0;
    actions.setDraggerForTesting([&](std::unique_ptr<QMimeData> data, const QImage&) {
        dragged = std::move(data);
        ++drags;
        return true;
    });
    root->setProperty("mediaActions", QVariant::fromValue(static_cast<QObject*>(&actions)));
    auto* list = root->property("list").value<QQuickItem*>();
    QMetaObject::invokeMethod(list, "positionViewAtEnd");
    settle();
    auto items = named(root, "mediaItem");
    expect(items.size() == 2, "the diagram in place and the PDF under the reply");
    if (items.size() != 2)
        return 1;
    auto* picture = items.front();
    auto* card = items.back();
    const auto middle = [](QQuickItem* item) {
        return QPointF(item->width() / 2, item->height() / 2);
    };

    // A plain click opens the picture.
    press(view, picture, middle(picture), Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
    press(view, picture, middle(picture), Qt::LeftButton, Qt::NoModifier,
          QEvent::MouseButtonRelease);
    settle();
    expect(launched.size() == 1 && launched.back().first == QLatin1String("open") &&
               launched.back().second.endsWith(QLatin1String("diagram.png")),
           "a click opens the picture");

    // A drag carries the file and its pixels, and opens nothing.
    press(view, picture, middle(picture), Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
    for (int step = 1; step <= 8; ++step)
        press(view, picture, middle(picture) + QPointF(step * 6, step * 2), Qt::LeftButton,
              Qt::NoModifier, QEvent::MouseMove);
    press(view, picture, middle(picture) + QPointF(48, 16), Qt::LeftButton, Qt::NoModifier,
          QEvent::MouseButtonRelease);
    settle();
    std::printf("media: %d drag(s) from one gesture\n", drags);
    expect(drags == 1, "a drag starts one file drag");
    expect(dragged && dragged->urls().size() == 1 &&
               dragged->urls().front().toLocalFile().endsWith(QLatin1String("diagram.png")) &&
               dragged->hasImage(),
           "the drag carries the file URL and the picture");
    expect(launched.size() == 1, "a drag is not also a click");
    // The card drags as its file.
    press(view, card, middle(card), Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
    for (int step = 1; step <= 8; ++step)
        press(view, card, middle(card) + QPointF(step * 6, 0), Qt::LeftButton, Qt::NoModifier,
              QEvent::MouseMove);
    press(view, card, middle(card) + QPointF(48, 0), Qt::LeftButton, Qt::NoModifier,
          QEvent::MouseButtonRelease);
    settle();
    expect(drags == 2 && dragged && !dragged->hasImage() &&
               dragged->urls().front().toLocalFile().endsWith(QLatin1String("report.pdf")),
           "a card drags as its file alone");

    // The right-click menu, made on the first one.
    expect(root->findChild<QObject*>(QStringLiteral("mediaMenu")) == nullptr,
           "no menu before a right-click");
    press(view, picture, QPointF(40, 40), Qt::RightButton, Qt::NoModifier,
          QEvent::MouseButtonPress);
    press(view, picture, QPointF(40, 40), Qt::RightButton, Qt::NoModifier,
          QEvent::MouseButtonRelease);
    settle();
    auto* menu = root->findChild<QObject*>(QStringLiteral("mediaMenu"));
    expect(menu != nullptr, "a media menu");
    if (menu == nullptr)
        return 1;
    const auto item = [root](const char* name) {
        return root->findChild<QObject*>(QString::fromLatin1(name));
    };
    expect(menu->property("visible").toBool(), "a right-click opens the menu");
    expect(item("mediaCopy") &&
               item("mediaCopy")->property("text").toString() == QLatin1String("Copy Image"),
           "a picture offers Copy Image");
    for (const char* name : {"mediaOpen", "mediaReveal", "mediaCopyPath", "mediaSaveAs"})
        expect(item(name) && item(name)->property("enabled").toBool(), name);
    for (QElapsedTimer opened; !opened.isValid() || opened.elapsed() < 300;) {
        if (!opened.isValid())
            opened.start();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    if (!capture.isEmpty())
        (void)view.grabWindow().save(capture);
    QMetaObject::invokeMethod(item("mediaCopy"), "click");
    settle();
    expect(copied && copied->hasImage() && copied->urls().size() == 1,
           "Copy Image puts the picture and the file on the clipboard");
    expect(!menu->property("visible").toBool(), "choosing closes the menu");
    press(view, picture, QPointF(40, 40), Qt::RightButton, Qt::NoModifier,
          QEvent::MouseButtonPress);
    press(view, picture, QPointF(40, 40), Qt::RightButton, Qt::NoModifier,
          QEvent::MouseButtonRelease);
    settle();
    QMetaObject::invokeMethod(item("mediaReveal"), "click");
    settle();
    expect(launched.size() == 2 && launched.back().first == QLatin1String("reveal"),
           "Show in Finder selects the file");

    // Control-click on the card: a file's menu.
    press(view, card, middle(card), Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonPress);
    press(view, card, middle(card), Qt::LeftButton, Qt::ControlModifier,
          QEvent::MouseButtonRelease);
    settle();
    expect(menu->property("visible").toBool(), "a Control-click opens the menu");
    expect(item("mediaCopy")->property("text").toString() == QLatin1String("Copy File"),
           "a file offers Copy File");
    expect(launched.size() == 2, "a Control-click does not open the file");
    QMetaObject::invokeMethod(item("mediaCopyPath"), "click");
    settle();
    expect(copied && copied->text().endsWith(QLatin1String("report.pdf")) && !copied->hasImage(),
           "Copy Path copies the path as text");
    std::puts(failures == 0 ? "media: all checks passed" : "media: failed");
    return failures == 0 ? 0 : 1;
}

// The highest top and deepest bottom edges, in `row`'s coordinates, of
// anything visible drawn inside it (text laid out past its item's height
// counts).
std::pair<qreal, qreal> drawn_span(QQuickItem* row) {
    qreal top = 0, bottom = 0;
    const auto walk = [&top, &bottom, row](auto&& self, QQuickItem* item) -> void {
        if (!item->isVisible() || item->opacity() <= 0)
            return;
        const qreal height = std::max(
            item->height(), item->property("contentHeight").isValid() &&
                                    item->metaObject()->indexOfProperty("textDocument") >= 0
                                ? item->property("contentHeight").toReal()
                                : 0.0);
        if (item != row && item->width() > 0 && height > 0) {
            top = std::min(top, item->mapToItem(row, QPointF(0, 0)).y());
            bottom = std::max(bottom, item->mapToItem(row, QPointF(0, height)).y());
        }
        if (item->clip() && item != row)
            return;
        for (auto* child : item->childItems())
            self(self, child);
    };
    walk(walk, row);
    return {top, bottom};
}

// Rows of the thread the list holds, each above the next, nothing drawn
// past a row's bottom, and the footer below the last. Problems go to
// `what`, prefixed; returns their number.
int check_rows(QQuickItem* list, const QString& when, QStringList& what) {
    int problems = 0;
    const auto add = [&](const QString& text) {
        what << when + QStringLiteral(": ") + text;
        ++problems;
    };
    const int count = list->property("count").toInt();
    QQuickItem* above = nullptr;
    int above_index = -1;
    for (int index = 0; index < count; ++index) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, index));
        if (item == nullptr || !item->isVisible())
            continue;
        const auto [drawn_top, drawn_bottom] = drawn_span(item);
        if (drawn_top < -0.5 || drawn_bottom > item->height() + 0.5)
            add(QStringLiteral("row %1 (%2) draws from %3 to %4, outside its height %5")
                    .arg(index)
                    .arg(item->property("kind").toString())
                    .arg(drawn_top)
                    .arg(drawn_bottom)
                    .arg(item->height()));
        if (above != nullptr && above_index == index - 1 &&
            above->y() + above->height() > item->y() + 0.5)
            add(QStringLiteral("row %1 ends at %2, below row %3's top %4")
                    .arg(above_index)
                    .arg(above->y() + above->height())
                    .arg(index)
                    .arg(item->y()));
        above = item;
        above_index = index;
    }
    // A row the list no longer holds for any index must not draw: left in
    // the list, it would sit over whatever took its place. (Rows waiting in
    // the reuse pool stay "visible" but are culled, so only a row whose
    // hiding changes the frame counts.)
    QSet<QQuickItem*> held;
    for (int index = 0; index < count; ++index) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, index));
        held.insert(item);
    }
    auto* content = list->property("contentItem").value<QQuickItem*>();
    for (auto* child : content != nullptr ? content->childItems() : QList<QQuickItem*>{}) {
        if (!child->isVisible() || held.contains(child) || !child->property("kind").isValid())
            continue;
        // Its area of the frame with it, without it (opacity, so the row's
        // own visible binding stays) and with it again; a pulsing dot
        // nearby makes the first and last differ, which proves nothing.
        auto* window = list->window();
        const auto area = child->mapRectToScene(QRectF(0, 0, child->width(), child->height()))
                              .toAlignedRect()
                              .intersected(QRect(QPoint(0, 0), window->size()));
        if (area.isEmpty())
            continue;
        const auto with = window->grabWindow().copy(area);
        const qreal opacity = child->opacity();
        child->setOpacity(0);
        const auto without = window->grabWindow().copy(area);
        child->setOpacity(opacity);
        const bool drawn = with != without && window->grabWindow().copy(area) == with;
        if (drawn)
            add(QStringLiteral("a %1 row the list no longer holds draws at %2")
                    .arg(child->property("kind").toString())
                    .arg(child->y()));
    }
    auto* footer = list->property("footerItem").value<QQuickItem*>();
    if (footer != nullptr && footer->isVisible()) {
        const auto [top, bottom] = drawn_span(footer);
        if (top < -0.5 || bottom > footer->height() + 0.5)
            add(QStringLiteral("the footer draws from %1 to %2, outside its height %3")
                    .arg(top)
                    .arg(bottom)
                    .arg(footer->height()));
    }
    if (footer != nullptr && above != nullptr && above_index == count - 1) {
        const auto row_end = above->mapToScene(QPointF(0, above->height())).y();
        const auto footer_top = footer->mapToScene(QPointF(0, drawn_span(footer).first)).y();
        if (footer_top + 0.5 < row_end)
            add(QStringLiteral("the footer's top %1 is above the last row's end %2")
                    .arg(footer_top)
                    .arg(row_end));
    }
    return problems;
}

// A prompt sent to a conversation already shown: the bar's send (working,
// and for a CLI that records no queue the prompt noted as sent), then the
// CLI's records appended. The thread's rows are checked right after each
// change and once settled; `out`, when given, saves each moment's frame.
int check_sent(QQuickView& view, QQuickItem* root, TranscriptModel& model, QFile& file,
               const QList<QByteArray>& records, const QString& prompt, const QString& out) {
    auto* list = root->property("list").value<QQuickItem*>();
    QStringList problems;
    int shot_number = 0;
    // The next frame drawn: rows and columns lay out as it is made.
    const auto look = [&](const char* when) {
        const auto frame = view.grabWindow();
        check_rows(list, QString::fromLatin1(when), problems);
        if (!out.isEmpty())
            (void)frame.save(
                QStringLiteral("%1-%2.png").arg(out).arg(shot_number++, 2, 10, QLatin1Char('0')));
    };
    QMetaObject::invokeMethod(list, "positionViewAtEnd");
    settle();
    look("before sending");
    // Lines of the records starting "#" are moments of their own: "#working
    // 1" or "#working 0" as the terminal reports it; "#send" (working, the
    // prompt noted as sent); "#type" (the prompt into the bar, "\n" for a
    // line break) and "#submit" (the bar's Return); "#top" (scrolled back);
    // "#hide" and "#show" (the terminal shown instead, then the thread);
    // "#other FILE" (another agent's conversation); "#wait MS".
    QList<QByteArray> lines = records;
    if (!prompt.isEmpty() && !lines.contains("#send") && !lines.contains("#submit"))
        lines.prepend("#send");
    for (const auto& line : lines) {
        if (line.startsWith("#working")) {
            root->setProperty("working", line.endsWith('1'));
        } else if (line == "#send") {
            root->setProperty("working", true);
            model.noteSent(prompt);
        } else if (line.startsWith("#type")) {
            // The prompt typed into the bar first, so the bar is tall.
            if (auto* input = root->findChild<QQuickItem*>(QStringLiteral("visualComposer")))
                input->setProperty("text", QString(prompt).replace(QStringLiteral("\\n"), "\n"));
        } else if (line == "#submit") {
            // Sent as Return in the bar sends it.
            QJSValue sent =
                view.engine()->evaluate(QStringLiteral("(function(t) { return true })"));
            root->setProperty("submit", QVariant::fromValue(sent));
            QMetaObject::invokeMethod(root, "send");
        } else if (line == "#top") {
            // Scrolled back to the conversation's start.
            root->setProperty("following", false);
            QMetaObject::invokeMethod(list, "positionViewAtBeginning");
        } else if (line == "#hide") {
            // The terminal shows instead: the view hides and reads nothing.
            root->setVisible(false);
            model.setPath(QString());
        } else if (line.startsWith("#other ")) {
            // Another agent's conversation shows in the same view.
            model.setPath(QString::fromUtf8(line.mid(7)));
        } else if (line == "#show") {
            root->setVisible(true);
            model.setPath(file.fileName());
        } else if (line.startsWith("#wait")) {
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < line.mid(6).toInt())
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        } else {
            file.write(line + '\n');
            file.flush();
            model.readMore();
        }
        QCoreApplication::processEvents();
        look(line.startsWith('#') ? line.constData() : "right after a record");
        for (int i = 0; i < 20; ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        look("settled");
    }
    for (const auto& problem : problems)
        std::fprintf(stderr, "rows FAIL %s\n", qPrintable(problem));
    return static_cast<int>(problems.size());
}

// A long made-up reply: a two-level list with bold lead-ins, inline code
// and math (sub- and superscripts change line heights).
QString nested_reply() {
    QStringList lines{QStringLiteral("Here is the plan, in order:"), QString()};
    for (int i = 1; i <= 6; ++i) {
        lines << QStringLiteral(
                     "- **Stage %1:** move `src/part%1.cpp` along at a cost of $x_%1^2 + "
                     "y_%1$ per unit, with a sentence long enough to wrap the column.")
                     .arg(i);
        for (int j = 1; j <= 3; ++j)
            lines << QStringLiteral("    - **Detail %1.%2:** keep `flag_%2` on; the bound is "
                                    "$O(n^%2)$ and $\\frac{a_%2}{b}$ stays below one, as this "
                                    "sentence explains at some length.")
                         .arg(i)
                         .arg(j);
    }
    lines << QString() << QStringLiteral("That is the whole plan.");
    return lines.join(QLatin1Char('\n'));
}

QByteArray compact(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// Each CLI's records for a finished turn ending in that reply, for the
// next prompt that starts a turn, and for that turn's step and final reply.
struct SentFixture {
    const char* format{};
    QList<QByteArray> before;
    QList<QByteArray> prompt;
    QList<QByteArray> step;
    QList<QByteArray> reply;
};
std::vector<SentFixture> sent_fixtures() {
    const auto reply = nested_reply();
    const auto text = [](const QString& value) {
        return QJsonArray{QJsonObject{{"type", "text"}, {"text", value}}};
    };
    const auto codex_item = [](const char* at, const char* turn, const QJsonObject& item) {
        return compact(
            {{"timestamp", at},
             {"type", "event_msg"},
             {"payload",
              QJsonObject{{"type", "item_completed"}, {"turn_id", turn}, {"item", item}}}});
    };
    const auto codex_event = [](const char* at, const QJsonObject& payload) {
        return compact({{"timestamp", at}, {"type", "event_msg"}, {"payload", payload}});
    };
    return {
        {"claude",
         {compact({{"type", "user"},
                   {"uuid", "u1"},
                   {"timestamp", "2026-11-11T16:00:00.000Z"},
                   {"message", QJsonObject{{"content", "Summarize the plan"}}}}),
          compact({{"type", "assistant"},
                   {"uuid", "a1"},
                   {"timestamp", "2026-11-11T16:00:05.000Z"},
                   {"message", QJsonObject{{"content", text(reply)}}}}),
          compact({{"type", "system"}, {"subtype", "turn_duration"}, {"durationMs", 5000}})},
         {compact({{"type", "user"},
                   {"uuid", "u2"},
                   {"timestamp", "2026-11-11T16:06:00.000Z"},
                   {"message", QJsonObject{{"content", "Now do stage one"}}}})},
         {compact({{"type", "assistant"},
                   {"uuid", "a2"},
                   {"timestamp", "2026-11-11T16:06:02.000Z"},
                   {"message",
                    QJsonObject{{"content", QJsonArray{QJsonObject{
                                                {"type", "tool_use"},
                                                {"id", "b1"},
                                                {"name", "Bash"},
                                                {"input", QJsonObject{{"command", "make"}}}}}}}}}),
          compact({{"type", "user"},
                   {"uuid", "r2"},
                   {"timestamp", "2026-11-11T16:06:20.000Z"},
                   {"message",
                    QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "tool_result"},
                                                                   {"tool_use_id", "b1"},
                                                                   {"content", "built"}}}}}}})},
         {compact({{"type", "assistant"},
                   {"uuid", "a3"},
                   {"timestamp", "2026-11-11T16:06:23.000Z"},
                   {"message", QJsonObject{{"content", text(reply)}}}}),
          compact({{"type", "system"}, {"subtype", "turn_duration"}, {"durationMs", 23000}})}},
        {"codex",
         {codex_event("2026-11-11T16:00:00.000Z", {{"type", "task_started"}, {"turn_id", "t1"}}),
          codex_item(
              "2026-11-11T16:00:00.100Z", "t1",
              {{"type", "UserMessage"}, {"id", "u1"}, {"content", text("Summarize the plan")}}),
          codex_item("2026-11-11T16:00:05.000Z", "t1",
                     {{"type", "AgentMessage"},
                      {"id", "a1"},
                      {"phase", "final_answer"},
                      {"content", QJsonArray{QJsonObject{{"type", "Text"}, {"text", reply}}}}}),
          codex_event("2026-11-11T16:00:05.100Z",
                      {{"type", "task_complete"}, {"turn_id", "t1"}, {"duration_ms", 5000}})},
         {codex_event("2026-11-11T16:06:00.000Z", {{"type", "task_started"}, {"turn_id", "t2"}}),
          codex_item(
              "2026-11-11T16:06:00.100Z", "t2",
              {{"type", "UserMessage"}, {"id", "u2"}, {"content", text("Now do stage one")}})},
         {codex_item("2026-11-11T16:06:20.000Z", "t2",
                     {{"type", "CommandExecution"},
                      {"id", "c1"},
                      {"command", "make"},
                      {"exit_code", 0},
                      {"aggregated_output", "built"}})},
         {codex_item("2026-11-11T16:06:23.000Z", "t2",
                     {{"type", "AgentMessage"},
                      {"id", "a2"},
                      {"phase", "final_answer"},
                      {"content", QJsonArray{QJsonObject{{"type", "Text"}, {"text", reply}}}}}),
          codex_event("2026-11-11T16:06:23.100Z",
                      {{"type", "task_complete"}, {"turn_id", "t2"}, {"duration_ms", 23000}})}},
        {"antigravity",
         {compact({{"step_index", 0},
                   {"type", "USER_INPUT"},
                   {"created_at", "2026-11-11T16:00:00Z"},
                   {"content", "<USER_REQUEST>Summarize the plan</USER_REQUEST>"}}),
          compact({{"step_index", 1},
                   {"type", "PLANNER_RESPONSE"},
                   {"created_at", "2026-11-11T16:00:05Z"},
                   {"content", reply}})},
         {compact({{"step_index", 2},
                   {"type", "USER_INPUT"},
                   {"created_at", "2026-11-11T16:06:00Z"},
                   {"content", "<USER_REQUEST>Now do stage one</USER_REQUEST>"}})},
         {compact({{"step_index", 3},
                   {"type", "PLANNER_RESPONSE"},
                   {"created_at", "2026-11-11T16:06:02Z"},
                   {"thinking", "Building first."},
                   {"tool_calls",
                    QJsonArray{QJsonObject{{"name", "run_command"},
                                           {"args", QJsonObject{{"CommandLine", "make"}}}}}}}),
          compact({{"step_index", 4},
                   {"type", "GENERIC"},
                   {"created_at", "2026-11-11T16:06:20Z"},
                   {"content", "built"}})},
         {compact({{"step_index", 5},
                   {"type", "PLANNER_RESPONSE"},
                   {"created_at", "2026-11-11T16:06:23Z"},
                   {"content", reply}})}},
    };
}

// A prompt sent after a long reply, for each CLI at two text sizes: the
// running turn's line and box start below the reply, never over it, right
// after the send and the CLI's record and once laid out.
int check_sent_after_reply(QQuickView& view, QQuickItem* root, TranscriptModel& model,
                           const QString& folder) {
    int failures = 0;
    for (const auto& fixture : sent_fixtures()) {
        for (const int size : {14, 21}) {
            QFile file(
                QDir(folder).filePath(QStringLiteral("%1-%2.jsonl").arg(fixture.format).arg(size)));
            if (!file.open(QIODevice::WriteOnly))
                return 1;
            for (const auto& line : fixture.before)
                file.write(line + '\n');
            file.flush();
            root->setProperty("working", false);
            root->setProperty("sizeSetting", size);
            model.setFormat(
                lapis::desktop::transcript::format_named(QString::fromLatin1(fixture.format)));
            model.setPath(file.fileName());
            model.loadNow();
            QThreadPool::globalInstance()->waitForDone();
            settle();
            const auto rows = model.rowCount();
            const int problems = check_sent(view, root, model, file, fixture.prompt,
                                            QStringLiteral("Now do stage one"), {});
            const bool live =
                model.rowCount() > rows &&
                model.data(model.index(model.rowCount() - 1), TranscriptModel::KindRole)
                        .toString() == QLatin1String("live");
            if (problems > 0 || !live) {
                std::fprintf(stderr, "live FAIL %s at %d px: %d row problems%s\n", fixture.format,
                             size, problems, live ? "" : ", no running turn shown");
                ++failures;
            }
        }
    }
    root->setProperty("working", false);
    return failures;
}

// Items named `name` shown in the thread's rows or its footer (where the
// running strip sits while the agent waits on its work).
std::vector<QQuickItem*> shown_named(QQuickItem* root, const char* name) {
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
    if (auto* footer = list->property("footerItem").value<QQuickItem*>())
        walk(walk, footer);
    for (auto* item : named(root, name))
        out.push_back(item);
    return out;
}

// The pending prompts the footer shows, top to bottom, as their texts.
QStringList pending_shown(QQuickItem* root) {
    auto items = shown_named(root, "pendingPrompt");
    std::ranges::sort(items, [](QQuickItem* a, QQuickItem* b) {
        return a->mapToScene({0, 0}).y() < b->mapToScene({0, 0}).y();
    });
    QStringList out;
    for (auto* item : items)
        out << item->property("modelData").toMap().value(QStringLiteral("text")).toString();
    return out;
}

// The agent's status says working while its file shows the last turn over
// (an output estimate lagging agy's or Codex's final record), for each CLI
// at two text sizes: the thread shows no running turn then, unless a prompt
// sent from the bar waits to be recorded. Each moment's rows are checked
// (each above the next, nothing drawn past its row or by a row the list let
// go of, the footer below); `out`, when given, saves each moment's frame.
int check_turn_ended(QQuickView& view, QQuickItem* root, TranscriptModel& model,
                     const QString& folder, const QString& out) {
    auto* list = root->property("list").value<QQuickItem*>();
    int failures = 0;
    for (const auto& fixture : sent_fixtures()) {
        for (const int size : {14, 21}) {
            QFile file(QDir(folder).filePath(
                QStringLiteral("ended-%1-%2.jsonl").arg(fixture.format).arg(size)));
            if (!file.open(QIODevice::WriteOnly))
                return 1;
            const auto append = [&](const QList<QByteArray>& records) {
                for (const auto& line : records)
                    file.write(line + '\n');
                file.flush();
                model.readMore();
            };
            root->setProperty("working", false);
            root->setProperty("sizeSetting", size);
            model.setFormat(
                lapis::desktop::transcript::format_named(QString::fromLatin1(fixture.format)));
            for (const auto& line : fixture.before)
                file.write(line + '\n');
            file.flush();
            model.setPath(file.fileName());
            model.loadNow();
            QThreadPool::globalInstance()->waitForDone();
            root->setProperty("following", true);
            QMetaObject::invokeMethod(list, "positionViewAtEnd");
            settle();
            QStringList problems;
            const auto look = [&](const QString& when, bool running) {
                for (int i = 0; i < 20; ++i)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                // At the end, as following leaves it once a frame has laid
                // the footer out.
                (void)view.grabWindow();
                QMetaObject::invokeMethod(list, "positionViewAtEnd");
                settle();
                const auto frame = view.grabWindow();
                check_rows(list, when, problems);
                const auto lines = shown_named(root, "busyText").size();
                const bool live =
                    model.rowCount() > 0 &&
                    model.data(model.index(model.rowCount() - 1), TranscriptModel::KindRole)
                            .toString() == QLatin1String("live");
                if ((lines > 0) != running)
                    problems << QStringLiteral("%1: %2 working lines shown, expected %3")
                                    .arg(when)
                                    .arg(lines)
                                    .arg(running ? "one" : "none");
                if (live && !running)
                    problems << when + QStringLiteral(": a running turn's box shown");
                if (!out.isEmpty())
                    (void)frame.save(QStringLiteral("%1-%2-%3-%4.png")
                                         .arg(out, QString::fromLatin1(fixture.format))
                                         .arg(size)
                                         .arg(QString(when).replace(QLatin1Char(' '), '-')));
            };
            // The last turn ended; the status says working, stops, and
            // says working again.
            root->setProperty("working", true);
            look(QStringLiteral("ended working"), false);
            root->setProperty("working", false);
            look(QStringLiteral("ended idle"), false);
            root->setProperty("working", true);
            look(QStringLiteral("ended working again"), false);
            // A prompt sent from the bar, not recorded yet: the turn shows.
            // (The bar sends it as to an idle agent: the turn ended.)
            model.noteSent(QStringLiteral("Now do stage one"), false);
            look(QStringLiteral("prompt pending"), true);
            // Recorded, a step, then the final reply lands while the status
            // still says working.
            append(fixture.prompt);
            look(QStringLiteral("prompt recorded"), true);
            append(fixture.step);
            look(QStringLiteral("step recorded"), true);
            append(fixture.reply);
            look(QStringLiteral("reply landed"), false);
            root->setProperty("working", false);
            look(QStringLiteral("reply landed idle"), false);
            for (const auto& problem : problems)
                std::fprintf(stderr, "ended FAIL %s at %d px: %s\n", fixture.format, size,
                             qPrintable(problem));
            failures += problems.isEmpty() ? 0 : 1;
        }
    }
    root->setProperty("working", false);
    return failures;
}

// Where each visible text item showing exactly `text` sits in the window.
std::vector<QPointF> text_at(QQuickItem* root, const QString& text) {
    std::vector<QPointF> out;
    const auto walk = [&out, &text](auto&& self, QQuickItem* item) -> void {
        if (!item->isVisible())
            return;
        if (item->width() > 0 && item->property("text").toString() == text &&
            item->property("font").isValid())
            out.push_back(item->mapToScene({0, 0}));
        for (auto* child : item->childItems())
            self(self, child);
    };
    walk(walk, root->property("list").value<QQuickItem*>());
    return out;
}

// Rows of the thread that are prompts with this text.
int prompt_rows(TranscriptModel& model, const QString& text) {
    int count = 0;
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto index = model.index(row);
        if (model.data(index, TranscriptModel::KindRole).toString() == QLatin1String("prompt") &&
            model.data(index, TranscriptModel::BodyRole).toString().simplified() ==
                text.simplified())
            ++count;
    }
    return count;
}

// Return in the bar, for each CLI, idle and working: the prompt shows at the
// thread's end in the first frame drawn after it, with the bar cleared and
// the turn shown starting; the CLI's record then takes its place once, with
// no copy left; two sent quickly keep their order. Then the check after
// sending: a prompt the agent's input box still holds gets one more Return,
// only then, and a bubble note when that does not take it either. The
// agent's screen and Return are stand-ins with each CLI's input box.
int check_bubble(QQuickView& view, QQuickItem* root, TranscriptModel& model,
                 const QString& folder) {
    int failures = 0;
    const auto expect = [&failures](bool ok, const QString& what) {
        if (!ok) {
            std::fprintf(stderr, "bubble FAIL %s\n", qPrintable(what));
            ++failures;
        }
    };
    auto* input = root->findChild<QQuickItem*>(QStringLiteral("visualComposer"));
    if (input == nullptr)
        return 1;
    // A stand-in terminal: its input box holds the last text sent until a
    // Return takes it; `swallow` Returns are lost first (as a TUI that
    // takes a Return arriving with a paste as part of the paste would).
    auto terminal = view.engine()->evaluate(QStringLiteral(R"((function(mark) {
        var t = { mark: mark, held: "", swallow: 0, presses: 0, sends: 0 };
        t.submit = function(text) { t.sends++; if (t.swallow > 0) { t.swallow--; t.held = text } return true };
        t.press = function() { t.presses++; if (t.swallow > 0) t.swallow--; else t.held = ""; return true };
        t.screen = function() { return ["", t.mark + " " + t.held.split("\n")[0], ""] };
        return t;
    }))"));
    const auto send = [&](const QString& text) {
        input->setProperty("text", text);
        QElapsedTimer timer;
        timer.start();
        QMetaObject::invokeMethod(root, "send");
        (void)view.grabWindow(); // the next frame
        return static_cast<double>(timer.nsecsElapsed()) / 1e6;
    };
    const auto wait = [](int ms) {
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    };
    for (const auto& fixture : sent_fixtures()) {
        const QString name = QString::fromLatin1(fixture.format);
        const QString mark = name == QLatin1String("claude")  ? QStringLiteral("❯")
                             : name == QLatin1String("codex") ? QStringLiteral("›")
                                                              : QStringLiteral(">");
        auto fake = terminal.call({QJSValue(mark)});
        root->setProperty("submit", QVariant::fromValue(fake.property("submit")));
        root->setProperty("pressReturn", QVariant::fromValue(fake.property("press")));
        root->setProperty("screen", QVariant::fromValue(fake.property("screen")));
        for (const bool working : {false, true}) {
            const auto what = [&](const char* text) {
                return QStringLiteral("%1 %2: %3")
                    .arg(name, working ? QStringLiteral("working") : QStringLiteral("idle"),
                         QString::fromLatin1(text));
            };
            QFile file(QDir(folder).filePath(
                QStringLiteral("bubble-%1-%2.jsonl").arg(name).arg(working ? 1 : 0)));
            if (!file.open(QIODevice::WriteOnly))
                return 1;
            for (const auto& line : fixture.before)
                file.write(line + '\n');
            file.flush();
            root->setProperty("sizeSetting", 15);
            model.setFormat(lapis::desktop::transcript::format_named(name));
            model.setPath(file.fileName());
            model.loadNow();
            root->setProperty("working", working);
            settle();
            (void)view.grabWindow();
            const auto prompt = QStringLiteral("Now do stage one");
            const double ms = send(prompt);
            const bool shown = pending_shown(root) == QStringList{prompt};
            if (shown)
                std::printf("bubble %s %s: shown in the first frame, %.1f ms after Return\n",
                            fixture.format, working ? "working" : "idle", ms);
            else
                std::printf("bubble %s %s: not shown before the CLI records it\n", fixture.format,
                            working ? "working" : "idle");
            expect(shown, what("the prompt shows at once"));
            expect(input->property("text").toString().isEmpty(), what("the bar clears"));
            auto busy = shown_named(root, "busyText");
            expect(working ||
                       (busy.size() == 1 && busy.front()->property("text").toString().startsWith(
                                                QLatin1String("Working"))),
                   what("the turn shows starting"));
            QStringList rows_problems;
            check_rows(root->property("list").value<QQuickItem*>(), what("sent"), rows_problems);
            expect(rows_problems.isEmpty(), what("the bubble overlaps nothing"));
            if (working) {
                root->setProperty("working", false);
                continue;
            }
            // The CLI records it: one prompt row, no bubble left, its text
            // drawn where the bubble's was.
            settle();
            (void)view.grabWindow();
            const auto pending_at = text_at(root, prompt);
            for (const auto& line : fixture.prompt)
                file.write(line + '\n');
            file.flush();
            model.readMore();
            settle();
            (void)view.grabWindow();
            const auto recorded_at = text_at(root, prompt);
            expect(pending_shown(root).isEmpty() && prompt_rows(model, prompt) == 1,
                   what("its record takes its place once"));
            expect(pending_at.size() == 1 && recorded_at.size() == 1 &&
                       std::abs(pending_at.front().y() - recorded_at.front().y()) <= 1 &&
                       std::abs(pending_at.front().x() - recorded_at.front().x()) <= 1,
                   what("the record replaces the bubble without a jump"));
            root->setProperty("working", true);
            (void)view.grabWindow();
            root->setProperty("working", false);
            // Two sent quickly keep their order until each is recorded.
            const auto first = QStringLiteral("First of two");
            const auto second = QStringLiteral("Second of two");
            (void)send(first);
            (void)send(second);
            expect(pending_shown(root) == QStringList({first, second}),
                   what("two keep their order"));
            root->setProperty("working", false);
        }
        // The check after sending, with a stand-in input box, on a shorter
        // clock than the app's 1.5 s.
        constexpr int check_ms = 200;
        root->setProperty("sendCheckMs", check_ms);
        const auto prompt = QStringLiteral("check the build");
        fake.setProperty("swallow", 1); // the Return sent with the text is lost
        (void)send(prompt);
        wait(check_ms + 120);
        expect(fake.property("presses").toInt() == 1 && fake.property("held").toString().isEmpty(),
               QStringLiteral("%1: a box still holding it gets one more Return").arg(name));
        wait(check_ms + 120);
        expect(fake.property("presses").toInt() == 1 &&
                   root->property("unsentText").toString().isEmpty(),
               QStringLiteral("%1: and once taken, nothing more").arg(name));
        fake.setProperty("presses", 0);
        fake.setProperty("swallow", 2); // that Return is lost too
        (void)send(prompt);
        wait(2 * check_ms + 200);
        (void)view.grabWindow();
        const auto notes = shown_named(root, "pendingPrompt");
        bool noted = false;
        for (auto* item : notes)
            noted = noted || item->property("unsent").toBool();
        expect(
            fake.property("presses").toInt() == 1 && noted,
            QStringLiteral("%1: never a second extra Return; the bubble says not sent").arg(name));
        fake.setProperty("presses", 0);
        fake.setProperty("swallow", 0);
        fake.setProperty("held", QString());
        (void)send(QStringLiteral("taken at once"));
        wait(check_ms + 120);
        expect(fake.property("presses").toInt() == 0,
               QStringLiteral("%1: a prompt taken at once gets no extra Return").arg(name));
    }
    root->setProperty("working", false);
    root->setProperty("screen", QVariant());
    root->setProperty("sendCheckMs", 1500);
    std::puts(failures == 0 ? "bubble: all checks passed" : "bubble: failed");
    return failures;
}

// A made-up running Claude Code turn with subagents and a background
// command, timed from now: `ago` seconds before it.
QString stamp(int ago) {
    return QDateTime::currentDateTimeUtc().addSecs(-ago).toString(Qt::ISODateWithMs);
}
QByteArray launch(const QString& id, const QString& agent, const QString& what, int ago) {
    return QStringLiteral(
               R"({"type":"assistant","timestamp":"%1","message":{"content":[{"type":"tool_use","id":"%2","name":"Agent","input":{"description":"%3","subagent_type":"Explore","run_in_background":true,"prompt":"Look"}}]}})"
               "\n"
               R"({"type":"user","timestamp":"%4","toolUseResult":{"status":"async_launched","agentId":"%5"},"message":{"content":[{"type":"tool_result","tool_use_id":"%2","content":"launched"}]}})"
               "\n")
        .arg(stamp(ago), id, what, stamp(ago - 1), agent)
        .toUtf8();
}

// The running strip: subagents and a background command listed under the
// live box while the agent works, under "Waiting on ..." once its turn
// ended, a subagent opening to its own steps, more than four summed up, and
// each leaving once it ends.
int check_work(QQuickView& view, QQuickItem* root, TranscriptModel& model, const QString& folder) {
    int failures = 0;
    const auto expect = [&failures](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "work FAIL %s\n", what);
            ++failures;
        }
    };
    const auto path = QDir(folder).filePath(QStringLiteral("work.jsonl"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    file.write(
        QStringLiteral(R"({"type":"user","timestamp":"%1","message":{"content":"Split it"}})")
            .arg(stamp(300))
            .toUtf8() +
        '\n');
    file.write(
        launch(QStringLiteral("g1"), QStringLiteral("ag1"), QStringLiteral("Map the lexer"), 280));
    file.write(
        launch(QStringLiteral("g2"), QStringLiteral("ag2"), QStringLiteral("Check errors"), 260));
    file.write(
        QStringLiteral(
            R"({"type":"assistant","timestamp":"%1","message":{"content":[{"type":"tool_use","id":"b1","name":"Bash","input":{"command":"npm test --watch","run_in_background":true}}]}})"
            "\n"
            R"({"type":"user","timestamp":"%2","toolUseResult":{"backgroundTaskId":"bg1"},"message":{"content":[{"type":"tool_result","tool_use_id":"b1","content":"Command running in background with ID: bg1."}]}})"
            "\n")
            .arg(stamp(240), stamp(239))
            .toUtf8());
    file.flush();
    // The second subagent's own file, two steps in.
    const auto own = QDir(folder).filePath(QStringLiteral("work/subagents/agent-ag2.jsonl"));
    QDir().mkpath(QFileInfo(own).path());
    QFile steps(own);
    if (steps.open(QIODevice::WriteOnly)) {
        for (const auto& [name, input] : {std::pair{"Grep", R"({"pattern":"error("})"},
                                          std::pair{"Bash", R"({"command":"ctest"})"}})
            steps.write(
                QStringLiteral(
                    R"({"type":"assistant","isSidechain":true,"timestamp":"%1","message":{"content":[{"type":"tool_use","id":"%2","name":"%2","input":%3}]}})")
                    .arg(stamp(200), QString::fromUtf8(name), QString::fromUtf8(input))
                    .toUtf8() +
                '\n');
        steps.close();
    }
    model.setPath(path);
    model.loadNow();
    auto* list = root->property("list").value<QQuickItem*>();
    const auto end = [&] {
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        settle();
        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        settle();
    };
    // Each moment also checks the rows and the strip under them overlap
    // nothing.
    QStringList overlaps;
    int moment = 0;
    const auto look = [&] {
        (void)view.grabWindow();
        check_rows(list, QStringLiteral("work moment %1").arg(++moment), overlaps);
        settle();
    };
    root->setProperty("working", true);
    end();
    look();
    expect(model.running().size() == 3, "two subagents and a command run");
    auto strips = shown_named(root, "runningStrip");
    expect(named(root, "liveBox").size() == 1 && strips.size() == 1 &&
               strips.front()->height() < 4 * root->property("liveLineHeight").toReal() + 12,
           "while it works, one strip of three lines under the box");
    // The turn ends; its work runs on.
    file.write(
        QStringLiteral(
            R"({"type":"assistant","timestamp":"%1","message":{"content":[{"type":"text","text":"Started them."}]}})"
            "\n"
            R"({"type":"system","subtype":"turn_duration","durationMs":120000,"timestamp":"%2"})"
            "\n")
            .arg(stamp(180), stamp(180))
            .toUtf8());
    file.flush();
    model.readMore();
    root->setProperty("working", false);
    end();
    look();
    const auto busy = shown_named(root, "busyText");
    expect(busy.size() == 1 && busy.front()->property("text").toString().startsWith(
                                   QLatin1String("Waiting on 2 subagents and 1 background task")),
           "idle, the agent shows it waits on its work");
    strips = shown_named(root, "runningStrip");
    expect(named(root, "liveBox").empty() && strips.size() == 1, "the strip stays, under it");
    // A subagent's line opens to its own steps, once a frame has placed the
    // footer where the thread's end puts it (its items move when polished).
    (void)view.grabWindow();
    strips = shown_named(root, "runningStrip");
    if (!strips.empty()) {
        QQuickItem* line = nullptr;
        const auto walk = [&line](auto&& self, QQuickItem* item) -> void {
            const auto entry = item->property("entry").toMap();
            if (line == nullptr &&
                entry.value(QStringLiteral("body")).toString() == QLatin1String("Check errors"))
                line = item;
            for (auto* child : item->childItems())
                self(self, child);
        };
        walk(walk, strips.front());
        expect(line != nullptr, "the subagent is a line of the strip");
        if (line != nullptr) {
            click(view, line);
            for (int i = 0; i < 5; ++i)
                settle();
            const auto boxes = shown_named(root, "subagentList");
            expect(boxes.size() == 1 && boxes.front()->property("count").toInt() == 2,
                   "opened, it lists its own two steps");
            click(view, line);
        }
    }
    // More than fit: one line sums them up.
    for (int i = 3; i <= 5; ++i)
        file.write(launch(QStringLiteral("g%1").arg(i), QStringLiteral("ag%1").arg(i),
                          QStringLiteral("Extra %1").arg(i), 100));
    file.flush();
    model.readMore();
    end();
    look();
    const auto summary = shown_named(root, "runningSummary");
    expect(model.running().size() == 6 && summary.size() == 1 &&
               shown_named(root, "runningStrip").empty(),
           "six running collapse to one line");
    // Ends land: finished work leaves; nothing is left in the thread.
    const int rows = model.rowCount();
    for (int i = 1; i <= 5; ++i)
        file.write(
            QStringLiteral(
                R"({"type":"user","timestamp":"%1","message":{"content":"<task-notification><task-id>ag%2</task-id><status>completed</status><summary>Agent finished</summary></task-notification>"}})")
                .arg(stamp(10))
                .arg(i)
                .toUtf8() +
            '\n');
    file.write(
        QStringLiteral(
            R"({"type":"user","timestamp":"%1","message":{"content":"<task-notification><task-id>bg1</task-id><status>completed</status><summary>Background command completed (exit code 0)</summary></task-notification>"}})")
            .arg(stamp(5))
            .toUtf8() +
        '\n');
    file.flush();
    model.readMore();
    end();
    expect(model.running().isEmpty() && shown_named(root, "runningStrip").empty() &&
               shown_named(root, "runningSummary").empty(),
           "all ended: the strip goes");
    expect(model.rowCount() == rows, "and leaves no finished rows behind");
    look();
    for (const auto& overlap : overlaps)
        std::fprintf(stderr, "work FAIL rows: %s\n", qPrintable(overlap));
    expect(overlaps.empty(), "no row or strip overlaps another");
    std::puts(failures == 0 ? "work: all checks passed" : "work: failed");
    return failures == 0 ? 0 : 1;
}

// Earlier turns enough that a conversation opens as its end (a tail) and
// the rest comes in from another thread.
QList<QByteArray> long_history(const char* format) {
    QList<QByteArray> out;
    const auto filler =
        QStringLiteral("An earlier answer, long enough to fill the file. ").repeated(80);
    for (int i = 0; i < 500; ++i) {
        const auto n = QString::number(i);
        const auto at =
            QStringLiteral("2026-11-11T15:%1:00.000Z").arg(i % 60, 2, 10, QLatin1Char('0'));
        if (qstrcmp(format, "codex") == 0) {
            const auto turn = QStringLiteral("p") + n;
            const auto event = [&at](const QJsonObject& payload) {
                return compact({{"timestamp", at}, {"type", "event_msg"}, {"payload", payload}});
            };
            const auto item = [&](const QJsonObject& value) {
                return event({{"type", "item_completed"}, {"turn_id", turn}, {"item", value}});
            };
            out << event({{"type", "task_started"}, {"turn_id", turn}})
                << item({{"type", "UserMessage"},
                         {"id", QStringLiteral("pu") + n},
                         {"content", QJsonArray{QJsonObject{{"type", "text"},
                                                            {"text", "Earlier question " + n}}}}})
                << item({{"type", "AgentMessage"},
                         {"id", QStringLiteral("pa") + n},
                         {"phase", "final_answer"},
                         {"content", QJsonArray{QJsonObject{{"type", "Text"}, {"text", filler}}}}})
                << event({{"type", "task_complete"}, {"turn_id", turn}, {"duration_ms", 1000}});
        } else {
            out << compact({{"type", "user"},
                            {"uuid", QStringLiteral("pu") + n},
                            {"timestamp", at},
                            {"message", QJsonObject{{"content", "Earlier question " + n}}}})
                << compact({{"type", "assistant"},
                            {"uuid", QStringLiteral("pa") + n},
                            {"timestamp", at},
                            {"message",
                             QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"},
                                                                            {"text", filler}}}}}}})
                << compact(
                       {{"type", "system"}, {"subtype", "turn_duration"}, {"durationMs", 1000}});
        }
    }
    return out;
}

// A switch to a conversation not kept, then to one kept: a prompt sent
// while only the end shows stays shown when the whole thread replaces the
// end, and the bubble, the running turn's box, the running strip and a
// picture in place lay out over nothing in the thread just swapped from its
// end to the whole; then the same after switching away and back to it, now
// kept.
int check_swapped(QQuickView& view, QQuickItem* root, TranscriptModel& model,
                  const QString& folder) {
    int failures = 0;
    const auto expect = [&failures](bool ok, const QString& what) {
        if (!ok) {
            std::fprintf(stderr, "swapped FAIL %s\n", qPrintable(what));
            ++failures;
        }
    };
    QImage picture(240, 120, QImage::Format_ARGB32);
    picture.fill(QColor("#4a6fa5"));
    const auto picture_path = QDir(folder).filePath(QStringLiteral("swapped.png"));
    (void)picture.save(picture_path);
    QFile other(QDir(folder).filePath(QStringLiteral("swapped-other.jsonl")));
    if (!other.open(QIODevice::WriteOnly))
        return 1;
    other.write(R"({"type":"user","uuid":"o1","message":{"content":"Elsewhere"}})"
                "\n");
    other.close();
    for (const auto& fixture : sent_fixtures()) {
        const QString name = QString::fromLatin1(fixture.format);
        if (name == QLatin1String("antigravity"))
            continue; // its steps are numbered from the start; no history to add
        const bool claude = name == QLatin1String("claude");
        const auto what = [&name](const char* text) {
            return QStringLiteral("%1: %2").arg(name, QString::fromLatin1(text));
        };
        QFile file(QDir(folder).filePath(QStringLiteral("swapped-%1.jsonl").arg(name)));
        if (!file.open(QIODevice::WriteOnly))
            return 1;
        for (const auto& line : long_history(fixture.format))
            file.write(line + '\n');
        for (const auto& line : fixture.before)
            file.write(line + '\n');
        file.flush();
        root->setProperty("working", false);
        root->setProperty("sizeSetting", 15);
        model.setFormat(lapis::desktop::transcript::format_named(name));
        model.setPath(file.fileName());
        expect(model.tailShown(), what("a cold thread opens at its end"));
        // Sent before the rest of the thread comes in.
        const auto prompt = QStringLiteral("Now do stage one");
        root->setProperty("working", true);
        model.noteSent(prompt);
        QThreadPool::globalInstance()->waitForDone();
        for (int i = 0; i < 5; ++i)
            settle();
        (void)view.grabWindow();
        expect(!model.tailShown() && model.rowCount() > 1000,
               what("the whole thread replaced its end"));
        expect(pending_shown(root) == QStringList{prompt},
               what("the prompt sent meanwhile still shows"));
        // The CLI records it and works: the running turn's box, for Claude
        // Code a subagent in the running strip, then a reply with a picture.
        QList<QByteArray> records = fixture.prompt;
        if (claude)
            records << launch(QStringLiteral("sw1"), QStringLiteral("asw1"),
                              QStringLiteral("Look around"), 5)
                           .trimmed();
        int problems = check_sent(view, root, model, file, records, {}, {});
        expect(problems == 0, what("nothing overlaps while it is recorded"));
        expect(pending_shown(root).isEmpty() && prompt_rows(model, prompt) == 1,
               what("its record takes the bubble's place once"));
        expect(model.rowCount() > 0 &&
                   model.data(model.index(model.rowCount() - 1), TranscriptModel::KindRole)
                           .toString() == QLatin1String("live"),
               what("the running turn shows"));
        if (claude) {
            expect(named(root, "liveBox").size() == 1, what("the live work box shows"));
            expect(shown_named(root, "runningStrip").size() == 1, what("the running strip shows"));
            const QList<QByteArray> reply{
                compact({{"type", "assistant"},
                         {"uuid", "a2"},
                         {"timestamp", stamp(1)},
                         {"message",
                          QJsonObject{{"content",
                                       QJsonArray{QJsonObject{{"type", "text"},
                                                              {"text", "Drawn:\n\n" + picture_path +
                                                                           "\n\nDone."}}}}}}}),
                compact({{"type", "system"}, {"subtype", "turn_duration"}, {"durationMs", 4000}}),
                "#working 0"};
            problems = check_sent(view, root, model, file, reply, {}, {});
            expect(problems == 0, what("nothing overlaps once the reply lands"));
            QMetaObject::invokeMethod(root->property("list").value<QQuickItem*>(),
                                      "positionViewAtEnd");
            settle();
            expect(named(root, "mediaItem").size() == 1, what("the picture shows in place"));
        }
        // Away and back: it is kept now and shows at once, and a prompt sent
        // there lays out the same.
        root->setProperty("working", false);
        model.setPath(other.fileName());
        settle();
        expect(model.kept(file.fileName()), what("the thread left is kept"));
        QList<QByteArray> again{"#show", "#send"};
        for (auto line : fixture.prompt)
            again << line.replace("u2", "u3")
                         .replace("\"t2\"", "\"t3\"")
                         .replace("Now do stage one", "Now do stage two");
        problems =
            check_sent(view, root, model, file, again, QStringLiteral("Now do stage two"), {});
        expect(!model.tailShown(), what("the kept thread shows whole at once"));
        expect(problems == 0, what("nothing overlaps in the kept thread"));
        expect(pending_shown(root).isEmpty() &&
                   prompt_rows(model, QStringLiteral("Now do stage two")) == 1,
               what("the kept thread's bubble is replaced once"));
    }
    root->setProperty("working", false);
    std::puts(failures == 0 ? "swapped: all checks passed" : "swapped: failed");
    return failures;
}

// --open: the newest step whose verb starts with `verb` opened (the oldest
// in its "Worked for" with a leading ^), in the live box while it shows,
// else in its opened "Worked for", and shot there.
QImage open_step(QQuickView& view, QQuickItem* root, TranscriptModel& model, QObject* list,
                 QString verb) {
    const bool oldest = verb.startsWith(QLatin1Char('^'));
    if (oldest)
        verb.remove(0, 1);
    const auto& steps = model.liveStepsModel();
    for (int row = steps.rowCount() - 1; row >= 0; --row)
        if (steps.data(steps.index(row), TranscriptModel::TitleRole).toString().startsWith(verb)) {
            root->setProperty("liveOpenItem",
                              steps.data(steps.index(row), TranscriptModel::ItemRole).toInt());
            settle();
            return shot(view, list, -1);
        }
    root->setProperty("following", false);
    for (int work = model.rowCount() - 1; work >= 0; --work) {
        if (model.data(model.index(work), TranscriptModel::KindRole).toString() !=
            QLatin1String("work"))
            continue;
        model.toggleWork(work);
        std::vector<int> matches;
        for (int row = work + 1; row < model.rowCount(); ++row) {
            const auto index = model.index(row);
            if (model.data(index, TranscriptModel::InWorkRole).toBool() &&
                model.data(index, TranscriptModel::TitleRole).toString().startsWith(verb))
                matches.push_back(row);
        }
        if (!matches.empty()) {
            const int row = oldest ? matches.front() : matches.back();
            QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, row),
                                      Q_ARG(int, 3 /* ListView.Center */));
            settle();
            QQuickItem* delegate = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, delegate),
                                      Q_ARG(int, row));
            const auto walk = [](auto&& self, QQuickItem* item) -> QQuickItem* {
                if (item->objectName() == QLatin1String("step"))
                    return item;
                for (auto* child : item->childItems())
                    if (auto* hit = self(self, child))
                        return hit;
                return nullptr;
            };
            if (auto* step = delegate != nullptr ? walk(walk, delegate) : nullptr)
                step->setProperty("open", true);
            settle();
            // Long enough for the opened task to read its files.
            for (int i = 0; i < 10; ++i)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, row),
                                      Q_ARG(int, 1 /* ListView.Beginning */));
            settle();
            const auto image = view.grabWindow();
            root->setProperty("following", true);
            return image;
        }
        model.toggleWork(work);
    }
    std::fprintf(stderr, "no step \"%s\" to open\n", qPrintable(verb));
    root->setProperty("following", true);
    return view.grabWindow();
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

double ms_of(const QElapsedTimer& timer) { return static_cast<double>(timer.nsecsElapsed()) / 1e6; }

// The model side of a cold load, file by file, with no view: reading the
// bytes, parsing and folding the lines, and laying the thread out.
void switch_breakdown(const QStringList& paths) {
    namespace t = lapis::desktop::transcript;
    std::printf("breakdown (no view): MB, records, items, rows, read ms, parse+fold ms, lay out "
                "ms, tail (1 MB) ms and rows, copy ms, kept MB\n");
    for (const auto& path : paths) {
        QElapsedTimer timer;
        timer.start();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const auto bytes = file.readAll();
        const double read = ms_of(timer);
        timer.restart();
        auto reader = t::make_reader(t::format_of(path));
        int records = 0;
        for (qsizetype start = 0, end = bytes.indexOf('\n'); end >= 0;
             start = end + 1, end = bytes.indexOf('\n', start)) {
            reader->add(bytes.sliced(start, end - start));
            ++records;
        }
        const double parse = ms_of(timer);
        timer.restart();
        const auto layout = t::lay_out(reader->items(), {});
        const double lay = ms_of(timer);
        timer.restart();
        const auto tail = t::read_file(path, reader->format(), TranscriptModel::tail_bytes);
        auto tail_layout = t::lay_out(tail.reader->items(), {});
        if (tail.tail)
            t::trim_to_turns(tail_layout);
        const double tail_ms = ms_of(timer);
        timer.restart();
        const auto copy = reader->clone();
        const double copy_ms = ms_of(timer);
        std::printf("  %-11s %7.1f %7d %7zu %7zu %8.1f %8.1f %8.1f %8.1f %5zu %6.1f %7.1f\n",
                    qPrintable(t::format_name(reader->format())),
                    static_cast<double>(bytes.size()) / 1e6, records, reader->items().size(),
                    layout.rows.size(), read, parse, lay, tail_ms, tail_layout.rows.size(), copy_ms,
                    static_cast<double>(reader->footprint()) / (1024.0 * 1024.0));
    }
}

// --switch: shows the conversations one after another as the app does when
// the person selects another agent, and times each switch: the path set
// (the model's own work on the GUI thread), rows to show, the list laid
// out at the end (delegates made and styled), the first frame drawn, and
// the full history ready. The longest single pass of the event loop during
// a switch is the GUI thread's worst stall. Patterns: each file once (the
// first visit), a walk through all of them twice (Command-Shift-bracket),
// back and forth between two, and random picks.
int bench_switch(QQuickView& view, TranscriptModel& model, QObject* list, const QStringList& paths,
                 int idle_s, int pairs) {
    struct Times {
        double call{}, content{}, laid{}, frame{}, settled{}, stall{};
        bool loaded_at_once{};
    };
    std::map<std::string, std::vector<Times>> phases;
    std::vector<std::string> order;
    const double start_footprint = footprint_mb();
    double peak_footprint = start_footprint;
    bool stale = false;
    const auto run = [&](const std::string& phase, int index) {
        const auto& path = paths.at(index);
        if (phases.find(phase) == phases.end())
            order.push_back(phase);
        Times times;
        QElapsedTimer timer;
        timer.start();
        model.setPath(path);
        times.call = ms_of(timer);
        times.stall = times.call;
        times.loaded_at_once = !model.loading();
        // Rows of another conversation never show under this one's path.
        const auto pass = [&] {
            QElapsedTimer one;
            one.start();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            times.stall = std::max(times.stall, ms_of(one));
        };
        while (model.rowCount() == 0 && model.loading() && timer.elapsed() < 60000)
            pass();
        times.content = ms_of(timer);
        const auto draw = [&] {
            QMetaObject::invokeMethod(list, "forceLayout");
            QMetaObject::invokeMethod(list, "positionViewAtEnd");
            QMetaObject::invokeMethod(list, "forceLayout");
            QMetaObject::invokeMethod(list, "positionViewAtEnd");
        };
        QElapsedTimer one;
        one.start();
        draw();
        times.stall = std::max(times.stall, ms_of(one));
        times.laid = ms_of(timer);
        (void)view.grabWindow();
        times.frame = ms_of(timer);
        while (model.loading() && timer.elapsed() < 60000)
            pass();
        if (times.frame < ms_of(timer)) {
            draw();
            (void)view.grabWindow();
        }
        times.settled = ms_of(timer);
        if (model.path() != path)
            stale = true;
        phases[phase].push_back(times);
        peak_footprint = std::max(peak_footprint, footprint_mb());
        // A short dwell, as a person reads before moving on.
        for (QElapsedTimer dwell; !dwell.isValid() || dwell.elapsed() < 30;) {
            if (!dwell.isValid())
                dwell.start();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    };
    const int n = static_cast<int>(paths.size());
    for (int i = 0; i < n; ++i)
        run("first visit", i);
    for (int round = 0; round < 2; ++round)
        for (int i = 0; i < n; ++i)
            run("walk all", i);
    for (int i = 0; i < pairs && n >= 2; ++i)
        run("back and forth", n - 1 - (i % 2));
    std::uint32_t seed = 12345;
    for (int i = 0; i < 40; ++i) {
        seed = seed * 1664525u + 1013904223u;
        run("random", static_cast<int>((seed >> 8) % static_cast<std::uint32_t>(n)));
    }
    std::printf("switch: %d conversations (offscreen, software scene graph), ms\n", n);
    std::printf("  %-15s %4s %5s  %-18s %-18s %-18s %-18s %-18s %-18s\n", "phase", "n", "warm",
                "path set p50/p95", "content p50/p95", "laid out p50/p95", "frame p50/p95",
                "settled p50/p95", "stall p50/max");
    for (const auto& phase : order) {
        const auto& all = phases[phase];
        std::vector<double> call, content, laid, frame, settled, stall;
        int warm = 0;
        for (const auto& t : all) {
            call.push_back(t.call);
            content.push_back(t.content);
            laid.push_back(t.laid);
            frame.push_back(t.frame);
            settled.push_back(t.settled);
            stall.push_back(t.stall);
            warm += t.loaded_at_once ? 1 : 0;
        }
        const auto pair = [](const std::vector<double>& v, double high) {
            return QStringLiteral("%1 / %2")
                .arg(percentile(v, 0.5), 0, 'f', 1)
                .arg(percentile(v, high), 0, 'f', 1);
        };
        std::printf("  %-15s %4zu %5d  %-18s %-18s %-18s %-18s %-18s %-18s\n", phase.c_str(),
                    all.size(), warm, qPrintable(pair(call, 0.95)), qPrintable(pair(content, 0.95)),
                    qPrintable(pair(laid, 0.95)), qPrintable(pair(frame, 0.95)),
                    qPrintable(pair(settled, 0.95)), qPrintable(pair(stall, 1.0)));
    }
    std::printf("  footprint before %.0f MB, now %.0f MB, peak seen %.0f MB; %d kept, estimated "
                "%.0f MB\n",
                start_footprint, footprint_mb(), peak_footprint, model.keptCount(),
                static_cast<double>(model.keptBytes()) / (1024.0 * 1024.0));
    if (idle_s > 0) {
        // Idle with the conversations warm: the process's own CPU time.
        rusage before{};
        getrusage(RUSAGE_SELF, &before);
        // A real event loop that sleeps between events, as the app's does.
        QEventLoop idle;
        QTimer::singleShot(idle_s * 1000, &idle, &QEventLoop::quit);
        idle.exec();
        rusage after{};
        getrusage(RUSAGE_SELF, &after);
        const auto cpu = [](const rusage& r) {
            return static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
                   static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
        };
        std::printf("  idle %d s: cpu %.3f s (%.2f%% of one core)\n", idle_s,
                    cpu(after) - cpu(before),
                    100.0 * (cpu(after) - cpu(before)) / static_cast<double>(idle_s));
    }
    if (stale) {
        std::puts("  a switch ended on another conversation's path");
        return 1;
    }
    return 0;
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
                      "steps opening, diffs only when opened), the running strip, the sent "
                      "prompt's bubble, and those again in a thread just swapped from its end "
                      "to the whole and in a kept one, then exit"});
    parser.addOption({"media-checks",
                      "Check a made-up reply's picture and file: click, drag out, right-click "
                      "menu and its actions (with --out, a capture of the open menu), then exit"});
    parser.addOption({"panel", "Save only the panel whose caption starts with this", "caption"});
    parser.addOption({"open",
                      "Open the newest step whose verb starts with this (in the live box, else in "
                      "its opened \"Worked for\") and shoot it as the \"Opened\" panel",
                      "verb"});
    parser.addOption({"working", "Show the agent as working (the live box), with no screen"});
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
    parser.addOption({"send",
                      "Send this prompt to the conversation shown, then append the records of "
                      "--then to a copy of its file, checking the thread's rows at each moment",
                      "text"});
    parser.addOption({"then", "With --send, the CLI's records to append (a JSONL file)", "file"});
    parser.addOption({"switch",
                      "Time switching among the conversations listed in this file (one path a "
                      "line) as selecting agents does; prints timings, sizes and counts only",
                      "file"});
    parser.addOption({"breakdown", "With --switch, first time each file's read, parse and lay "
                                   "out with no view"});
    parser.addOption(
        {"idle", "With --switch, then measure idle CPU for this many seconds", "s", "0"});
    parser.addOption(
        {"pairs", "With --switch, switches back and forth between the last two", "n", "20"});
    parser.process(app);
    const bool keys = parser.isSet("keys");
    QStringList switch_paths;
    if (parser.isSet("switch")) {
        QFile list_file(parser.value("switch"));
        if (!list_file.open(QIODevice::ReadOnly)) {
            std::fputs("could not read the --switch list\n", stderr);
            return 2;
        }
        for (const auto& line : QString::fromUtf8(list_file.readAll()).split(QLatin1Char('\n')))
            if (!line.trimmed().isEmpty())
                switch_paths << line.trimmed();
        if (parser.isSet("breakdown"))
            switch_breakdown(switch_paths);
    }
    const bool switching = !switch_paths.isEmpty();
    const bool live_checks = parser.isSet("live-checks");
    const bool media_checks = parser.isSet("media-checks");
    const bool checking = parser.isSet("check");
    // --live-checks writes its own made-up running turn and appends to it.
    QFile live_file(home.filePath(QStringLiteral("live.jsonl")));
    QFile media_file(home.filePath(QStringLiteral("media.jsonl")));
    if (media_checks) {
        if (!media_file.open(QIODevice::WriteOnly)) {
            std::fputs("could not write the media fixture\n", stderr);
            return 2;
        }
        write_media_fixture(home.path(), media_file);
    }
    if (live_checks) {
        if (!live_file.open(QIODevice::WriteOnly)) {
            std::fputs("could not write the live fixture\n", stderr);
            return 2;
        }
        for (const char* line : kLiveTurn)
            live_file.write(QByteArray(line) + '\n');
        live_file.flush();
    }
    const bool sending = parser.isSet("send");
    QList<QByteArray> sent_records;
    if (sending) {
        // A copy of the conversation to append to; the file stays as it is.
        QFile source(parser.value("jsonl"));
        QFile then(parser.value("then"));
        if (!source.open(QIODevice::ReadOnly) || !live_file.open(QIODevice::WriteOnly) ||
            (parser.isSet("then") && !then.open(QIODevice::ReadOnly))) {
            std::fputs("could not copy the conversation\n", stderr);
            return 2;
        }
        live_file.write(source.readAll());
        live_file.flush();
        if (then.isOpen())
            for (const auto& line : then.readAll().split('\n'))
                if (!line.trimmed().isEmpty())
                    sent_records << line;
    }
    if (!keys && !live_checks && !media_checks && !switching &&
        (!parser.isSet("jsonl") || (!parser.isSet("out") && !parser.isSet("census") &&
                                    !parser.isSet("list") && !checking && !sending))) {
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
    lapis::desktop::MediaActions media_actions;
    if (parser.isSet("format")) {
        const auto format = lapis::desktop::transcript::format_named(parser.value("format"));
        if (!format) {
            std::fputs("--format takes claude, codex or antigravity\n", stderr);
            return 2;
        }
        model.setFormat(format);
    }
    if (!switching) {
        model.setPath(live_checks || sending ? live_file.fileName()
                      : media_checks         ? media_file.fileName()
                                             : parser.value("jsonl"));
        model.loadNow();
    }
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
    const auto first_of = [](const QStringList& wanted, QFontDatabase::SystemFont fallback) {
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
    // What the window gives it, for pictures' cards; nothing here is clicked.
    root->setProperty("mediaActions", QVariant::fromValue(static_cast<QObject*>(&media_actions)));
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
    if (parser.isSet("working")) {
        root->setProperty("interrupt", QVariant::fromValue(view.engine()->evaluate(
                                           QStringLiteral("(function () {})"))));
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
    if (switching)
        return bench_switch(view, model, list, switch_paths, parser.value("idle").toInt(),
                            parser.value("pairs").toInt());
    if (live_checks) {
        const int live = check_live(view, root, model, live_file);
        const int work = check_work(view, root, model, home.path());
        const int bubbles = check_bubble(view, root, model, home.path());
        const int overlaps = check_sent_after_reply(view, root, model, home.path());
        std::puts(overlaps == 0 ? "sent after a reply: all checks passed"
                                : "sent after a reply: failed");
        const int ended = check_turn_ended(view, root, model, home.path(), parser.value("out"));
        std::puts(ended == 0 ? "working after the turn ended: all checks passed"
                             : "working after the turn ended: failed");
        const int swapped = check_swapped(view, root, model, home.path());
        return live == 0 && work == 0 && bubbles == 0 && overlaps == 0 && ended == 0 && swapped == 0
                   ? 0
                   : 1;
    }
    if (sending) {
        const int problems = check_sent(view, root, model, live_file, sent_records,
                                        parser.value("send"), parser.value("out"));
        std::printf("send: %d row problems\n", problems);
        return problems == 0 ? 0 : 1;
    }
    if (media_checks)
        return check_media(view, root, parser.value("out"));
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
    if (parser.isSet("open"))
        panels.push_back(
            {QStringLiteral("Opened"), open_step(view, root, model, list, parser.value("open"))});
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
