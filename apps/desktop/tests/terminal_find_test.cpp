#include "terminal_surface.hpp"
#include <lapis/session/terminal.hpp>

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>

#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::lay_out_suggestion;
using lapis::desktop::terminal_find;
using lapis::desktop::text_after_cursor;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Finding moves through the screen row by row, ignoring case, from wherever
// the last match was selected.
void find_walks_the_page() {
    lapis::session::Terminal terminal({20, 4});
    terminal.feed("build ok\r\nTest one\r\nno match\r\ntest two, TEST");
    const auto page = terminal.snapshot();
    const auto last = terminal_find(page, QStringLiteral("test"), QPoint(20, 3), true);
    require(last && last->row == 3 && last->first_column == 10 && last->last_column == 13,
            "backwards from the end finds the last match first");
    const auto earlier = terminal_find(page, QStringLiteral("test"), QPoint(10, 3), true);
    require(earlier && earlier->row == 3 && earlier->first_column == 0,
            "then the one before it on the same row");
    const auto above = terminal_find(page, QStringLiteral("test"), QPoint(0, 3), true);
    require(above && above->row == 1 && above->first_column == 0 && above->last_column == 3,
            "then a row above, in any case");
    require(!terminal_find(page, QStringLiteral("test"), QPoint(0, 1), true),
            "and nothing before the first");
    const auto next = terminal_find(page, QStringLiteral("TEST"), QPoint(-1, 0), false);
    require(next && next->row == 1, "forwards from the top finds the first");
    require(!terminal_find(page, QStringLiteral("absent"), QPoint(0, 0), false) &&
                !terminal_find(page, {}, QPoint(0, 0), false),
            "no match, and no empty search");
}

// The offered next prompt is drawn after the cursor over a filled strip, so it
// shows only where nothing typed follows the cursor: moving left into typed
// text hides it rather than covering the text, and moving back shows it again.
void suggestion_never_covers_typed_text() {
    const QFontMetricsF metrics(QFont(QStringLiteral("Menlo"), 14));
    const auto shown = [&](const lapis::session::TerminalSnapshot& screen) {
        const auto layout = lay_out_suggestion(screen, metrics, QStringLiteral("run the tests"));
        return !layout.shown.isEmpty();
    };
    lapis::session::Terminal terminal({40, 3});
    terminal.feed("> fix the build");
    auto screen = terminal.snapshot();
    require(!text_after_cursor(screen) && shown(screen), "at the end of the line it shows");
    terminal.feed("\x1b[5D");
    screen = terminal.snapshot();
    require(text_after_cursor(screen) && !shown(screen), "inside typed text it hides");
    terminal.feed("\x1b[5C");
    require(shown(terminal.snapshot()), "back at the end it shows again");

    // Wrapped and multi-line drafts keep their later rows in the same screen.
    lapis::session::Terminal wrapped({40, 3});
    wrapped.feed("> first line\r\nsecond line\x1b[A");
    const auto wrapped_screen = wrapped.snapshot();
    require(text_after_cursor(wrapped_screen) && !shown(wrapped_screen),
            "typed text on a later wrapped row hides");

    // A CLI that hides the hardware cursor and draws its own glyph at that
    // cell is not typed text. The offered text starts beside the glyph.
    lapis::session::Terminal software_cursor({40, 3});
    software_cursor.feed("\x1b[?25l> ▏\x1b[D");
    const auto software_screen = software_cursor.snapshot();
    require(!text_after_cursor(software_screen) && shown(software_screen),
            "a lone software-cursor glyph hid the suggestion");

    // A faint placeholder at the cursor (Codex draws one) does not count, nor
    // does a border in the row's last two cells (Claude Code's input box).
    lapis::session::Terminal codex({40, 3});
    codex.feed("> \x1b[2mAsk Codex to do anything\x1b[22m\x1b[1;3H");
    require(shown(codex.snapshot()), "a faint placeholder is covered as before");
    lapis::session::Terminal claude({40, 3});
    claude.feed("\x1b[1;40H|\x1b[1;1H> hi");
    require(shown(claude.snapshot()), "a border at the row's end does not hide it");
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    const QGuiApplication app(argc, argv);
    try {
        find_walks_the_page();
        suggestion_never_covers_typed_text();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "terminal find tests passed\n";
    return 0;
}
