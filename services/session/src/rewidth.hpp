#ifndef LAPIS_SESSION_REWIDTH_HPP
#define LAPIS_SESSION_REWIDTH_HPP
// Claude Code's classic renderer answers a width change by erasing the screen
// and drawing the end of its conversation again at the new width (more than a
// screen of it, so the top scrolls into history); it never reprints what had
// scrolled away before. That redraw starts at a different point of the
// conversation than the screen it replaces: earlier when the terminal widened
// (those rows are already in history), later when it narrowed (the rows in
// between would be lost). These helpers line the old screen and the redraw up
// by their text, ignoring the whitespace that wrapping and indentation change.
#include <lapis/session/terminal.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace lapis::session::rewidth {

// Rows' visible characters without whitespace, where each row starts in that
// text, and the first row with prose in it (not a rule or a blank).
struct Text {
    std::u32string text;
    std::vector<std::size_t> row_starts;
    std::optional<std::size_t> first_words;
};

// A compact, dependency-free classifier for prose anchors. It covers letters,
// marks, and numerals in the Unicode script blocks terminals commonly render,
// while excluding decoration such as box drawing and emoji. This deliberately
// is not an identifier classifier: a few script-block punctuation characters
// can count, but eight of them are still far more likely to be prose than
// Claude Code's rules or prompt chrome.
struct CodePointRange {
    char32_t first;
    char32_t last;
};

inline constexpr CodePointRange word_ranges[] = {
    {U'0', U'9'},       {U'A', U'Z'},       {U'a', U'z'},       {0x00C0, 0x052F},
    {0x0530, 0x05FF},   {0x0600, 0x08FF},   {0x0900, 0x0DFF},   {0x0E00, 0x10FF},
    {0x1100, 0x11FF},   {0x1200, 0x18AF},   {0x1E00, 0x1FFF},   {0x2070, 0x218F},
    {0x2C00, 0x2DDF},   {0x3005, 0x30FF},   {0x3100, 0x31FF},   {0x3220, 0x325F},
    {0x3280, 0x32BF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xD7FF},
    {0xF900, 0xFDFF},   {0xFE20, 0xFE2F},   {0xFE70, 0xFEFC},   {0xFF10, 0xFF19},
    {0xFF21, 0xFF3A},   {0xFF41, 0xFF5A},   {0xFF66, 0xFFDC},   {0x10000, 0x18AFF},
    {0x1B000, 0x1B2FF}, {0x1BC00, 0x1BC9F}, {0x1E100, 0x1E14F}, {0x1E800, 0x1E8D7},
    {0x1F200, 0x1F2FF}, {0x20000, 0x2FA1F}, {0x30000, 0x323AF},
};

inline bool is_word_char(char32_t c) {
    return std::any_of(std::begin(word_ranges), std::end(word_ranges),
                       [&](auto range) { return c >= range.first && c <= range.last; });
}

// Adds rows [first, first + count) of `page`.
inline void append_rows(Text& result, const TerminalSnapshot& page, std::size_t first,
                        std::size_t count) {
    // A row with this many letters or digits is prose, not decoration.
    constexpr std::size_t words_row = 8;
    for (std::size_t row = first; row < first + count && row < page.size.rows; ++row) {
        result.row_starts.push_back(result.text.size());
        std::size_t word_chars = 0;
        for (std::size_t column = 0; column < page.size.columns; ++column)
            for (const char32_t c : page.text(row * page.size.columns + column))
                if (c != U' ' && c != U'\t' && c != U' ') {
                    result.text.push_back(c);
                    word_chars += is_word_char(c) ? 1 : 0;
                }
        if (!result.first_words && word_chars >= words_row)
            result.first_words = result.row_starts.size() - 1;
    }
}

inline Text screen_text(const TerminalSnapshot& screen) {
    Text result;
    append_rows(result, screen, 0, screen.size.rows);
    return result;
}

// Characters the probe needs: enough that a match is the same passage.
inline constexpr std::size_t probe_chars = 48;
inline constexpr std::size_t min_probe_chars = 24;

// Where `from`'s first words appear in `in`: the row of `in` holding the
// match, whether the match begins that row, and the row of `from` the probe
// starts on. Empty when `from` shows too little text or the passage is not
// there.
struct Place {
    std::size_t row{};
    bool row_start{};
    std::size_t from_row{};
};
inline std::size_t row_holding(const Text& text, std::size_t at) {
    std::size_t row = 0;
    while (row + 1 < text.row_starts.size() && text.row_starts[row + 1] <= at)
        ++row;
    return row;
}

