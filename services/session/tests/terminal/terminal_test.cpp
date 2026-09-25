#include "lapis/session/terminal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using lapis::session::CellKind;
using lapis::session::ColorKind;
using lapis::session::CursorShape;
using lapis::session::Terminal;
using lapis::session::TerminalCell;
using lapis::session::TerminalColor;
using lapis::session::TerminalHistory;
using lapis::session::TerminalLimits;
using lapis::session::TerminalSize;
using lapis::session::TerminalSnapshot;
using lapis::session::TerminalStyle;
using lapis::session::Underline;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const TerminalCell& cell(const TerminalSnapshot& snapshot, std::size_t column, std::size_t row) {
    require(snapshot.cells.size() ==
                static_cast<std::size_t>(snapshot.size.columns) * snapshot.size.rows,
            "snapshot cell count does not match geometry");
    require(column < snapshot.size.columns && row < snapshot.size.rows, "cell outside viewport");
    return snapshot.cells.at(row * snapshot.size.columns + column);
}

void expect_text(const TerminalSnapshot& snapshot, std::size_t row, std::u32string_view expected) {
    for (std::size_t column = 0; column < expected.size(); ++column) {
        require(cell(snapshot, column, row).kind == CellKind::narrow &&
                    snapshot.text(row * snapshot.size.columns + column) ==
                        expected.substr(column, 1),
                "unexpected row text");
    }
}

TerminalColor rgb(std::uint32_t value) { return TerminalColor{ColorKind::rgb, value}; }

TerminalColor indexed(std::uint32_t value) { return TerminalColor{ColorKind::indexed, value}; }

void ascii_cursor() {
    Terminal terminal({12, 4});
    terminal.feed("hello\r\nOK");
    const TerminalSnapshot snapshot = terminal.snapshot();
    expect_text(snapshot, 0, U"hello");
    expect_text(snapshot, 1, U"OK");
    require(snapshot.cursor.column == 2 && snapshot.cursor.row == 1, "incorrect cursor after CRLF");
}

void fragmented_utf8() {
    Terminal terminal({12, 4});
    const std::string bytes = "A\xe7\x95\x8c"
                              "e\xcc\x81\xf0\x9f\x9a\x80";
    for (const char byte : bytes) {
        terminal.feed(std::string_view(&byte, 1));
    }
    const TerminalSnapshot snapshot = terminal.snapshot();
    require(snapshot.text(0) == U"A", "missing ASCII before fragmented UTF-8");
    require(cell(snapshot, 1, 0).kind == CellKind::wide && snapshot.text(1) == U"\u754c",
            "fragmented wide UTF-8 was not retained");
    require(cell(snapshot, 2, 0).kind == CellKind::wide_tail && snapshot.text(2).empty(),
            "wide continuation cell was not represented");
    require(snapshot.text(3) == U"e\u0301", "combining grapheme was not retained");
    require(cell(snapshot, 4, 0).kind == CellKind::wide && snapshot.text(4) == U"\U0001f680" &&
                cell(snapshot, 5, 0).kind == CellKind::wide_tail,
            "fragmented emoji was not retained");
    require(snapshot.cursor.column == 6, "wrong cursor after grapheme cluster advance");
}

