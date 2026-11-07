// Renders a Claude Code conversation in the visual view, offscreen with the
// software scene graph, and saves one contact sheet: the latest items, the
// latest edit and the latest failed tool side by side. --bench also scrolls
// the whole conversation and reports frame times. No window is shown and the
// process exits when done.
//
//   lapis_transcript_capture --jsonl FILE --out SHEET.png [--bench]
#include "transcript.hpp"

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QSGRendererInterface>
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
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"jsonl", "Claude Code session file", "file"});
    parser.addOption({"out", "Contact sheet PNG", "file"});
    parser.addOption({"width", "Panel width", "px", "900"});
    parser.addOption({"height", "Panel height", "px", "1200"});
    parser.addOption({"font-size", "Text size", "px", "15"});
    parser.addOption({"bench", "Scroll the whole conversation and report frame times"});
    parser.process(app);
    if (!parser.isSet("jsonl") || !parser.isSet("out")) {
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
    root->setProperty("fontSize", parser.value("font-size").toInt());
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
    root->setProperty("sansFamily", sans);
    root->setProperty("monoFamily", mono);
    std::printf("fonts: sans %s, mono %s\n", qPrintable(sans), qPrintable(mono));
    root->setProperty("model", QVariant::fromValue(static_cast<QObject*>(&model)));
    auto* list = root->property("list").value<QObject*>();
    view.show(); // offscreen: nothing reaches the screen
    settle();

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
    if (edit >= 0)
        panels.push_back({QStringLiteral("Latest edit"), shot(view, list, edit)});
    if (failure >= 0)
        panels.push_back({QStringLiteral("Latest failed tool"), shot(view, list, failure)});
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
        QMetaObject::invokeMethod(list, "positionViewAtBeginning");
        settle();
        std::vector<double> frames;
        QElapsedTimer total;
        total.start();
        for (int step = 0; step < 4000; ++step) {
            const bool at_end = list->property("atYEnd").toBool();
            if (at_end)
                break;
            list->setProperty("contentY", list->property("contentY").toReal() + 240.0);
            QElapsedTimer frame;
            frame.start();
            QCoreApplication::processEvents();
            (void)view.grabWindow();
            frames.push_back(static_cast<double>(frame.nsecsElapsed()) / 1e6);
        }
        std::printf("bench (offscreen software scene graph, includes readback): %zu frames in %lld ms\n"
                    "  p50 %.2f ms  p95 %.2f ms  p99 %.2f ms  max %.2f ms\n",
                    frames.size(), static_cast<long long>(total.elapsed()), percentile(frames, 0.50),
                    percentile(frames, 0.95), percentile(frames, 0.99), percentile(frames, 1.0));
    }
    return 0;
}
