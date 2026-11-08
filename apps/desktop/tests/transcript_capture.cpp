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
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJSValue>
#include <QKeyEvent>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <algorithm>
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
// Return adds a line by default and Command-Return sends. Arrows carry the
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
                         qPrintable(QString(got).replace(QLatin1Char('\n'), QStringLiteral("\\n"))), at);
            ++failures;
        }
    };
    const auto set = [&](const QString& text) {
        input->setProperty("text", text);
        input->setProperty("cursorPosition", static_cast<int>(text.size()));
    };
    set(QStringLiteral("alpha beta.gamma\nsecond line"));
    key(Qt::Key_Left, Qt::MetaModifier);
    expect("Command-Left goes to the line start", QStringLiteral("alpha beta.gamma\nsecond line"), 17);
    key(Qt::Key_Right, Qt::AltModifier);
    expect("Option-Right ends the word", QStringLiteral("alpha beta.gamma\nsecond line"), 23);
    key(Qt::Key_Backspace, Qt::AltModifier);
    expect("Option-Backspace deletes the word", QStringLiteral("alpha beta.gamma\n line"), 17);
    key(Qt::Key_Backspace, Qt::MetaModifier);
    expect("Command-Backspace at a line start joins it", QStringLiteral("alpha beta.gamma line"), 16);
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
    key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    expect("Return adds a line by default", QStringLiteral("alpha \n"), 7);
    key(Qt::Key_Return, Qt::ShiftModifier, QStringLiteral("\r"));
    expect("Shift-Return adds a plain newline", QStringLiteral("alpha \n\n"), 8);
    key(Qt::Key_Return, Qt::MetaModifier, QStringLiteral("\r"));
    expect("Command-Return sends", QString(), -1);
    root->setProperty("sendOnReturn", true);
    set(QStringLiteral("hi"));
    key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    expect("Return sends when chosen", QString(), -1);
    // Typing a slash command lists the best matches; Return fills one in.
    root->setProperty("slashCommands", true);
    for (const auto letter : QStringLiteral("/comp"))
        key(letter == QLatin1Char('/') ? Qt::Key_Slash : Qt::Key_A + (letter.unicode() - 'a'), Qt::NoModifier,
            QString(letter));
    const auto matches = root->property("slashMatches").toList();
    if (matches.isEmpty() || matches.front().toMap().value(QStringLiteral("name")) != QStringLiteral("compact")) {
        std::fputs("keys FAIL \"/comp\" lists compact first\n", stderr);
        ++failures;
    }
    key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    expect("Return fills in the chosen command", QStringLiteral("/compact "), 9);
    std::puts(failures == 0 ? "keys: all checks passed" : "keys: failed");
    return failures == 0 ? 0 : 1;
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
    parser.addOption({"jsonl", "Claude Code session file", "file"});
    parser.addOption({"out", "Contact sheet PNG", "file"});
    parser.addOption({"width", "Panel width", "px", "900"});
    parser.addOption({"height", "Panel height", "px", "1200"});
    parser.addOption({"font-size", "Text size", "px", "15"});
    parser.addOption({"theme", "Visual theme name", "name", "terminal"});
    parser.addOption({"font", "Reading font name", "name", "system"});
    parser.addOption({"code-font", "Code font name", "name", "system"});
    parser.addOption({"terminal-background", "Background the terminal theme matches", "color", "#0f131c"});
    parser.addOption({"suggestion", "A Tab suggestion shown in the empty bar", "text", ""});
    parser.addOption({"bench", "Scroll the whole conversation and report frame times"});
    parser.addOption({"step", "Pixels scrolled per benchmark step", "px", "240"});
    parser.addOption({"keys", "Check the bar's Mac line editing and send keys, then exit"});
    parser.process(app);
    const bool keys = parser.isSet("keys");
    if (!keys && (!parser.isSet("jsonl") || !parser.isSet("out"))) {
        std::fputs("--jsonl and --out are required\n", stderr);
        return 2;
    }

    QElapsedTimer load_timer;
    load_timer.start();
    TranscriptModel model;
    model.setPath(parser.value("jsonl"));
    model.loadNow();
    const auto load_ms = load_timer.elapsed();

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
    root->setProperty("models", QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("claude-opus-5-5")},
                                                         {QStringLiteral("name"), QStringLiteral("Opus 5.5")}}});
    root->setProperty("efforts", QStringList{QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"),
                                             QStringLiteral("xhigh"), QStringLiteral("max")});
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
    const auto sans = first_of({QStringLiteral("SF Pro Text"), QStringLiteral("SF Pro"),
                                QStringLiteral(".AppleSystemUIFont"), QStringLiteral("Helvetica Neue")},
                               QFontDatabase::GeneralFont);
    const auto mono = first_of({QStringLiteral("SF Mono"), QStringLiteral("Menlo")}, QFontDatabase::FixedFont);
    root->setProperty("systemFamily", sans);
    root->setProperty("terminalFamily", mono);
    std::printf("fonts: sans %s, mono %s\n", qPrintable(sans), qPrintable(mono));
    root->setProperty("model", QVariant::fromValue(static_cast<QObject*>(&model)));
    auto* list = root->property("list").value<QObject*>();
    view.show(); // offscreen: nothing reaches the screen
    settle();
    // Long enough for the view's short timers (the chart style it writes).
    for (QElapsedTimer waited; !waited.isValid() || waited.elapsed() < 400;) {
        if (!waited.isValid())
            waited.start();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    if (keys)
        return check_keys(view, root);

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
    // Following would pull every later panel back to the end.
    root->setProperty("following", false);
    if (edit >= 0)
        panels.push_back({QStringLiteral("Latest edit"), shot(view, list, edit)});
    if (failure >= 0)
        panels.push_back({QStringLiteral("Latest failed tool"), shot(view, list, failure)});
    // The latest folded turn, closed and then opened.
    int work = -1;
    for (int row = model.rowCount() - 1; row >= 0 && work < 0; --row)
        if (model.data(model.index(row), TranscriptModel::KindRole).toString() == QLatin1String("work"))
            work = row;
    if (work >= 0) {
        panels.push_back({QStringLiteral("Latest turn, folded"), shot(view, list, work)});
        model.toggleWork(work);
        panels.push_back({QStringLiteral("Latest turn, opened"), shot(view, list, work + 2)});
        model.toggleWork(work);
    }
    root->setProperty("following", true);

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
    std::printf("items %d  load %lld ms  sheet %s\n", model.rowCount(), static_cast<long long>(load_ms),
                qPrintable(parser.value("out")));

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
        const auto ms = [](const QElapsedTimer& timer) { return static_cast<double>(timer.nsecsElapsed()) / 1e6; };
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
                const qreal bottom = list->property("contentY").toReal() + list->property("height").toReal();
                int at = -1;
                QMetaObject::invokeMethod(list, "indexAt", Q_RETURN_ARG(int, at), Q_ARG(qreal, 10.0),
                                          Q_ARG(qreal, bottom - 10));
                if (at >= 0) {
                    const auto index = model.index(at);
                    std::printf("  slow step %.0f ms at item %d: %s %s, body %lld chars, result %lld chars\n",
                                scrolls.back(), at,
                                qPrintable(model.data(index, TranscriptModel::KindRole).toString()),
                                qPrintable(model.data(index, TranscriptModel::TitleRole).toString()),
                                static_cast<long long>(model.data(index, TranscriptModel::BodyRole).toString().size()),
                                static_cast<long long>(model.data(index, TranscriptModel::ResultRole).toString().size()));
                }
            }
            QElapsedTimer render;
            render.start();
            (void)view.grabWindow();
            renders.push_back(ms(render));
            frames.push_back(ms(frame));
        }
        const auto line = [](const char* name, const std::vector<double>& values) {
            std::printf("  %-22s p50 %6.2f  p95 %6.2f  p99 %6.2f  max %7.2f ms\n", name, percentile(values, 0.50),
                        percentile(values, 0.95), percentile(values, 0.99), percentile(values, 1.0));
        };
        std::printf("bench: %zu steps of %.0f px in %lld ms (offscreen, software scene graph)\n", frames.size(), step_px,
                    static_cast<long long>(total.elapsed()));
        line("scroll + item creation", scrolls);
        line("render + readback", renders);
        line("whole step", frames);
    }
    return 0;
}
