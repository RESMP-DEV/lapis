#include "transcript_table.hpp"

#include "transcript.hpp"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextTable>
#include <algorithm>
#include <cmath>

namespace lapis::desktop {
namespace transcript {

std::vector<qreal> table_widths(const std::vector<TableColumn>& columns, qreal available,
                                const TableRules& rules) {
    const qreal ch = std::max<qreal>(1, rules.ch);
    const qreal short_limit = rules.short_chars * ch;
    const qreal min_width = rules.min_chars * ch;
    const qreal max_width = std::max(min_width, rules.max_chars * ch);
    const qreal word_cap = rules.word_chars * ch;
    const auto n = columns.size();
    std::vector<qreal> widths(n, 0);
    std::vector<qreal> caps(n, 0);
    std::vector<bool> growing(n, false);
    qreal used = 0;
    for (std::size_t c = 0; c < n; ++c) {
        const auto& column = columns[c];
        if (column.natural <= short_limit) {
            // Short: its natural width, with a header longer than that
            // wrapping at words only past the short limit.
            widths[c] = std::ceil(std::max({column.natural, std::min(column.header, short_limit),
                                            std::min(column.header_word, word_cap)}));
        } else {
            // Whole pixels, rounded up, so measured text is never wrapped
            // by a fraction; shares of the spare room round down below.
            widths[c] = std::ceil(std::max({min_width, std::min(column.word, word_cap),
                                            std::min(column.header_word, word_cap)}));
            caps[c] = std::max(
                widths[c], std::ceil(std::min(std::max(column.natural, column.header), max_width)));
            growing[c] = caps[c] > widths[c];
        }
        used += widths[c];
    }
    // What is left goes to the wrapping columns by their text, each up to
    // its cap; room a capped column cannot take goes round again.
    qreal spare = available - used;
    while (spare > 0.5) {
        qreal total = 0;
        for (std::size_t c = 0; c < n; ++c)
            if (growing[c])
                total += std::max(columns[c].weight, columns[c].natural);
        if (total <= 0)
            break;
        bool capped = false;
        for (std::size_t c = 0; c < n; ++c) {
            if (!growing[c])
                continue;
            const qreal share = spare * std::max(columns[c].weight, columns[c].natural) / total;
            if (widths[c] + share >= caps[c]) {
                spare -= caps[c] - widths[c];
                widths[c] = caps[c];
                growing[c] = false;
                capped = true;
            }
        }
        if (capped)
            continue; // shares change once a column stops
        for (std::size_t c = 0; c < n; ++c)
            if (growing[c]) {
                // Whole pixels, rounded down so the shares never pass the room.
                widths[c] = std::floor(
                    widths[c] + spare * std::max(columns[c].weight, columns[c].natural) / total);
                growing[c] = false;
            }
        spare = 0;
    }
    return widths;
}

namespace {
// The text a reader sees in a cell's markdown: emphasis and code marks gone.
QString bare(QString cell) {
    static const QRegularExpression marks(QStringLiteral("[*_`~]"));
    return cell.remove(marks).trimmed();
}
} // namespace

bool numeric_column(const QStringList& cells) {
    // A number with an optional sign, currency, grouping, decimals,
    // exponent, and a short unit or percent after it.
    static const QRegularExpression number(
        QStringLiteral(R"(^[(]?[-+\x{2212}~\x{2248}]?[$\x{20ac}\x{a3}\x{a5}]?\d[\d,_]*(\.\d+)?)"
                       R"(([eE][-+]?\d+)?\)?(\s?(%|x|\x{d7}|[A-Za-z\x{b5}/]{1,4}))?$)"));
    bool any = false;
    for (const auto& cell : cells) {
        const auto text = bare(cell);
        if (text.isEmpty() || text == QLatin1String("-") || text == QStringLiteral("—") ||
            text == QStringLiteral("–") ||
            text.compare(QLatin1String("n/a"), Qt::CaseInsensitive) == 0)
            continue;
        if (!number.match(text).hasMatch())
            return false;
        any = true;
    }
    return any;
}

QStringList table_cells(const QString& row) {
    QStringList cells;
    QString cell;
    qsizetype code = 0; // the backtick run that opened a code span, 0 outside one
    const auto text = row.trimmed();
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('\\') && i + 1 < text.size() && text[i + 1] == QLatin1Char('|')) {
            cell += QLatin1Char('|');
            ++i;
            continue;
        }
        if (c == QLatin1Char('`')) {
            qsizetype run = 1;
            while (i + run < text.size() && text[i + run] == QLatin1Char('`'))
                ++run;
            if (code == 0)
                code = run;
            else if (code == run)
                code = 0;
            cell += text.mid(i, run);
            i += run - 1;
            continue;
        }
        if (c == QLatin1Char('|') && code == 0) {
            cells << cell.trimmed();
            cell.clear();
            continue;
        }
        cell += c;
    }
    cells << cell.trimmed();
    if (text.startsWith(QLatin1Char('|')))
        cells.removeFirst();
    if (text.endsWith(QLatin1Char('|')) && !text.endsWith(QLatin1String("\\|")) && !cells.isEmpty())
        cells.removeLast();
    return cells;
}

