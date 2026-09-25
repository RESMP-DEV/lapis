#include "terminal_surface.hpp"
#include <lapis/session/terminal.hpp>

#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::terminal_find;

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
} // namespace

int main() {
    try {
        find_walks_the_page();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "terminal find tests passed\n";
    return 0;
}