void styles_and_colors() {
    Terminal terminal({24, 4});
    terminal.feed("\x1b]4;1;rgb:11/22/33\x1b\\"
                  "\x1b]4;4;rgb:44/55/66\x1b\\"
                  "\x1b]4;9;rgb:a0/a0/a0\x1b\\"
                  "\x1b]4;12;rgb:b0/b0/b0\x1b\\"
                  "\x1b]10;rgb:c0/c0/c0\x1b\\"
                  "\x1b]11;rgb:d0/d0/d0\x1b\\");
    terminal.feed("\x1b[1;3;2;5;8;9;53;4:3;58:2:17:34:51;38;2:17:34:51;48:2:68:85:102mX"
                  "\x1b[0;38:5:1;48:5:4mN"
                  "\x1b[1;38:5:1;48:5:4mB"
                  "\x1b[1;38:5:9;48:5:12mE"
                  "\x1b[0;7;38:5:1;48:5:4mI"
                  "\x1b[1;7;38:5:1;48:5:4mJ"
                  "\x1b[0mR");
    const TerminalSnapshot snapshot = terminal.snapshot();
    require(snapshot.palette[1] == 0x112233 && snapshot.palette[4] == 0x445566 &&
                snapshot.palette[9] == 0xa0a0a0 && snapshot.palette[12] == 0xb0b0b0,
            "dynamic palette was not exposed");
    const TerminalCell& full = cell(snapshot, 0, 0);
    const TerminalStyle expected_full{rgb(0x112233), rgb(0x445566), rgb(0x112233), Underline::curly,
                                      true,          true,          true,          true,
                                      false,         true,          true,          true};
    require(snapshot.text(0) == U"X" && full.style == expected_full,
            "full style flags or underline color mismatch");
    require(snapshot.cell_foreground(0) == 0x112233 && snapshot.cell_background(0) == 0x445566,
            "truecolor resolution mismatch");
    require(cell(snapshot, 1, 0).style.foreground == indexed(1) &&
                cell(snapshot, 1, 0).style.background == indexed(4),
            "indexed style mismatch");
    require(snapshot.cell_foreground(1) == 0x112233 && snapshot.cell_background(1) == 0x445566,
            "normal indexed resolution mismatch");
    require(cell(snapshot, 2, 0).style.bold && snapshot.cell_foreground(2) == 0x112233,
            "bold brightened an indexed foreground");
    require(snapshot.cell_foreground(3) == 0xa0a0a0 && snapshot.cell_background(3) == 0xb0b0b0,
            "explicit bright indexed resolution mismatch");
    require(snapshot.cell_foreground(4) == 0x445566 && snapshot.cell_background(4) == 0x112233,
            "inverse indexed resolution mismatch");
    require(cell(snapshot, 5, 0).style.bold && snapshot.cell_foreground(5) == 0x445566,
            "inverse changed semantics with bold");
    require(snapshot.text(6) == U"R" &&
                cell(snapshot, 6, 0).style.foreground.kind == ColorKind::default_color &&
                cell(snapshot, 6, 0).style.background.kind == ColorKind::default_color &&
                snapshot.cell_foreground(6) == 0xc0c0c0 && snapshot.cell_background(6) == 0xd0d0d0,
            "default colors did not resolve or reset");
}

void alternate_screen() {
    Terminal terminal({12, 4});
    terminal.feed("main");
    terminal.feed("\x1b[?1049h\x1b[Halt");
    const TerminalSnapshot alternate = terminal.snapshot();
    require(alternate.alternate_screen, "alternate screen mode was not exposed");
    expect_text(alternate, 0, U"alt");
    terminal.feed("\x1b[?1049l");
    const TerminalSnapshot restored = terminal.snapshot();
    require(!restored.alternate_screen, "primary screen was not restored");
    expect_text(restored, 0, U"main");
    require(restored.cursor.column == 4 && restored.cursor.row == 0,
            "alternate-screen exit did not restore cursor");
}