bool table_delimiter(const QString& line) {
    static const QRegularExpression delimiter(
        QStringLiteral(R"(^ {0,3}\|?(\s*:?-+:?\s*\|)*\s*:?-+:?\s*\|?\s*$)"));
    return line.contains(QLatin1Char('|')) && delimiter.match(line).hasMatch();
}

bool table_header(const QString& header, const QString& delimiter) {
    // Indented: a list item's continuation or code.
    if (header.isEmpty() || header.front().isSpace())
        return false;
    const auto text = header.trimmed();
    static const QRegularExpression list(QStringLiteral(R"(^([-+*]|\d+[.)])\s)"));
    if (text.isEmpty() || !text.contains(QLatin1Char('|')) || text.startsWith(QLatin1Char('>')) ||
        list.match(text).hasMatch())
        return false;
    return table_cells(header).size() == table_cells(delimiter).size();
}

QList<int> table_alignments(const QString& markdown) {
    QList<int> out;
    const auto lines = markdown.split(QLatin1Char('\n'));
    for (const auto& line : lines) {
        if (!table_delimiter(line))
            continue;
        for (const auto& cell : table_cells(line)) {
            const bool left = cell.startsWith(QLatin1Char(':'));
            const bool right = cell.endsWith(QLatin1Char(':'));
            out.push_back(left && right ? int(Qt::AlignHCenter)
                          : right       ? int(Qt::AlignRight)
                          : left        ? int(Qt::AlignLeft)
                                        : 0);
        }
        break;
    }
    return out;
}

namespace {
// The cells' type, against the body text's size.
constexpr qreal cell_scale = 0.9;
constexpr int cell_line_height = 135; // percent
// Room around a cell's text, in cell-font pixels: generous sideways, as
// the gap between columns is the only thing between them, and tight above
// and below so a row is as short as its text.
constexpr qreal side_padding = 0.6;
constexpr qreal top_padding = 0.5;
constexpr qreal bottom_padding = 0.4;

int cell_size(const QVariantMap& style) {
    return static_cast<int>(
        std::lround(std::max(8, style.value(QStringLiteral("size")).toInt()) * cell_scale));
}

// `<br>` in a cell is a line break inside it (Qt's import ends the table
// at it otherwise).
QString prepared(QString markdown) {
    static const QRegularExpression br(QStringLiteral(R"(<br\s*/?>)"),
                                       QRegularExpression::CaseInsensitiveOption);
    return markdown.replace(br, QString(QChar::LineSeparator));
}

QTextTable* first_table(QTextDocument& document) {
    for (auto* frame : document.rootFrame()->childFrames())
        if (auto* table = qobject_cast<QTextTable*>(frame))
            return table;
    return nullptr;
}

// Each block of a cell.
template <typename F> void each_block(const QTextTableCell& cell, F&& f) {
    for (auto block = cell.firstCursorPosition().block();
         block.isValid() && block.position() <= cell.lastPosition(); block = block.next())
        f(block);
}

QString cell_text(const QTextTableCell& cell) {
    QTextCursor cursor = cell.firstCursorPosition();
    cursor.setPosition(cell.lastPosition(), QTextCursor::KeepAnchor);
    return cursor.selectedText();
}

// The cells' type and alignment, the header's weight and tone, a label
// column's slight weight, and their padding.
void dress(QTextTable& table, const QString& markdown, const QVariantMap& style,
           QList<bool>& numeric) {
    const int size = cell_size(style);
    const int code_size = static_cast<int>(
        std::lround(std::max(8, style.value(QStringLiteral("codeSize")).toInt()) * cell_scale));
    const auto header_color = style.value(QStringLiteral("headerColor")).value<QColor>();
    const int rows = table.rows();
    const int columns = table.columns();
    const auto explicit_alignment = table_alignments(markdown);
    numeric = QList<bool>(columns, false);
    for (int column = 0; column < columns; ++column) {
        QStringList body;
        for (int row = 1; row < rows; ++row)
            body << cell_text(table.cellAt(row, column));
        numeric[column] = rows > 1 && numeric_column(body);
    }

    auto format = table.format();
    format.setBorder(0);
    format.setBorderStyle(QTextFrameFormat::BorderStyle_None);
    format.setCellSpacing(0);
    format.setCellPadding(std::round(size * side_padding));
    format.setMargin(0);
    format.setAlignment(Qt::AlignLeft);
    format.setHeaderRowCount(rows > 1 ? 1 : 0);
    table.setFormat(format);

    struct CharChange {
        int position;
        int length;
        QTextCharFormat format;
    };
    std::vector<CharChange> chars;
    std::vector<std::pair<int, QTextBlockFormat>> blocks;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            auto cell = table.cellAt(row, column);
            auto cell_format = cell.format().toTableCellFormat();
            cell_format.setTopPadding(std::round(size * top_padding));
            cell_format.setBottomPadding(std::round(size * bottom_padding));
            cell_format.setLeftPadding(std::round(size * side_padding));
            cell_format.setRightPadding(std::round(size * side_padding));
            cell.setFormat(cell_format);
            const bool header = row == 0 && rows > 1;
            int align = explicit_alignment.value(column, 0);
            if (align == 0)
                align = numeric[column] ? int(Qt::AlignRight) : int(Qt::AlignLeft);
            each_block(cell, [&](const QTextBlock& block) {
                auto block_format = block.blockFormat();
                block_format.setLineHeight(cell_line_height, QTextBlockFormat::ProportionalHeight);
                block_format.setTopMargin(0);
                block_format.setBottomMargin(0);
                block_format.setAlignment(Qt::Alignment(align) | Qt::AlignAbsolute);
                blocks.emplace_back(block.position(), block_format);
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto part = it.fragment();
                    if (!part.isValid())
                        continue;
                    auto char_format = part.charFormat();
                    QFont font = char_format.font();
                    font.setPixelSize(char_format.fontFixedPitch() ? code_size : size);
                    if (header)
                        font.setWeight(std::max(font.weight(), QFont::DemiBold));
                    char_format.setFont(font, QTextCharFormat::FontPropertiesSpecifiedOnly);
                    if (header && header_color.isValid() && !char_format.isAnchor() &&
                        !char_format.fontFixedPitch())
                        char_format.setForeground(header_color);
                    chars.push_back({part.position(), part.length(), char_format});
                }
            });
        }
    }
    QTextCursor cursor(table.document());
    for (const auto& [position, block_format] : blocks) {
        cursor.setPosition(position);
        cursor.setBlockFormat(block_format);
    }
    for (const auto& change : chars) {
        cursor.setPosition(change.position);
        cursor.setPosition(change.position + change.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(change.format);
    }
}

