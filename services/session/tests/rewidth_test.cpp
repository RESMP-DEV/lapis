// Lining up Claude Code's redraw after a width change with the screen it
// replaced (src/rewidth.hpp), on synthetic conversations: each item wraps to
// two rows at 60 columns and fits one row at 120.
#include "rewidth.hpp"

#include <lapis/session/terminal.hpp>

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using lapis::session::Terminal;
using lapis::session::TerminalSize;
using lapis::session::TerminalSnapshot;
namespace rw = lapis::session::rewidth;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::string item(int number) {
    // About 90 characters, distinct from its first words on.
    return "Item " + std::to_string(number) + " says something only item " +
           std::to_string(number) + " says, long enough that it wraps at sixty columns.";
}

// The conversation drawn at `width` the way the classic renderer does: a
// two-space indent, and continuation rows indented too.
std::vector<std::string> rendered(int items, std::size_t width) {
    std::vector<std::string> rows;
    for (int number = 1; number <= items; ++number) {
        std::string text = item(number);
        std::string row = "  ";
        std::size_t start = 0;
        while (start < text.size()) {
            const auto room = width - row.size();
            if (text.size() - start <= room) {
                row += text.substr(start);
                break;
            }
            auto cut = text.rfind(' ', start + room);
            if (cut == std::string::npos || cut <= start)
                cut = start + room;
            row += text.substr(start, cut - start);
            rows.push_back(row);
            row = "    ";
            start = cut + 1;
        }
        rows.push_back(row);
    }
    return rows;
}

// A screen showing `rows`, top to bottom.
TerminalSnapshot screen(const std::vector<std::string>& rows, std::size_t width,
                        std::size_t height) {
    Terminal terminal(
        TerminalSize{static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height)});
    std::string bytes;
    for (std::size_t index = 0; index < rows.size(); ++index)
        bytes += (index ? "\r\n" : "") + rows[index];
    terminal.feed(bytes);
    return terminal.snapshot();
}

std::vector<std::string> last(const std::vector<std::string>& rows, std::size_t count) {
    return {rows.end() - static_cast<std::ptrdiff_t>(count), rows.end()};
}

void wideningDropsTheRepeatedRows() {
    // 20 rows at 60 columns show items 21 to 30; at 120 they show 11 to 30.
    const auto old_screen = screen(last(rendered(30, 60), 20), 60, 20);
    const auto redraw = screen(last(rendered(30, 120), 20), 120, 20);
    const auto plan = rw::plan(rw::screen_text(old_screen), rw::screen_text(redraw));
    require(plan && plan->drop_new_rows == 10 && plan->keep_old_rows == 0,
            "the ten items already in history are dropped from the redraw");
}

void narrowingKeepsTheRowsTheRedrawSkips() {
    const auto old_screen = screen(last(rendered(30, 120), 20), 120, 20);
    const auto redraw = screen(last(rendered(30, 60), 20), 60, 20);
    const auto plan = rw::plan(rw::screen_text(old_screen), rw::screen_text(redraw));
    require(plan && plan->keep_old_rows == 10 && plan->drop_new_rows == 0,
            "the ten items the redraw no longer shows are kept");
}

void anUnchangedScreenNeedsNothing() {
    const auto rows = last(rendered(30, 120), 20);
    const auto plan =
        rw::plan(rw::screen_text(screen(rows, 120, 20)), rw::screen_text(screen(rows, 120, 20)));
    require(plan && plan->keep_old_rows == 0 && plan->drop_new_rows == 0,
            "the same screen lines up with itself");
}

void rulesDoNotLineUp() {
    // A screen that starts with a rule must not match the redraw's first rule.
    const std::string rule(60, '-');
    auto old_rows = last(rendered(30, 60), 18);
    old_rows.insert(old_rows.begin(), {rule, ""});
    auto new_rows = last(rendered(30, 120), 18);
    new_rows.insert(new_rows.begin(), {std::string(120, '-'), ""});
    const auto plan = rw::plan(rw::screen_text(screen(old_rows, 60, 20)),
                               rw::screen_text(screen(new_rows, 120, 20)));
    // Old shows items 22-30 after its rule; the redraw has items 13-30 after
    // its rule, so item 22 is 9 rows below where old's first words sit.
    require(plan && plan->drop_new_rows == 9, "the probe starts at the first words");
}

void unrelatedScreensDoNotLineUp() {
    const auto other =
        std::vector<std::string>{"  Something else entirely, with plenty of words in it."};
    const auto plan = rw::plan(rw::screen_text(screen(last(rendered(30, 60), 20), 60, 20)),
                               rw::screen_text(screen(other, 120, 20)));
    require(!plan, "no plan when nothing matches");
}

void wideningKeepsTheWiderRedraw() {
    // History holds items 1 to 20 at 60 columns (40 rows); the redraw at 120
    // starts at item 11, so history keeps its first 20 rows.
    const auto history = screen(rendered(20, 60), 60, 40);
    const auto redraw = screen(last(rendered(30, 120), 20), 120, 20);
    const auto kept = rw::history_kept(rw::screen_text(history), rw::screen_text(redraw));
    require(kept && *kept == 20, "the older copy of items 11 to 20 is dropped");
}

void theOldConversationEndsAtThePromptBox() {
    std::vector<std::string> rows = last(rendered(30, 120), 10);
    rows.insert(rows.end(), {"", "────", "❯ ", "────", "  status"});
    require(rw::conversation_rows(screen(rows, 120, 20)) == 11,
            "the conversation ends above the prompt box's rule");
    require(rw::conversation_rows(screen(last(rendered(30, 120), 10), 120, 20)) == 10,
            "without a box, it ends at the last text");
}

struct Case {
    const char* name;
    void (*run)();
};

const Case cases[] = {
    {"widening drops the repeated rows", wideningDropsTheRepeatedRows},
    {"narrowing keeps the rows the redraw skips", narrowingKeepsTheRowsTheRedrawSkips},
    {"an unchanged screen needs nothing", anUnchangedScreenNeedsNothing},
    {"rules do not line up", rulesDoNotLineUp},
    {"unrelated screens do not line up", unrelatedScreensDoNotLineUp},
    {"widening keeps the wider redraw", wideningKeepsTheWiderRedraw},
    {"the old conversation ends at the prompt box", theOldConversationEndsAtThePromptBox},
};

} // namespace

int main() {
    bool passed = true;
    for (const Case& test_case : cases) {
        try {
            test_case.run();
            std::cout << "PASS " << test_case.name << "\n";
        } catch (const std::exception& error) {
            passed = false;
            std::cout << "FAIL " << test_case.name << ": " << error.what() << "\n";
        }
    }
    return passed ? 0 : 1;
}
