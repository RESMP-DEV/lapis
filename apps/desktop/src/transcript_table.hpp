#ifndef LAPIS_DESKTOP_TRANSCRIPT_TABLE_HPP
#define LAPIS_DESKTOP_TRANSCRIPT_TABLE_HPP
// A reply's markdown table, laid out for reading: columns sized from their
// content, cells in a tighter type than the prose, and the room it takes
// allowed past the reading column up to the thread's width.
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <Qt>
#include <vector>

class QTextDocument;

namespace lapis::desktop::transcript {

// One column as measured, in pixels of the cell font, without padding.
struct TableColumn {
    qreal natural{}; // the widest body cell unwrapped
    qreal word{};    // the widest run no line can break inside
    qreal header{};  // the header cell unwrapped
    qreal header_word{};
    qreal weight{}; // all its body text side by side: its claim on spare room
};

// The fitting rules, in characters of the cell font (`ch` pixels each).
struct TableRules {
    qreal ch{8};
    // A column whose cells all fit in this many characters is short: it
    // keeps its natural width and never wraps. A little over the minimum,
    // as wrapping a column to save a few characters only costs lines.
    qreal short_chars{24};
    // A column that wraps is never narrower than this...
    qreal min_chars{20};
    // ...or wider than this, so its lines stay readable.
    qreal max_chars{64};
    // A longer unbreakable run (a path, a URL) breaks rather than widening
    // its column past this.
    qreal word_chars{32};
};

// Content widths for `columns` in `available` pixels (padding already
// taken out). Short columns get their natural width; the rest start at
// their minimum and share what is left in proportion to their text, each
// up to its natural width and the maximum. The sum can be less than
// `available` (the table does not need it) or more (it scrolls: no column
// goes below its minimum).
[[nodiscard]] std::vector<qreal> table_widths(const std::vector<TableColumn>& columns,
                                              qreal available, const TableRules& rules);

// True when every non-empty body cell is a number, percent or amount
// ("1,702.9", "-3%", "$2.10", "18.2 GB") and at least one is.
[[nodiscard]] bool numeric_column(const QStringList& cells);

// A markdown table's column alignments from its delimiter row ("|:--|--:|"):
// Qt::AlignLeft, AlignRight or AlignHCenter where a colon asks, 0 where none.
[[nodiscard]] QList<int> table_alignments(const QString& markdown);

// True when `line` is a table's delimiter row: cells of dashes with
// optional colons, at least one pipe.
[[nodiscard]] bool table_delimiter(const QString& line);

// A table row's cells, trimmed, split at pipes outside code spans and not
// escaped ("\|"); the outer pipes are optional.
[[nodiscard]] QStringList table_cells(const QString& row);

// True when `header` starts a table whose delimiter row is `delimiter`: a
// row with pipes, not in a list, quote or indented code, with as many
// cells as the delimiter row.
[[nodiscard]] bool table_header(const QString& header, const QString& delimiter);

namespace detail {
// The markdown imported into `document` with the view's fonts and colors
// (headings, code, links, quotes), as the thread shows prose.
void compose(QTextDocument& document, const QString& markdown, const QVariantMap& style);
// `document` copied into the view's `shown` in one edit.
void present(QTextDocument& shown, const QTextDocument& document, const QVariantMap& style);
} // namespace detail
} // namespace lapis::desktop::transcript
#endif
