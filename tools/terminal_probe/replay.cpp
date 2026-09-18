#include "probe.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using lapis::probe::make_engine;
using lapis::probe::Snapshot;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const lapis::probe::Cell& cell(const Snapshot& state, std::size_t column, std::size_t row) {
    require(state.cells.size() == static_cast<std::size_t>(state.size.columns) * state.size.rows,
            "snapshot must contain exactly the viewport cells");
    require(column < state.size.columns && row < state.size.rows, "cell outside viewport");
    return state.cells.at(row * state.size.columns + column);
}

void expect_text(const Snapshot& state, std::size_t row, std::u32string_view text) {
    for (std::size_t column = 0; column < text.size(); ++column) {
        require(cell(state, column, row).text == std::u32string(1, text[column]),
                "unexpected row text");
    }
}

void expect_style(const lapis::probe::Cell& styled, char32_t text, bool bold,
                  std::uint32_t foreground, std::uint32_t background,
                  std::string_view description) {
    require(styled.text == std::u32string(1, text) && styled.bold == bold,
            std::string(description) + " text/bold mismatch");
    require(styled.foreground_rgb == foreground && styled.background_rgb == background,
            std::string(description) + " RGB mismatch");
}

void ascii_cursor() {
    auto engine = make_engine({12, 4});
    engine->feed("hello\r\nOK");
    const auto state = engine->snapshot();
    expect_text(state, 0, U"hello");
    expect_text(state, 1, U"OK");
    require(state.cursor_row == 1 && state.cursor_column == 2, "incorrect cursor after CRLF");
}

void fragmented_unicode() {
    auto engine = make_engine({12, 4});
    const std::string bytes = "A\xe7\x95\x8c"
                              "e\xcc\x81"
                              "Z";
    for (const char byte : bytes) {
        engine->feed(std::string_view(&byte, 1));
    }
    const auto state = engine->snapshot();
    require(cell(state, 0, 0).text == U"A", "missing ASCII before UTF-8");
    require(cell(state, 1, 0).text == U"\u754c" && cell(state, 1, 0).width == 2,
            "fragmented wide UTF-8 was not retained");
    require(cell(state, 2, 0).width == 0, "wide continuation cell was not marked");
    require(cell(state, 3, 0).text == U"e\u0301", "combining grapheme was not retained");
    require(cell(state, 4, 0).text == U"Z" && state.cursor_column == 5,
            "wrong cursor/cell after combining text");
}

void alternate_screen() {
    auto engine = make_engine({12, 4});
    engine->feed("main");
    engine->feed("\x1b[?1049h\x1b[Halt");
    const auto alternate = engine->snapshot();
    require(alternate.alternate_screen, "alternate screen mode was not exposed");
    expect_text(alternate, 0, U"alt");
    engine->feed("\x1b[?1049l");
    const auto restored = engine->snapshot();
    require(!restored.alternate_screen, "primary screen was not restored");
    expect_text(restored, 0, U"main");
    require(restored.cursor_column == 4 && restored.cursor_row == 0,
            "alternate-screen exit did not restore cursor");
}

void wrap_and_resize() {
    auto engine = make_engine({5, 4});
    engine->feed("abcdef");
    const auto before = engine->snapshot();
    expect_text(before, 0, U"abcde");
    expect_text(before, 1, U"f");
    require(before.cursor_row == 1 && before.cursor_column == 1, "autowrap cursor mismatch");
    engine->resize({3, 4});
    const auto after = engine->snapshot();
    require(after.size.columns == 3 && after.size.rows == 4, "resize did not change geometry");
    expect_text(after, 0, U"abc");
    expect_text(after, 1, U"def");
}

