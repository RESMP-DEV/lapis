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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
    require(plan && plan->keep_at == 0 && plan->keep_old_rows == 10 && plan->drop_new_rows == 0,
            "the ten items the redraw no longer shows are kept");
}

void anUnchangedScreenNeedsNothing() {
    const auto rows = last(rendered(30, 120), 20);
    const auto plan =
        rw::plan(rw::screen_text(screen(rows, 120, 20)), rw::screen_text(screen(rows, 120, 20)));
    require(plan && plan->keep_at == 0 && plan->keep_old_rows == 0 && plan->drop_at == 0 &&
                plan->drop_new_rows == 0,
            "the same screen lines up with itself");
}

void leadingChromeShiftsAlignmentWindows() {
    // A screen that starts with a rule must not match the redraw's first rule.
    const std::string rule(60, '-');
    auto old_rows = last(rendered(30, 60), 18);
    old_rows.insert(old_rows.begin(), {rule, ""});
    auto new_rows = last(rendered(30, 120), 18);
    new_rows.insert(new_rows.begin(), {std::string(120, '-'), ""});
    const auto plan = rw::plan(rw::screen_text(screen(old_rows, 60, 20)),
                               rw::screen_text(screen(new_rows, 120, 20)));
    // Old shows items 22-30 after its rule; the redraw has items 13-30 after
    // its rule. The shared rule and blank stay; only items 13-21 are dropped.
    require(plan && plan->drop_at == 2 && plan->drop_new_rows == 9 && plan->keep_at == 0 &&
                plan->keep_old_rows == 0,
            "widening drops only the inserted conversation rows");

    const auto narrow_plan = rw::plan(rw::screen_text(screen(new_rows, 120, 20)),
                                      rw::screen_text(screen(old_rows, 60, 20)));
    require(narrow_plan && narrow_plan->keep_at == 2 && narrow_plan->keep_old_rows == 9 &&
                narrow_plan->drop_new_rows == 0,
            "narrowing keeps only the skipped conversation rows");
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

void leadingRedrawChromeIsNotHistory() {
    const auto history = screen(rendered(20, 60), 60, 40);
    auto redraw_rows = last(rendered(30, 120), 20);
    redraw_rows.insert(redraw_rows.begin(), {std::string(120, '-'), ""});
    const auto kept =
        rw::history_kept(rw::screen_text(history), rw::screen_text(screen(redraw_rows, 120, 20)));
    require(kept && *kept == 20, "history keeps the rows before item eleven");
}

void repeatedProbesContinueTheNewestCopy() {
    auto history_rows = rendered(10, 60);
    const auto repeat = rendered(10, 60);
    history_rows.insert(history_rows.end(), repeat.begin(), repeat.end());
    const auto history = screen(history_rows, 60, 40);
    const auto redraw = screen(last(rendered(10, 120), 10), 120, 10);
    const auto kept = rw::history_kept(rw::screen_text(history), rw::screen_text(redraw));
    require(kept && *kept == 20, "the rows between duplicate copies stay in history");
}

void unicodeProseAnchorsAlignment() {
    const std::vector<std::string> cyrillic{
        "  Прошлый ответ сохраняет смысл при изменении ширины",
        "  Новый ответ продолжает этот разговор",
    };
    const std::vector<std::string> cjk{
        "  会话在宽度改变之后仍然保持对齐",
        "  新的回复继续这段对话",
    };
    for (const auto& rows : {cyrillic, cjk}) {
        const auto same =
            rw::plan(rw::screen_text(screen(rows, 80, 4)), rw::screen_text(screen(rows, 120, 4)));
        require(same && same->drop_new_rows == 0 && same->keep_old_rows == 0,
                "non-Latin prose anchors the redraw");
    }
}

void shortContentDoesNotGuessAnAlignment() {
    const auto rows = std::vector<std::string>{"  too short"};
    const auto plan =
        rw::plan(rw::screen_text(screen(rows, 60, 4)), rw::screen_text(screen(rows, 120, 4)));
    require(!plan, "there is no safe probe when the passage is too short");
}

void theOldConversationEndsAtThePromptBox() {
    std::vector<std::string> rows = last(rendered(30, 120), 10);
    rows.insert(rows.end(), {"", "────", "❯ ", "────", "  status"});
    require(rw::conversation_rows(screen(rows, 120, 20)) == 11,
            "the conversation ends above the prompt box's rule");
    require(rw::conversation_rows(screen(last(rendered(30, 120), 10), 120, 20)) == 10,
            "without a box, it ends at the last text");
}

void promptChromeHasVariedShapes() {
    std::vector<std::string> heavy_rows = last(rendered(30, 120), 10);
    heavy_rows.insert(heavy_rows.end(), {"", "━━━━", "➯ ", "  status"});
    require(rw::conversation_rows(screen(heavy_rows, 120, 20)) == 11,
            "a heavy rule and alternate prompt mark end the conversation");

    std::vector<std::string> colored_rows;
    colored_rows.insert(colored_rows.end(), {"────", "\x1b[35m❯\x1b[0m ", "  status"});
    require(rw::conversation_rows(screen(colored_rows, 120, 20)) == 0,
            "color escapes do not hide the prompt box");

    std::vector<std::string> blank_rows = last(rendered(30, 120), 10);
    blank_rows.insert(blank_rows.end(), {"❯ ", "  status"});
    require(rw::conversation_rows(screen(blank_rows, 120, 20)) == 10,
            "an empty prompt without a rule ends the conversation");
}

struct Case {
    const char* name;
    void (*run)();
};

const Case cases[] = {
    {"widening drops the repeated rows", wideningDropsTheRepeatedRows},
    {"narrowing keeps the rows the redraw skips", narrowingKeepsTheRowsTheRedrawSkips},
    {"an unchanged screen needs nothing", anUnchangedScreenNeedsNothing},
    {"leading chrome shifts alignment windows", leadingChromeShiftsAlignmentWindows},
    {"unrelated screens do not line up", unrelatedScreensDoNotLineUp},
    {"widening keeps the wider redraw", wideningKeepsTheWiderRedraw},
    {"leading redraw chrome is not history", leadingRedrawChromeIsNotHistory},
    {"repeated probes continue the newest copy", repeatedProbesContinueTheNewestCopy},
    {"unicode prose anchors alignment", unicodeProseAnchorsAlignment},
    {"short content does not guess an alignment", shortContentDoesNotGuessAnAlignment},
    {"the old conversation ends at the prompt box", theOldConversationEndsAtThePromptBox},
    {"prompt chrome has varied shapes", promptChromeHasVariedShapes},
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