// A label column (short words naming each row, first) reads a little
// heavier; known once the columns are measured.
void emphasise_labels(QTextTable& table) {
    QTextCursor cursor(table.document());
    for (int row = 1; row < table.rows(); ++row) {
        const auto cell = table.cellAt(row, 0);
        each_block(cell, [&](const QTextBlock& block) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto part = it.fragment();
                if (!part.isValid() || part.charFormat().fontWeight() >= QFont::Medium)
                    continue;
                cursor.setPosition(part.position());
                cursor.setPosition(part.position() + part.length(), QTextCursor::KeepAnchor);
                QTextCharFormat medium;
                medium.setFontWeight(QFont::Medium);
                cursor.mergeCharFormat(medium);
            }
        });
    }
}

// Each column's natural width, longest unbreakable run and text, from the
// document laid out wide enough that nothing wraps.
std::vector<TableColumn> measure(QTextDocument& document, QTextTable& table) {
    document.setTextWidth(1e6);
    (void)document.documentLayout()->documentSize();
    std::vector<TableColumn> out(static_cast<std::size_t>(table.columns()));
    for (int column = 0; column < table.columns(); ++column) {
        auto& measured = out[static_cast<std::size_t>(column)];
        for (int row = 0; row < table.rows(); ++row) {
            qreal natural = 0;
            qreal word = 0;
            each_block(table.cellAt(row, column), [&](const QTextBlock& block) {
                if (const auto* layout = block.layout()) {
                    natural = std::max(natural, layout->maximumWidth());
                    word = std::max(word, layout->minimumWidth());
                }
            });
            if (row == 0 && table.rows() > 1) {
                measured.header = natural;
                measured.header_word = word;
            } else {
                measured.natural = std::max(measured.natural, natural);
                measured.word = std::max(measured.word, word);
                measured.weight += natural;
            }
        }
    }
    return out;
}

TableRules rules_for(const QVariantMap& style) {
    QFont font(style.value(QStringLiteral("sans")).toString());
    font.setPixelSize(cell_size(style));
    static const QString sample = QStringLiteral("the quick brown fox jumps over a lazy dog");
    TableRules rules;
    rules.ch = QFontMetricsF(font).horizontalAdvance(sample) / static_cast<qreal>(sample.size());
    return rules;
}
} // namespace
} // namespace transcript