void resize_and_wrap_spacer() {
    Terminal terminal({5, 4});
    terminal.feed("abcdef");
    const TerminalSnapshot before = terminal.snapshot();
    expect_text(before, 0, U"abcde");
    expect_text(before, 1, U"f");
    require(before.cursor.row == 1 && before.cursor.column == 1, "autowrap cursor mismatch");
    terminal.resize({3, 4});
    const TerminalSnapshot after = terminal.snapshot();
    require(after.size.columns == 3 && after.size.rows == 4, "resize geometry mismatch");
    expect_text(after, 0, U"abc");
    expect_text(after, 1, U"def");

    Terminal wrapped({3, 4});
    wrapped.feed("\x1b[1mAB\xe7\x95\x8c");
    const TerminalSnapshot split = wrapped.snapshot();
    require(cell(split, 0, 0).kind == CellKind::narrow &&
                cell(split, 2, 0).kind == CellKind::wrap_spacer &&
                cell(split, 0, 1).kind == CellKind::wide &&
                cell(split, 1, 1).kind == CellKind::wide_tail,
            "wide character did not wrap with a spacer");
    require(split.cursor.row == 1 && split.cursor.column == 2 && !split.cursor.wide_tail,
            "wide-wrap cursor mismatch");
}

void input_modes() {
    Terminal terminal({12, 4});
    require(terminal.encode_key(lapis::session::TerminalKey::up) == "\x1b[A",
            "normal cursor key encoding mismatch");
    require(terminal.encode_paste("echo hi") == "echo hi", "unbracketed paste encoding mismatch");
    require(terminal.encode_paste("a\nb\x1b") == "a\rb ", "plain paste sanitization mismatch");
    require(terminal.encode_key(lapis::session::TerminalKey::up, {.control = true}) == "\x1b[1;5A",
            "modified navigation encoding mismatch");
    require(terminal.encode_key(lapis::session::TerminalKey::up) == "\x1b[A",
            "key modifiers leaked into the next event");
    terminal.feed("\x1b[?1h\x1b[?2004h");
    require(terminal.encode_key(lapis::session::TerminalKey::up) == "\x1bOA",
            "application cursor key encoding mismatch");
    require(terminal.encode_paste("a\nb\x1b") == "\x1b[200~a\nb \x1b[201~",
            "bracketed paste did not preserve newline and sanitize escape");
    const TerminalSnapshot snapshot = terminal.snapshot();
    require(snapshot.application_cursor_keys && snapshot.bracketed_paste,
            "input modes were not exposed");
    require(terminal.encode_paste("echo hi") == "\x1b[200~echo hi\x1b[201~",
            "bracketed paste encoding mismatch");
    terminal.feed("\x1b[?1l\x1b[?2004l");
    require(terminal.encode_key(lapis::session::TerminalKey::up) == "\x1b[A" &&
                terminal.encode_paste("echo hi") == "echo hi",
            "input modes did not reset");
}

void snapshots_survive_changes() {
    TerminalSnapshot retained{};
    {
        Terminal terminal({12, 4});
        terminal.feed("owned");
        retained = terminal.snapshot();
        terminal.feed("\x1b[2J\x1b[Hnew");
        terminal.resize({4, 2});
        static_cast<void>(terminal.snapshot());
    }
    expect_text(retained, 0, U"owned");
    require(retained.size.columns == 12 && retained.cursor.column == 5 &&
                retained.history.total_rows == 4,
            "retained snapshot changed after terminal destruction or mutation");
}

void cursor_visibility_and_shape() {
    Terminal terminal({12, 4});
    terminal.feed("\x1b[?25l\x1b[6 q");
    TerminalSnapshot hidden = terminal.snapshot();
    require(!hidden.cursor.visible && hidden.cursor.shape == CursorShape::bar,
            "cursor visibility or shape mismatch");
    terminal.feed("\x1b[?25h\x1b[3 q");
    hidden = terminal.snapshot();
    require(hidden.cursor.visible && hidden.cursor.shape == CursorShape::underline,
            "cursor visibility or shape did not update");
}

void dsr_reply_order() {
    Terminal terminal({80, 24});
    terminal.feed("\x1b[6n");
    terminal.resize({81, 23});
    terminal.feed("\x1b[3;7H\x1b[6n");
    require(terminal.take_replies() == "\x1b[1;1R\x1b[3;7R",
            "DSR replies were not exact or ordered");
    require(terminal.take_replies().empty(), "take_replies did not drain replies");
    terminal.feed("\x1b[6n");
    require(terminal.take_replies() == "\x1b[3;7R", "reply buffer was not reusable after drain");
}

