#include "terminal_surface.hpp"
#include <lapis/session/terminal.hpp>

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>

#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::lay_out_suggestion;
using lapis::desktop::text_after_cursor;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
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

    // The strip is row-local: a later status or border row cannot hide it.
    lapis::session::Terminal wrapped({40, 3});
    wrapped.feed("> first line\r\n[status]\x1b[A\x1b[5C");
    const auto wrapped_screen = wrapped.snapshot();
    require(!text_after_cursor(wrapped_screen) && shown(wrapped_screen),
            "a later status row did not hide the suggestion");

    // A hidden cursor still means a real character underneath is typed text.
    lapis::session::Terminal hidden_text({40, 3});
    hidden_text.feed("\x1b[?25lhello\x1b[D");
    const auto hidden_screen = hidden_text.snapshot();
    require(text_after_cursor(hidden_screen) && !shown(hidden_screen),
            "a hidden cursor skipped typed text");

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

// Who wrote the guess follows the Tab key where the row has room, and is the
// first thing left out of a narrow one.
void the_author_follows_the_key_when_it_fits() {
    const QFontMetricsF metrics(QFont(QStringLiteral("Menlo"), 14));
    const auto author = QStringLiteral("Fable 5.2 · high");
    lapis::session::Terminal wide({120, 3});
    wide.feed("> ");
    const auto signed_layout =
        lay_out_suggestion(wide.snapshot(), metrics, QStringLiteral("run the tests"), author);
    require(signed_layout.keys.endsWith(author) && signed_layout.shown == "run the tests",
            "a wide row shows the guess, the key and its author");
    lapis::session::Terminal narrow({40, 3});
    narrow.feed("> ");
    const auto plain =
        lay_out_suggestion(narrow.snapshot(), metrics, QStringLiteral("run the tests"), author);
    require(!plain.keys.contains(author) && !plain.shown.isEmpty(),
            "a narrow row keeps the guess and drops the author");
    require(!lay_out_suggestion(wide.snapshot(), metrics, QStringLiteral("run the tests"))
                 .keys.contains(QStringLiteral("·")),
            "no author, none shown");
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    const QGuiApplication app(argc, argv);
    try {
        suggestion_never_covers_typed_text();
        the_author_follows_the_key_when_it_fits();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "terminal suggestion tests passed\n";
    return 0;
}
