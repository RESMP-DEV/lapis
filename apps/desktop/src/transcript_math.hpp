#ifndef LAPIS_DESKTOP_TRANSCRIPT_MATH_HPP
#define LAPIS_DESKTOP_TRANSCRIPT_MATH_HPP

// Math in replies: the LaTeX agents write in prose ($x_i$, $$...$$, \(..\),
// \[..\]) found before the markdown import, so markdown never reads it, and
// set as readable text with real super- and subscripts. What cannot be set
// well (matrices, aligned equations, deep fractions, unknown commands with
// arguments) shows as its source in the inline code style instead.

#include <QString>
#include <QTextCharFormat>
#include <cstdint>
#include <vector>

class QTextDocument;

namespace lapis::desktop::transcript::math {

// One math span in markdown: [start, start + length) covers the delimiters,
// `source` is what is between them and `written` the span as written.
struct Span {
    qsizetype start{};
    qsizetype length{};
    bool display{};
    QString source;
    QString written;
};

// Whether the text could hold math at all: a "$", "\(" or "\[". The rest of
// the pass is skipped when it cannot.
[[nodiscard]] bool may_contain(const QString& markdown);

// The math spans in markdown prose, in order. Never inside code spans,
// fenced code, link destinations or autolinks. $$..$$ and \[..\] are display
// math; $..$ and \(..\) inline. Single dollars follow pandoc's rule (the
// opening $ is followed by a non-space, the closing one preceded by a
// non-space and not followed by a digit, \$ is a literal dollar, no blank
// line inside) and also skip an opening $ right after a letter or digit
// ("US$5"), ${VAR}, $VAR/... and $VAR: shell text, URLs, and money that runs
// into words ("$5 for the cables ... $").
[[nodiscard]] std::vector<Span> find(const QString& markdown);

// A piece of rendered math.
struct Run {
    enum class Shift : std::uint8_t { none, super, sub };
    QString text;
    Shift shift{Shift::none};
    bool italic{};
    bool bold{};
};
// LaTeX set as runs, or ok false when it should show as source.
struct Rendered {
    bool ok{};
    std::vector<Run> runs;
};
[[nodiscard]] Rendered render(const QString& latex);
// The runs' text, with "^" and "_" before shifted runs (for tests).
[[nodiscard]] QString plain(const Rendered& rendered);

// Character format properties the math in a styled document carries: the
// span's number (to tell adjacent spans apart) and its source with its
// delimiters, which copying a whole span gives back.
inline constexpr int span_property = QTextFormat::UserProperty + 41;
inline constexpr int source_property = QTextFormat::UserProperty + 42;
// Block property marking a display equation's own paragraph.
inline constexpr int display_property = QTextFormat::UserProperty + 43;

// Markdown with each span replaced by one private-use placeholder character,
// for QTextDocument::setMarkdown. Empty spans when there is no math or the
// text already uses those characters.
struct Prepared {
    QString markdown;
    std::vector<Span> spans;
};
[[nodiscard]] Prepared prepare(const QString& markdown);

// Replaces the placeholders setMarkdown kept with the rendered math (or its
// source in a fixed-pitch format the view's code styling picks up). Display
// spans get a centred paragraph of their own marked with display_property,
// except inside lists and tables, where they stay in line.
void fill(QTextDocument& document, const std::vector<Span>& spans);

} // namespace lapis::desktop::transcript::math

#endif // LAPIS_DESKTOP_TRANSCRIPT_MATH_HPP