void output_burst() {
    Terminal terminal({12, 4});
    for (int line = 0; line < 1000; ++line) {
        terminal.feed("burst\r\n");
    }
    terminal.feed("tail");
    const TerminalSnapshot snapshot = terminal.snapshot();
    expect_text(snapshot, 3, U"tail");
    require(snapshot.cells.size() == 48, "viewport grew with output history");
}

void history_eviction_and_clear() {
    TerminalLimits limits{};
    limits.history_bytes = 4096;
    Terminal terminal({12, 4}, limits);
    for (int line = 0; line < 5000; ++line) {
        terminal.feed("0123456789ab\r\n");
    }
    require(terminal.snapshot().history.total_rows > 4, "recent history was not retained");
    for (int line = 0; line < 5000; ++line) {
        terminal.feed("0123456789ab\r\n");
    }
    terminal.feed("tail");
    const TerminalSnapshot evicted = terminal.snapshot();
    require(evicted.history.total_rows > 4 && evicted.history.total_rows < 10001,
            "history did not retain recent rows while evicting old rows");
    require(evicted.history.viewport_offset + evicted.history.viewport_rows ==
                evicted.history.total_rows,
            "viewport offset does not follow output");
    require(evicted.history.viewport_rows == 4, "history viewport row count mismatch");
    terminal.clear_history();
    const TerminalSnapshot cleared = terminal.snapshot();
    require(cleared.history.total_rows == 4 && cleared.history.viewport_rows == 4,
            "clear_history did not retain the current viewport");
    expect_text(cleared, 3, U"tail");
    terminal.feed("\r\nmore");
    require(terminal.snapshot().history.total_rows > 4, "history budget was not restored");
}

std::u32string visible_text(const TerminalSnapshot& snapshot) {
    std::u32string text;
    for (std::size_t index = 0; index < snapshot.cells.size(); ++index)
        text += snapshot.text(index);
    return text;
}
void history_snapshot_extraction() {
    Terminal terminal({20, 4});
    for (int line = 0; line < 1000; ++line) {
        auto number = std::to_string(line);
        terminal.feed("line" + std::string(4 - number.size(), '0') + number + "\r\n");
    }
    terminal.feed("\x1b[5n");
    const auto live = terminal.snapshot();
    const auto metadata = terminal.history_metadata();
    require(metadata.total_rows == 1001 && metadata.primary_available, "history row count");
    for (std::size_t offset : {0U, 17U, 500U, 996U}) {
        const auto page = terminal.history_snapshot(offset);
        for (std::size_t row = 0; row < page.size.rows; ++row) {
            const auto number = std::to_string(offset + row);
            const auto expected = "line" + std::string(4 - number.size(), '0') + number;
            const std::u32string unicode(expected.begin(), expected.end());
            expect_text(page, row, unicode);
        }
        const auto restored = terminal.snapshot();
        require(visible_text(restored) == visible_text(live) &&
                    restored.cursor.row == live.cursor.row &&
                    restored.cursor.column == live.cursor.column &&
                    restored.revision == live.revision &&
                    restored.history.viewport_offset == live.history.viewport_offset,
                "history extraction changed live screen or cursor");
    }
    require(terminal.take_replies() == "\x1b[0n", "history extraction changed queued replies");
    bool rejected{};
    try {
        static_cast<void>(terminal.history_snapshot(metadata.total_rows));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "out of range history accepted");
    terminal.clear_history();
    require(visible_text(terminal.snapshot()) == visible_text(live), "drain changed live screen");
    require(terminal.history_metadata().total_rows == 4, "drain retained archived rows");
    terminal.feed("\r\nmore");
    require(terminal.history_metadata().total_rows == 5, "drain did not restore history budget");
}
void history_snapshot_resize_and_alternate() {
    Terminal terminal({20, 4});
    terminal.feed("\x1b[1;3;38;2;18;52;86m界é\x1b[0m\r\n");
    terminal.feed("1234567890\r\nnext\r\nend\r\n");
    const auto styled = terminal.history_snapshot(0);
    require(styled.cells.front().kind == CellKind::wide &&
                styled.cells[1].kind == CellKind::wide_tail && styled.text(0) == U"界" &&
                styled.text(2) == U"é",
            "archived Unicode changed");
    require(styled.cells.front().style.bold && styled.cells.front().style.italic &&
                styled.cells.front().style.foreground == rgb(0x123456),
            "archived style changed");
    terminal.resize({5, 3});
    const auto live = terminal.snapshot();
    const auto count = terminal.history_metadata().total_rows;
    std::u32string all;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto start = std::min(offset, count - live.size.rows);
        const auto page = terminal.history_snapshot(start);
        const auto row = offset - start;
        for (std::size_t column = 0; column < page.size.columns; ++column)
            all += page.text(row * page.size.columns + column);
    }
    require(all.find(U"1234567890") != std::u32string::npos, "reflow lost ordered text");
    require(visible_text(terminal.snapshot()) == visible_text(live),
            "reflow browsing changed live screen");
    terminal.feed("\x1b[?1049halt");
    require(!terminal.history_metadata().primary_available, "alternate history marked primary");
    bool rejected{};
    try {
        static_cast<void>(terminal.history_snapshot(0));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "alternate history browsing accepted");
    terminal.feed("\x1b[?1049l");
    require(visible_text(terminal.snapshot()) == visible_text(live),
            "alternate roundtrip changed primary");
}