QVariantMap TranscriptModel::styleTable(QQuickTextDocument* target, const QString& markdown,
                                        const QVariantMap& style, qreal available) const {
    if (target == nullptr || target->textDocument() == nullptr)
        return {};
    using namespace transcript;
    QTextDocument& shown = *target->textDocument();
    const auto fonts = QStringList{style.value(QStringLiteral("sans")).toString(),
                                   style.value(QStringLiteral("mono")).toString(),
                                   style.value(QStringLiteral("size")).toString(),
                                   style.value(QStringLiteral("codeSize")).toString()}
                           .join(QLatin1Char('|'));
    const auto measure_key = fonts + QLatin1Char('\n') + markdown;
    QStringList colors;
    for (auto it = style.cbegin(); it != style.cend(); ++it)
        if (it.value().canConvert<QColor>() && it.value().typeId() == QMetaType::QColor)
            colors << it.key() + QLatin1Char('=') +
                          it.value().value<QColor>().name(QColor::HexArgb);
    const auto shown_key = colors.join(QLatin1Char('|')) + QLatin1Char('\n') + measure_key;
    const auto padding = std::round(cell_size(style) * side_padding) * 2;

    // The columns fit to the room, as constraints for the table's format;
    // `width` is the table's.
    qreal width = 0;
    QVariantList fitted;
    const auto fit = [&](QTextTable& table, const std::vector<TableColumn>& columns) {
        const auto content =
            table_widths(columns, available - (padding + 1) * static_cast<qreal>(columns.size()),
                         rules_for(style));
        QList<QTextLength> constraints;
        fitted.clear();
        width = 0;
        for (const auto w : content) {
            // A pixel over the measure, so a cell that fits is never wrapped by rounding.
            const auto column = w + padding + 1;
            constraints << QTextLength(QTextLength::FixedLength, column);
            fitted << column;
            width += column;
        }
        if (table.document()->property("lapisTableWidths").toList() == fitted)
            return false;
        auto format = table.format();
        format.setColumnWidthConstraints(constraints);
        format.setWidth(QTextLength(QTextLength::FixedLength, width));
        table.setFormat(format);
        table.document()->setProperty("lapisTableWidths", fitted);
        return true;
    };

    if (shown.property("lapisTable").toString() != shown_key) {
        QTextDocument document;
        const auto source = prepared(markdown);
        detail::compose(document, source, style);
        auto* table = first_table(document);
        if (table == nullptr) {
            detail::present(shown, document, style);
            shown.setProperty("lapisTable", shown_key);
            return {{QStringLiteral("width"), 0}};
        }
        QList<bool> numeric;
        dress(*table, source, style, numeric);
        // The first column names its rows when it is short words, not numbers.
        if (table->columns() > 1 && table->rows() > 1 && !numeric[0]) {
            bool short_words = true;
            for (int row = 1; row < table->rows() && short_words; ++row)
                short_words = static_cast<qreal>(cell_text(table->cellAt(row, 0)).size()) <=
                              TableRules{}.short_chars;
            if (short_words)
                emphasise_labels(*table);
        }
        auto kept = table_columns_.constFind(measure_key);
        if (kept == table_columns_.cend()) {
            if (table_columns_.size() >= kept_tables)
                table_columns_.clear();
            kept = table_columns_.insert(measure_key, measure(document, *table));
        }
        // Fit before the copy, and give the view's document the width its
        // TextEdit has (the room, or the table when wider), so the table is
        // laid out once, at its widths.
        fit(*table, *kept);
        shown.setTextWidth(std::max(width, available));
        detail::present(shown, document, style);
        shown.setProperty("lapisTable", shown_key);
        shown.setProperty("lapisTableWidths", fitted);
    }
    auto* table = first_table(shown);
    const auto kept = table_columns_.constFind(measure_key);
    if (table == nullptr || kept == table_columns_.cend())
        return {{QStringLiteral("width"), 0}};
    // A new room: refit in place. A table's format change lays out only
    // its first position again; its cells' lines are broken anew too.
    // (A new text width lays the whole document out again already.)
    if (fit(*table, *kept)) {
        if (const auto text_width = std::max(width, available); shown.textWidth() != text_width)
            shown.setTextWidth(text_width);
        else
            shown.markContentsDirty(table->firstPosition(),
                                    table->lastPosition() - table->firstPosition() + 1);
    }

    // Where each row starts, for the rules drawn behind the text: a row's
    // first cell's text less the padding above it.
    auto* layout = shown.documentLayout();
    (void)layout->documentSize();
    QVariantList rows;
    for (int row = 0; row < table->rows(); ++row) {
        const auto cell = table->cellAt(row, 0);
        const auto top = cell.format().toTableCellFormat().topPadding();
        rows << layout->blockBoundingRect(cell.firstCursorPosition().block()).top() - top;
    }
    rows << layout->frameBoundingRect(table).bottom();
    return {{QStringLiteral("width"), width},
            {QStringLiteral("rows"), rows},
            {QStringLiteral("header"), table->rows() > 1}};
}
} // namespace lapis::desktop
