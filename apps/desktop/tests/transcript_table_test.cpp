// A reply's tables: where they are cut from the reply, how their columns
// share the room, and how they are laid into the view's document.
#include "transcript.hpp"
#include "transcript_table.hpp"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextTable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>

using lapis::desktop::TranscriptModel;
namespace transcript = lapis::desktop::transcript;

namespace {
int failures = 0;

void require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

qreal sum(const std::vector<qreal>& widths) {
    return std::accumulate(widths.begin(), widths.end(), qreal{0});
}

// The fitting rules, on made-up measures in characters of 10 px.
void columns_share_the_room() {
    transcript::TableRules rules;
    rules.ch = 10;
    const auto column = [](qreal natural, qreal word, qreal header, qreal weight) {
        return transcript::TableColumn{natural * 10, word * 10, header * 10, header * 10,
                                       weight * 10};
    };
    // A label, an id, long prose, a status phrase, a decision.
    const std::vector<transcript::TableColumn> status{
        column(9, 8, 5, 60), column(30, 11, 5, 200), column(240, 12, 16, 1400),
        column(36, 9, 5, 150), column(90, 11, 4, 300)};

    // Wide room: short columns at their natural width, the rest each at
    // least the minimum and at most the cap, and the room used up.
    auto widths = transcript::table_widths(status, 1800, rules);
    require(widths[0] == 90, "a short column keeps its natural width");
    require(widths[2] <= 640 && widths[2] >= 200, "prose stays between the minimum and the cap");
    require(widths[1] >= 200 && widths[3] >= 200 && widths[4] >= 200,
            "every wrapping column has its minimum");
    require(sum(widths) <= 1800 && sum(widths) >= 1790, "the room is used, never passed");
    require(widths[2] > widths[4] && widths[4] > widths[3],
            "more text gets more of the spare room");
    for (const auto w : widths)
        require(w == std::floor(w), "widths are whole pixels");

    // Narrow room: nothing below its minimum; the table is wider than the
    // room and scrolls.
    widths = transcript::table_widths(status, 500, rules);
    require(widths[0] == 90 && widths[2] == 200 && sum(widths) > 500,
            "too little room keeps the minimums and overflows");

    // Very wide room: every column stops at its natural width or the cap.
    widths = transcript::table_widths(status, 10000, rules);
    require(widths[1] == 300 && widths[3] == 360 && widths[2] == 640 && widths[4] == 640,
            "columns stop at their natural width or the cap");
    require(sum(widths) < 10000, "a table does not stretch past what its text needs");

    // A long header over a short column wraps only past the short limit.
    widths = transcript::table_widths({{40, 40, 500, 90, 200}}, 2000, rules);
    require(widths[0] == 240, "a long header over short cells wraps at the short limit");
    // A long unbreakable run widens its column only up to the word cap.
    widths = transcript::table_widths({column(80, 70, 4, 80)}, 210, rules);
    require(widths[0] == 320, "a long path widens its column only to the word cap");
}

void cells_and_rows_are_read() {
    require(transcript::table_cells(QStringLiteral("| a | `b|c` | d\\|e |")) ==
                QStringList({"a", "`b|c`", "d|e"}),
            "cells split at bare pipes only");
    require(transcript::table_cells(QStringLiteral("a | b")) == QStringList({"a", "b"}),
            "outer pipes are optional");
    require(transcript::table_delimiter(QStringLiteral("|---|:--:|--:|")) &&
                transcript::table_delimiter(QStringLiteral("--- | ---")) &&
                !transcript::table_delimiter(QStringLiteral("---")) &&
                !transcript::table_delimiter(QStringLiteral("| a | b |")),
            "delimiter rows are told from rules and rows");
    require(
        transcript::table_header(QStringLiteral("| a | b |"), QStringLiteral("|---|---|")) &&
            !transcript::table_header(QStringLiteral("| a |"), QStringLiteral("|---|---|")) &&
            !transcript::table_header(QStringLiteral("- | a | b |"), QStringLiteral("|---|---|")) &&
            !transcript::table_header(QStringLiteral("> | a | b |"), QStringLiteral("|---|---|")),
        "a header matches its delimiter row and is not in a list or quote");
    const auto align = transcript::table_alignments(QStringLiteral("|a|b|c|d|\n|:--|:-:|--:|---|"));
    require(align ==
                QList<int>({int(Qt::AlignLeft), int(Qt::AlignHCenter), int(Qt::AlignRight), 0}),
            "alignment colons are read");
    require(transcript::numeric_column(
                {"1", "1,702.9", "-3%", "$2.10", "18.2 GB", "**4x**", "", "-", "1e-4"}),
            "numbers, percents, amounts and units are numeric");
    require(!transcript::numeric_column({"1", "two"}) && !transcript::numeric_column({"", "-"}),
            "words or no numbers are not");
}

void tables_are_pieces() {
    using transcript::pieces;
    auto parts =
        pieces(QStringLiteral("Intro\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\nAfter"));
    require(parts.size() == 3 && !parts[0].table && parts[1].table && !parts[2].table,
            "a table is a piece between its prose");
    require(parts[1].text == QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |"),
            "the table piece keeps its rows as written");
    parts = pieces(QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |\nnot a row"));
    require(parts.size() == 2 && parts[0].table && parts[1].text == QStringLiteral("not a row"),
            "a line without a pipe ends the table");
    parts = pieces(QStringLiteral("- item\n  | a | b |\n  |---|---|\n> | a |\n> |---|"));
    require(parts.size() == 1 && !parts[0].table, "tables in lists and quotes stay in the prose");
    parts = pieces(QStringLiteral("```\n| a | b |\n|---|---|\n```"));
    require(parts.size() == 1 && parts[0].code, "a table in a code block is code");
    // Streaming: the header alone is prose until its delimiter row arrives.
    parts = pieces(QStringLiteral("| a | b |\n|--"));
    require(parts.size() == 1 && !parts[0].table, "a half-written delimiter row is prose");
}

QVariantMap style(int size) {
    return {{QStringLiteral("sans"), QStringLiteral("Helvetica")},
            {QStringLiteral("mono"), QStringLiteral("Menlo")},
            {QStringLiteral("size"), size},
            {QStringLiteral("codeSize"), static_cast<int>(size * 0.86)},
            {QStringLiteral("codeColor"), QColor("#111111")},
            {QStringLiteral("inlineCodeColor"), QColor("#202020")},
            {QStringLiteral("inlineCodeTextColor"), QColor("#cbcbcb")},
            {QStringLiteral("linkColor"), QColor("#339cff")},
            {QStringLiteral("borderColor"), QColor("#444444")},
            {QStringLiteral("quoteColor"), QColor("#888888")},
            {QStringLiteral("headerColor"), QColor("#bbbbbb")}};
}

// A made-up status table: two short label columns, long prose, a status,
// a bold decision; `rows` body rows.
QString status_table(int rows) {
    QString out = QStringLiteral("| Group | Agent | What it is doing | State | Call |\n"
                                 "|---|---|---|---|---|\n");
    for (int i = 0; i < rows; ++i)
        out += QStringLiteral("| ops | Worker %1 | Runs the nightly suite on both models, "
                              "compares against last week's numbers and writes the summary "
                              "to `runs/%1/summary.md`<br>Last run: %1 h ago | Waiting on "
                              "review | **Keep**, then retire |\n")
                   .arg(i);
    return out;
}

struct View {
    QQmlEngine engine;
    QQmlComponent component{&engine};
    std::unique_ptr<QObject> edit;
    QQuickTextDocument* document{};
    View() {
        // As the thread's Selectable: rich text that wraps.
        component.setData("import QtQuick\n"
                          "TextEdit { textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap }",
                          QUrl());
        edit.reset(component.create());
        document = edit ? edit->property("textDocument").value<QQuickTextDocument*>() : nullptr;
    }
    // As the thread does: the TextEdit as wide as the room, or the table
    // when wider.
    QVariantMap lay(const TranscriptModel& model, const QString& text, const QVariantMap& s,
                    qreal available) {
        edit->setProperty("width", available);
        auto fit = model.styleTable(document, text, s, available);
        edit->setProperty("width",
                          std::max(fit.value(QStringLiteral("width")).toReal(), available));
        return fit;
    }
};

QTextTable* table_in(QTextDocument* document) {
    for (auto* frame : document->rootFrame()->childFrames())
        if (auto* table = qobject_cast<QTextTable*>(frame))
            return table;
    return nullptr;
}

int lines_in(const QTextTableCell& cell) {
    int lines = 0;
    for (auto block = cell.firstCursorPosition().block();
         block.isValid() && block.position() <= cell.lastPosition(); block = block.next())
        lines += block.layout()->lineCount();
    return lines;
}

void tables_are_laid_out() {
    View view;
    require(view.document != nullptr, "a TextEdit gives a document to lay a table in");
    if (view.document == nullptr)
        return;
    TranscriptModel model;
    const auto text = status_table(3);
    auto fit = view.lay(model, text, style(23), 1900);
    auto* document = view.document->textDocument();
    auto* table = table_in(document);
    require(table != nullptr && table->rows() == 4 && table->columns() == 5,
            "the table and all its rows are kept (a <br> in a cell does not end it)");
    if (table == nullptr)
        return;
    const auto rows = fit.value(QStringLiteral("rows")).toList();
    require(rows.size() == 5 && fit.value(QStringLiteral("header")).toBool(),
            "a top for each row and the bottom, with a header");
    require(rows[1].toReal() > rows[0].toReal() && rows[4].toReal() > rows[3].toReal(),
            "rows go down the page");
    const auto width = fit.value(QStringLiteral("width")).toReal();
    require(width > 1000 && width <= 1900, "a wide room is used, never passed");
    require(lines_in(table->cellAt(1, 0)) == 1 && lines_in(table->cellAt(1, 1)) == 1 &&
                lines_in(table->cellAt(1, 3)) == 1,
            "short columns never wrap");
    require(lines_in(table->cellAt(1, 2)) >= 3, "the prose column wraps, with its <br>");
    const auto prose_cell = table->cellAt(1, 2).firstCursorPosition().block();
    require(document->documentLayout()->blockBoundingRect(prose_cell).width() <=
                table->format().columnWidthConstraints().at(2).rawValue(),
            "cells are broken at their column's width");
    // Cells in the tighter type.
    const auto first = table->cellAt(1, 2).firstCursorPosition().block();
    require(first.blockFormat().lineHeight() == 135 &&
                first.begin().fragment().charFormat().font().pixelSize() == 21,
            "cells are 0.9x the body size at 1.35 line height");
    require(table->cellAt(0, 0)
                    .firstCursorPosition()
                    .block()
                    .begin()
                    .fragment()
                    .charFormat()
                    .fontWeight() >= QFont::DemiBold,
            "the header is heavier");
    require(table->cellAt(1, 0)
                    .firstCursorPosition()
                    .block()
                    .begin()
                    .fragment()
                    .charFormat()
                    .fontWeight() == QFont::Medium,
            "a label column is a little heavier");

    // A narrower room refits the same document without laying it in again.
    const auto before = document->property("lapisTable");
    fit = view.lay(model, text, style(23), 1000);
    require(document->property("lapisTable") == before && table_in(document) == table,
            "a new width refits the columns in place");
    const auto refit = fit.value(QStringLiteral("width")).toReal();
    require(refit <= 1000 && refit < width, "a narrower room narrows the table");
    require(lines_in(table->cellAt(1, 2)) > 3, "the refit cells are broken again");
    fit = view.lay(model, text, style(23), 500);
    require(fit.value(QStringLiteral("width")).toReal() > 500,
            "too narrow a room keeps the minimums and scrolls");

    // Numbers line up on the right.
    view.lay(model, QStringLiteral("| n | ms |\n|---|---|\n| 1 | 7.1 |\n| 16 | 1,702.9 |"),
             style(15), 800);
    table = table_in(view.document->textDocument());
    require(table != nullptr &&
                table->cellAt(1, 1).firstCursorPosition().blockFormat().alignment() &
                    Qt::AlignRight,
            "numeric columns align right");
    // Text that is no table is laid out as prose.
    fit = view.lay(model, QStringLiteral("just words"), style(15), 800);
    require(fit.value(QStringLiteral("width")).toReal() == 0 &&
                view.document->textDocument()->toPlainText() == QStringLiteral("just words"),
            "text with no table shows as prose");
}

// Rough cost of a 5x30 table: first lay-out (measured), the same table in a
// fresh document (as when a row is made again on switching back), and a refit.
double ms(const QElapsedTimer& timer) { return static_cast<double>(timer.nsecsElapsed()) / 1e6; }

void layout_cost() {
    const auto text = status_table(30);
    TranscriptModel model;
    QElapsedTimer timer;
    View first;
    timer.start();
    first.lay(model, text, style(23), 1900);
    const auto cold = ms(timer);
    double warm = 0;
    constexpr int repeats = 5;
    for (int i = 0; i < repeats; ++i) {
        View again;
        timer.restart();
        again.lay(model, text, style(23), 1900);
        warm += ms(timer);
    }
    timer.restart();
    first.lay(model, text, style(23), 1200);
    const auto refit = ms(timer);
    std::printf("table 5x30: first %.1f ms, again %.1f ms, refit %.1f ms\n", cold, warm / repeats,
                refit);
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    columns_share_the_room();
    cells_and_rows_are_read();
    tables_are_pieces();
    tables_are_laid_out();
    layout_cost();
    if (failures == 0)
        std::puts("transcript-table: all checks passed");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