void invalid_geometry_and_rejected_mutations() {
    const auto expect_invalid = [](TerminalSize size) {
        try {
            Terminal terminal(size);
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("invalid geometry was accepted");
    };
    expect_invalid({0, 4});
    expect_invalid({12, 0});
    expect_invalid({65535, 65535});

    TerminalLimits limits{};
    limits.max_input_bytes = 4;
    Terminal terminal({12, 4}, limits);
    terminal.feed("kept");
    const std::string oversized(5, 'x');
    const auto expect_length = [&](auto operation, std::string_view message) {
        try {
            operation();
        } catch (const std::length_error&) {
            return;
        }
        throw std::runtime_error(std::string(message));
    };
    expect_length([&] { terminal.feed(oversized); }, "oversized feed was accepted");
    expect_length(
        [&] {
            const std::string encoded = terminal.encode_paste(oversized);
            static_cast<void>(encoded);
        },
        "oversized paste was accepted");
    bool resize_rejected = false;
    try {
        terminal.resize({0, 4});
    } catch (const std::invalid_argument&) {
        resize_rejected = true;
    }
    require(resize_rejected, "invalid resize was accepted");
    const TerminalSnapshot snapshot = terminal.snapshot();
    expect_text(snapshot, 0, U"kept");
    require(snapshot.size == TerminalSize{12, 4}, "rejected operation changed geometry");

    terminal.feed("\rOK");
    const TerminalSnapshot recovered = terminal.snapshot();
    expect_text(recovered, 0, U"OKpt");
    require(recovered.size == TerminalSize{12, 4}, "rejected input mutated terminal state");
}

void reply_overflow_faults_terminal() {
    TerminalLimits limits{};
    limits.max_reply_bytes = 5;
    Terminal terminal({80, 24}, limits);
    terminal.feed("safe");
    bool overflowed = false;
    try {
        terminal.feed("\x1b[6n");
    } catch (const std::overflow_error&) {
        overflowed = true;
    }
    require(overflowed, "reply budget overflow threw the wrong type");
    bool permanently_faulted = false;
    try {
        static_cast<void>(terminal.snapshot());
    } catch (const std::runtime_error&) {
        permanently_faulted = true;
    }
    require(permanently_faulted, "terminal remained usable after reply overflow");
    try {
        static_cast<void>(terminal.take_replies());
    } catch (const std::overflow_error&) {
        return;
    }
    throw std::runtime_error("faulted terminal returned a partial reply");
}

void grapheme_cap() {
    TerminalLimits limits{};
    limits.max_grapheme_codepoints = 2;
    Terminal terminal({12, 4}, limits);
    terminal.feed("A\xcc\x81\xcc\x82");
    bool rejected = false;
    try {
        static_cast<void>(terminal.snapshot());
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected, "snapshot grapheme codepoint cap was not enforced");
    terminal.feed("\r\x1b[2KZ");
    require(terminal.snapshot().text(0) == U"Z", "snapshot extraction failure prevented recovery");
}

void erased_backgrounds() {
    Terminal terminal({16, 4});
    // TUIs paint input panels by erasing with a background, then drawing text.
    terminal.feed("\x1b[48;2;47;50;57m\x1b[2K> text\x1b[0m");
    terminal.feed("\x1b[2;1H\x1b[48;5;4m\x1b[16X\x1b[0m");
    const auto snapshot = terminal.snapshot();
    for (std::size_t column = 0; column < 16; ++column) {
        require(snapshot.cell_background(column) == 0x2f3239U,
                "erase-line background must extend beyond printed text");
        require(cell(snapshot, column, 1).style.background == TerminalColor{ColorKind::indexed, 4},
                "erase-character background must retain its palette index");
    }
    terminal.feed("\x1b]4;4;rgb:12/34/56\x1b\\");
    const auto recolored = terminal.snapshot();
    require(recolored.cell_background(16) == 0x123456U,
            "erased indexed backgrounds follow palette changes");
    terminal.feed("\x1b[1;1H\x1b[2K");
    const auto cleared = terminal.snapshot();
    require(cleared.cell_background(0) == cleared.background_rgb,
            "default erase clears the explicit background");
}

struct Case {
    std::string_view name;
    void (*run)(){};
};

constexpr std::array cases{
    Case{"ascii_cursor", ascii_cursor},
    Case{"fragmented_utf8", fragmented_utf8},
    Case{"styles_and_colors", styles_and_colors},
    Case{"erased_backgrounds", erased_backgrounds},
    Case{"alternate_screen", alternate_screen},
    Case{"resize_and_wrap_spacer", resize_and_wrap_spacer},
    Case{"input_modes", input_modes},
    Case{"snapshot_durability", snapshots_survive_changes},
    Case{"cursor_visibility_and_shape", cursor_visibility_and_shape},
    Case{"dsr_reply_order", dsr_reply_order},
    Case{"output_burst", output_burst},
    Case{"history_eviction_and_clear", history_eviction_and_clear},
    Case{"history_snapshot_extraction", history_snapshot_extraction},
    Case{"history_snapshot_resize_and_alternate", history_snapshot_resize_and_alternate},
    Case{"invalid_geometry_and_rejected_mutations", invalid_geometry_and_rejected_mutations},
    Case{"reply_overflow_faults_terminal", reply_overflow_faults_terminal},
    Case{"grapheme_cap", grapheme_cap},
};

} // namespace

int main() {
    bool passed = true;
    for (const Case& test_case : cases) {
        bool case_passed = true;
        try {
            test_case.run();
        } catch (const std::exception& error) {
            std::cerr << test_case.name << ": " << error.what() << '\n';
            case_passed = false;
        } catch (...) {
            std::cerr << test_case.name << ": unknown exception\n";
            case_passed = false;
        }
        std::cout << test_case.name << ": " << (case_passed ? "PASS" : "FAIL") << '\n';
        passed = passed && case_passed;
    }
    std::cout << (passed ? "terminal tests passed\n" : "terminal tests failed\n");
    return passed ? 0 : 1;
}
