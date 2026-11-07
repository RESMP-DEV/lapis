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

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lapis::session::rewidth {

// Rows' visible characters without whitespace, where each row starts in that
// text, and the first row with words in it (not a rule or a blank).
struct Text {
    std::u32string text;
    std::vector<std::size_t> row_starts;
    std::optional<std::size_t> first_words;
};

inline bool is_word_char(char32_t c) {
    return (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z');
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
inline std::optional<Place> locate(const Text& from, const Text& in) {
    if (!from.first_words)
        return std::nullopt;
    const auto start = from.row_starts[*from.first_words];
    if (from.text.size() - start < min_probe_chars)
        return std::nullopt;
    const auto probe = std::u32string_view(from.text).substr(start, probe_chars);
    const auto at = std::u32string_view(in.text).find(probe);
    if (at == std::u32string_view::npos)
        return std::nullopt;
    std::size_t row = 0;
    while (row + 1 < in.row_starts.size() && in.row_starts[row + 1] <= at)
        ++row;
    return Place{row, in.row_starts[row] == at, *from.first_words};
}

// Widening keeps the wider redraw and drops the older copy instead: the rows
// of `history` (the newest kept rows, before the redraw) that come before the
// redraw's first words. Empty when the redraw does not begin inside them. A
// row holding the start of a line the redraw continues stays.
inline std::optional<std::size_t> history_kept(const Text& history, const Text& redraw) {
    const auto place = locate(redraw, history);
    if (!place)
        return std::nullopt;
    const auto rows = place->row >= place->from_row ? place->row - place->from_row : 0;
    return place->row_start ? rows : rows + 1;
}

// What to do once the redraw is in.
struct Plan {
    // Rows at the start of the redraw already kept in history: drop them.
    std::size_t drop_new_rows{};
    // Rows at the top of the old screen the redraw no longer shows: keep them
    // in history, ahead of the redraw.
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
            return Plan{.drop_new_rows = place->row - place->from_row};
    // Narrower: the redraw begins inside the old screen.
    if (const auto place = locate(redraw, old_screen)) {
        const auto rows = place->row >= place->from_row ? place->row - place->from_row : 0;
        return Plan{.keep_old_rows = place->row_start || rows == 0 ? rows : rows + 1};
    }
    return std::nullopt;
}

// The old screen's rows above Claude Code's prompt box (a rule, then a row
// starting with ❯), or through its last text when no box shows: what to keep
// when the redraw cannot be lined up with it.
inline std::size_t conversation_rows(const TerminalSnapshot& screen) {
    const auto first_char = [&](std::size_t row) -> char32_t {
        for (std::size_t column = 0; column < screen.size.columns; ++column)
            for (const char32_t c : screen.text(row * screen.size.columns + column))
                if (c != U' ')
                    return c;
        return U'\0';
    };
    std::size_t last_text = 0;
    std::optional<std::size_t> box;
    for (std::size_t row = 0; row < screen.size.rows; ++row) {
        const auto c = first_char(row);
        if (c != U'\0')
            last_text = row + 1;
        if (c == U'❯' && row > 0 && first_char(row - 1) == U'─')
            box = row - 1;
    }
    return box.value_or(last_text);
}

} // namespace lapis::session::rewidth

#endif