inline std::optional<Place> locate(const Text& from, const Text& in) {
    if (!from.first_words)
        return std::nullopt;
    const auto haystack = std::u32string_view(in.text);
    // A retried command or repeated tool output can put an identical passage
    // earlier in the larger text. The redraw continues the newest copy, so use
    // its last occurrence. Try later source rows when the first prose row is
    // transient chrome that the other text does not contain.
    for (std::size_t row = *from.first_words; row < from.row_starts.size(); ++row) {
        const auto start = from.row_starts[row];
        if (from.text.size() - start < min_probe_chars)
            continue;
        const auto probe = std::u32string_view(from.text).substr(start, probe_chars);
        const auto at = haystack.rfind(probe);
        if (at == std::u32string_view::npos)
            continue;
        const auto found_row = row_holding(in, at);
        return Place{found_row, in.row_starts[found_row] == at, row};
    }
    return std::nullopt;
}

// Widening keeps the wider redraw and drops the older copy instead: the rows
// of `history` (the newest kept rows, before the redraw) that come before the
// redraw's first words. Empty when the redraw does not begin inside them. A
// row holding the start of a line the redraw continues stays.
inline std::optional<std::size_t> history_kept(const Text& history, const Text& redraw) {
    const auto place = locate(redraw, history);
    if (!place)
        return std::nullopt;
    // The redraw's leading rows are screen chrome, not scrollback rows. Keep
    // every history row before the matched passage.
    return place->row_start ? place->row : place->row + 1;
}

// What to do once the redraw is in.
struct Plan {
    // The redraw window already kept in history. When drop_at is non-zero the
    // rows above it are shared screen chrome and must remain on the screen.
    std::size_t drop_at{};
    std::size_t drop_new_rows{};
    // The old-screen window the redraw no longer shows. The rows above keep_at
    // are shared chrome and are already represented by the redraw.
    std::size_t keep_at{};
    std::size_t keep_old_rows{};
};
// Empty when the two do not line up at all.
inline std::optional<Plan> plan(const Text& old_screen, const Text& redraw) {
    // Wider: the redraw begins earlier, so the old screen's first words sit
    // further down it. Rows above them, less the old screen's own rows above
    // its first words, are already in history. A partial row stays;
    // repeating part of a line is better than losing it.
    if (const auto place = locate(old_screen, redraw))
        if (place->row >= place->from_row)
            return Plan{.drop_at = place->from_row, .drop_new_rows = place->row - place->from_row};
    // Narrower: the redraw begins inside the old screen.
    if (const auto place = locate(redraw, old_screen)) {
        const auto rows = place->row >= place->from_row ? place->row - place->from_row : 0;
        return Plan{.keep_at = place->from_row,
                    .keep_old_rows = place->row_start || rows == 0 ? rows : rows + 1};
    }
    return std::nullopt;
}

// The old screen's rows above Claude Code's prompt box, or through its last
// text when no recognizable box shows: what to keep when the redraw cannot be
// lined up with it.
inline bool is_rule_char(char32_t c) {
    return c == U'-' || c == U'=' || c == U'_' || (c >= 0x2500 && c <= 0x257F);
}

inline bool is_strong_horizontal_char(char32_t c) {
    return c == U'-' || c == U'=' || c == U'─' || c == U'━' || c == U'═';
}

inline bool is_prompt_mark(char32_t c) {
    return c == U'>' || c == U'$' || c == U'%' || c == U'#' || c == U'❯' || c == U'➯' || c == U'›';
}

struct RowShape {
    char32_t first{};
    std::size_t visible{};
    std::size_t rule_chars{};
    std::size_t horizontal{};
};

inline RowShape row_shape(const TerminalSnapshot& screen, std::size_t row) {
    RowShape shape;
    for (std::size_t column = 0; column < screen.size.columns; ++column)
        for (const char32_t cell : screen.text(row * screen.size.columns + column)) {
            if (shape.first == U'\0' && cell != U' ')
                shape.first = cell;
            if (cell != U' ' && cell != U'\t' && cell != U' ') {
                ++shape.visible;
                shape.rule_chars += is_rule_char(cell) ? 1 : 0;
                shape.horizontal += is_strong_horizontal_char(cell) ? 1 : 0;
            }
        }
    return shape;
}

inline bool is_rule_row(const RowShape& shape) {
    return shape.visible >= 4 && shape.rule_chars == shape.visible &&
           shape.horizontal * 2 >= shape.visible;
}

inline std::size_t conversation_rows(const TerminalSnapshot& screen) {
    std::size_t last_text = 0;
    std::optional<std::size_t> box;
    std::optional<std::size_t> rule;
    for (std::size_t row = 0; row < screen.size.rows; ++row) {
        const auto shape = row_shape(screen, row);
        bool prompt_after_rule = false;
        if (shape.first != U'\0')
            last_text = row + 1;
        if (rule && is_prompt_mark(shape.first)) {
            box = *rule;
            prompt_after_rule = true;
        }
        if (is_rule_row(shape))
            rule = row;
        else if (shape.visible != 0)
            rule.reset();
        if (!prompt_after_rule && shape.visible == 1 && is_prompt_mark(shape.first))
            box = row;
    }
    return box.value_or(last_text);
}

} // namespace lapis::session::rewidth

#endif