void styles() {
    auto engine = make_engine({12, 4});
    engine->feed("\x1b]4;1;rgb:11/22/33\x1b\\"
                 "\x1b]4;4;rgb:44/55/66\x1b\\"
                 "\x1b]4;9;rgb:a0/a0/a0\x1b\\"
                 "\x1b]4;12;rgb:b0/b0/b0\x1b\\"
                 "\x1b]10;rgb:c0/c0/c0\x1b\\"
                 "\x1b]11;rgb:d0/d0/d0\x1b\\");
    engine->feed("\x1b[1;38;2;17;34;51;48;2;68;85;102mX"
                 "\x1b[0;38;5;1;48;5;4mN"
                 "\x1b[1;38;5;1;48;5;4mB"
                 "\x1b[1;38;5;9;48;5;12mE"
                 "\x1b[0;7;38;5;1;48;5;4mI"
                 "\x1b[1;7;38;5;1;48;5;4mJ"
                 "\x1b[0mR");
    const auto state = engine->snapshot();
    expect_style(cell(state, 0, 0), U'X', true, 0x112233, 0x445566, "truecolor");
    expect_style(cell(state, 1, 0), U'N', false, 0x112233, 0x445566, "normal indexed");
    expect_style(cell(state, 2, 0), U'B', true, 0x112233, 0x445566, "bold indexed");
    expect_style(cell(state, 3, 0), U'E', true, 0xA0A0A0, 0xB0B0B0, "explicit bright indexed");
    expect_style(cell(state, 4, 0), U'I', false, 0x445566, 0x112233, "inverse indexed");
    expect_style(cell(state, 5, 0), U'J', true, 0x445566, 0x112233, "bold inverse indexed");
    expect_style(cell(state, 6, 0), U'R', false, 0xC0C0C0, 0xD0D0D0, "reset");
}

void input_modes() {
    auto engine = make_engine({12, 4});
    require(engine->key_up() == "\x1b[A", "normal cursor key encoding mismatch");
    require(engine->paste("echo hi") == "echo hi", "unbracketed paste encoding mismatch");
    engine->feed("\x1b[?1h\x1b[?2004h");
    require(engine->key_up() == "\x1bOA", "application cursor key encoding mismatch");
    require(engine->snapshot().bracketed_paste, "bracketed paste mode was not exposed");
    require(engine->paste("echo hi") == "\x1b[200~echo hi\x1b[201~",
            "bracketed paste encoding mismatch");
    engine->feed("\x1b[?1l\x1b[?2004l");
    require(engine->key_up() == "\x1b[A" && engine->paste("echo hi") == "echo hi",
            "input modes did not reset");
}

void snapshot_lifetime() {
    Snapshot retained{};
    {
        auto engine = make_engine({12, 4});
        engine->feed("owned");
        retained = engine->snapshot();
        engine->feed("\x1b[2J\x1b[Hnew");
        engine->resize({4, 2});
        static_cast<void>(engine->snapshot());
    }
    expect_text(retained, 0, U"owned");
    require(retained.size.columns == 12 && retained.cursor_column == 5,
            "retained snapshot changed after engine destruction");
}

void output_burst() {
    auto engine = make_engine({12, 4});
    for (int line = 0; line < 1000; ++line) {
        engine->feed("burst\r\n");
    }
    engine->feed("tail");
    const auto state = engine->snapshot();
    expect_text(state, 3, U"tail");
    require(state.cells.size() == 48, "viewport grew with output history");
}

struct Case {
    std::string_view name;
    void (*run)() = nullptr;
};

// Case names/errors are fixed ASCII strings, so no arbitrary terminal/user data
// is inserted into the JSON report. Individual failures must remain visible.
constexpr std::array cases{
    Case{"ascii_cursor", ascii_cursor},
    Case{"fragmented_unicode", fragmented_unicode},
    Case{"alternate_screen", alternate_screen},
    Case{"wrap_and_resize", wrap_and_resize},
    Case{"styles", styles},
    Case{"input_modes", input_modes},
    Case{"snapshot_lifetime", snapshot_lifetime},
    Case{"output_burst", output_burst},
};

} // namespace

int main() {
    bool passed = true;
    std::cout << "{\"schema_version\":1,\"engine\":\"" << lapis::probe::engine_name()
              << "\",\"cases\":[";
    for (std::size_t index = 0; index < cases.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        bool case_passed = true;
        try {
            cases[index].run();
        } catch (const std::exception& error) {
            std::cerr << cases[index].name << ": " << error.what() << '\n';
            case_passed = false;
        } catch (...) {
            std::cerr << cases[index].name << ": unknown exception\n";
            case_passed = false;
        }
        std::cout << "{\"name\":\"" << cases[index].name
                  << "\",\"passed\":" << (case_passed ? "true" : "false") << '}';
        passed = passed && case_passed;
    }
    std::cout << "],\"passed\":" << (passed ? "true" : "false") << "}\n";
    return passed ? 0 : 1;
}
