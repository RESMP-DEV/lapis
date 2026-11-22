#include "history_strip.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using lapis::desktop::HistoryStrip;
using lapis::session::TerminalSnapshot;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// A screen or archived page holding `lines`, placed at row `first`.
TerminalSnapshot rows_of(std::uint16_t columns, const std::vector<std::string>& lines,
                         std::size_t first = 0) {
    lapis::session::Terminal terminal({columns, static_cast<std::uint16_t>(lines.size())});
    std::string text;
    for (std::size_t index = 0; index < lines.size(); ++index)
        text += lines[index] + (index + 1 < lines.size() ? "\r\n" : "");
    terminal.feed(text);
    auto snapshot = terminal.snapshot();
    snapshot.history = {first + lines.size(), first, lines.size(), true};
    return snapshot;
}

std::string row_text(const TerminalSnapshot& snapshot, std::size_t row) {
    std::string text;
    for (std::size_t column = 0; column < snapshot.size.columns; ++column) {
        const auto index = row * snapshot.size.columns + column;
        const auto cell = snapshot.text(index);
        if (cell.empty())
            text += ' ';
        for (const auto value : cell)
            text += value < 0x80 ? static_cast<char>(value) : '?';
    }
    return text.substr(0, text.find_last_not_of(' ') + 1);
}

std::vector<std::string> view_rows(const HistoryStrip& strip) {
    const auto view = strip.view();
    std::vector<std::string> rows;
    rows.reserve(view.size.rows);
    for (std::size_t row = 0; row < view.size.rows; ++row)
        rows.push_back(row_text(view, row));
    return rows;
}

using Rows = std::vector<std::string>;

void resized_screen_overlap_spans_archive_pages() {
    HistoryStrip strip(rows_of(8, {"l6", "l7", "l8", "l9", "live"}), 10);
    strip.addPage(rows_of(8, {"l8", "l9"}, 8));
    strip.moveTo(7);
    require(strip.missing() == 6, "a possible resize overlap must fetch its earlier rows");
    strip.addPage(rows_of(8, {"l2", "l3", "l4", "l5", "l6", "l7"}, 2));
    require(strip.archived() == 6 && strip.top() == 3 &&
                view_rows(strip) == Rows({"l3", "l4", "l5", "l6", "l7"}),
            "resize overlap spanning pages duplicated rows or lost the requested scroll distance");
}

void hyperlinks_survive_history_composition() {
    auto screen = rows_of(6, {"same", "tail"});
    screen.hyperlinks = {{0, 4, "https://example.com/new"}};
    auto page = rows_of(8, {"12345678", "same"});
    page.hyperlinks = {{2, 10, "https://example.com/old"}};
    HistoryStrip strip(screen, 2);
    strip.addPage(page);
    require(strip.top() == 2, "different link destinations were deduplicated at the history seam");
    strip.moveTo(1);
    auto view = strip.view();
    require(view.hyperlinks.size() == 2 && view.hyperlinks[0].first_cell == 0 &&
                view.hyperlinks[0].cell_count == 4 && view.hyperlinks[1].first_cell == 6 &&
                view.hyperlinks[1].uri == "https://example.com/new",
            "history links were lost or misaligned across the live screen seam");
    strip.moveTo(0);
    view = strip.view();
    require(view.hyperlinks.size() == 1 && view.hyperlinks[0].first_cell == 2 &&
                view.hyperlinks[0].cell_count == 8,
            "history links were not clipped and merged at the narrower viewport");
}

void views_are_whole_screens_across_pages() {
    // Six archived rows in a page of four and a short one of two, then the
    // screen: every view is a whole screen, whatever the pages' sizes.
    HistoryStrip strip(rows_of(10, {"s0", "s1", "s2", "s3"}), 6);
    require(strip.top() == 6 && view_rows(strip) == Rows({"s0", "s1", "s2", "s3"}),
            "browsing begins at the screen");
    strip.moveTo(5);
    require(strip.missing() == 5, "a row no page holds is missing");
    strip.addPage(rows_of(10, {"h4", "h5"}, 4));
    require(!strip.missing() && view_rows(strip) == Rows({"h5", "s0", "s1", "s2"}),
            "one row back shows the newest kept row above the screen");
    strip.moveTo(2);
    require(strip.missing() == 2, "the rows above the short page are missing");
    strip.addPage(rows_of(10, {"h0", "h1", "h2", "h3"}, 0));
    require(view_rows(strip) == Rows({"h2", "h3", "h4", "h5"}), "a view spans two pages");
    const auto view = strip.view();
    require(view.size == strip.view().size && view.size.rows == 4 && !view.cursor.in_viewport,
            "the view is the screen's size, without a cursor");
    require(view.history.viewport_offset == 2 && view.history.total_rows == 10 &&
                view.history.viewport_rows == 4,
            "the view says where it sits in the strip");
    // A trackpad scrolled part of a row back shows the row above the view.
    const auto above = strip.rowAbove();
    require(above && above->size.rows == 1 && above->size.columns == 10 &&
                row_text(*above, 0) == "h1",
            "the row above the view is the one row before it");
    strip.moveTo(6);
    require(strip.rowAbove() && row_text(*strip.rowAbove(), 0) == "h5",
            "above the screen is the newest kept row");
    strip.moveTo(-5);
    require(strip.top() == 0 && view_rows(strip) == Rows({"h0", "h1", "h2", "h3"}),
            "the view stops at the oldest kept row");
    require(!strip.rowAbove(), "nothing is above the oldest kept row");
    strip.moveTo(100);
    require(strip.top() == 6, "and at the screen");
    strip.addPage(rows_of(10, {"late"}, 6));
    require(view_rows(strip) == Rows({"s0", "s1", "s2", "s3"}),
            "rows archived after browsing began stay below the kept screen");
}

void pages_of_another_width_and_color_fit_the_screen() {
    auto screen = rows_of(5, {"s0", "s1"});
    screen.background_rgb = 0x101820;
    HistoryStrip strip(screen, 2);
    // 你 is two cells wide: at columns four and five it does not fit in five.
    auto page = rows_of(8, {"abcd\xE4\xBD\xA0", "xy"}, 0);
    page.background_rgb = 0x000000;
    strip.addPage(page);
    strip.moveTo(0);
    const auto view = strip.view();
    require(row_text(view, 0) == "abcd" && view.text(4).empty(),
            "a wider page is cut, and a wide character at the edge is dropped");
    require(row_text(view, 1) == "xy" && view.background_rgb == 0x101820,
            "rows take the screen's colors");
    HistoryStrip narrow(rows_of(8, {"s0"}), 1);
    narrow.addPage(rows_of(3, {"abc"}, 0));
    narrow.moveTo(0);
    require(view_rows(narrow) == Rows({"abc"}) && narrow.view().size.columns == 8,
            "a narrower page is padded");
}

void pages_that_do_not_say_where_they_sit_go_on_top() {
    // A service before placed pages: each page says it is all there is.
    HistoryStrip strip(rows_of(6, {"s0", "s1"}), 2);
    strip.addPage(rows_of(6, {"n0", "n1"}, 0));
    strip.moveTo(0);
    strip.prependPage(rows_of(6, {"o0", "o1", "o2"}, 0));
    require(strip.archived() == 5 && strip.top() == 3 && view_rows(strip) == Rows({"n0", "n1"}),
            "an older page goes above without moving the view");
    strip.moveTo(1);
    require(view_rows(strip) == Rows({"o1", "o2"}), "and scrolls on into it");
}

void kept_rows_the_screen_shows_again_belong_to_it() {
    // The screen grew and brought back the two newest kept rows.
    HistoryStrip strip(rows_of(10, {"h4", "h5", "s0", "s1"}), 6);
    strip.addPage(rows_of(10, {"h2", "h3", "h4", "h5"}, 2));
    require(strip.archived() == 4 && strip.top() == 4,
            "the kept rows the screen shows again are the screen's");
    strip.moveTo(1);
    strip.addPage(rows_of(10, {"h0", "h1"}, 0));
    require(view_rows(strip) == Rows({"h1", "h2", "h3", "h4"}), "and the join shows each row once");
    // Blank rows alone prove nothing: both keep theirs.
    HistoryStrip blank(rows_of(10, {"", "s0"}), 2);
    blank.addPage(rows_of(10, {"h0", ""}, 0));
    require(blank.archived() == 2, "a blank row on both sides is not taken for a repeat");
}

void pages_without_seam_evidence_stay_unsettled() {
    // Rows 0 and 1 are known, but every plausible overlap starts at rows 4
    // and 5. The distant page cannot settle a seam it never reached.
    HistoryStrip distant(rows_of(10, {"h4", "h5", "s0", "s1"}), 6);
    distant.addPage(rows_of(10, {"d0", "d1"}, 0));
    distant.moveTo(5);
    require(distant.missing() == 5, "a page away from the seam still requests its missing row");
    distant.moveTo(6);
    distant.addPage(rows_of(10, {"h2", "h3", "h4", "h5"}, 2));
    require(distant.archived() == 4 && distant.top() == 4 &&
                view_rows(distant) == Rows({"h4", "h5", "s0", "s1"}),
            "a later seam page still removes rows shown by the screen");

    // The blank row matches, but the other half of this possible overlap is
    // missing; together they are not evidence that two rows repeat.
    HistoryStrip partial(rows_of(10, {"h1", "", "s0"}), 3);
    partial.addPage(rows_of(10, {""}, 2));
    require(partial.archived() == 3 && partial.top() == 3,
            "a blank row beside a missing row does not settle the seam");
    partial.addPage(rows_of(10, {"h0", "h1"}, 0));
    require(partial.archived() == 1 && partial.top() == 1 && !partial.missing() &&
                view_rows(partial) == Rows({"h1", "", "s0"}),
            "a later page can prove a partial blank-and-missing overlap");
}

void many_screens_stay_and_the_furthest_go_first() {
    constexpr std::uint64_t screen = 4;
    constexpr auto budget = HistoryStrip::kept_screens * screen;
    const auto archived = budget * 2;
    HistoryStrip strip(rows_of(4, {"s0", "s1", "s2", "s3"}), archived);
    strip.moveTo(0);
    for (std::uint64_t first = 0; first < budget; first += screen)
        strip.addPage(rows_of(4, {"a", "b", "c", "d"}, first), first + 1);
    require(strip.pages() == HistoryStrip::kept_screens && strip.keptRows() == budget,
            "the whole budget stays");
    // Far more than four screens away: scrolling back needs no fetch.
    strip.moveTo(static_cast<std::int64_t>(budget - screen));
    require(!strip.missing() && strip.pages() == HistoryStrip::kept_screens,
            "pages dozens of screens from the view are kept");
    strip.moveTo(0);
    require(!strip.missing(), "and are there on the way back");
    // Past the budget, the page furthest from the view goes first.
    strip.moveTo(static_cast<std::int64_t>(budget));
    strip.addPage(rows_of(4, {"e", "f", "g", "h"}, budget), 999);
    require(strip.keptRows() == budget && strip.missing() == std::nullopt &&
                view_rows(strip) == Rows({"e", "f", "g", "h"}),
            "a new page in view displaces one");
    strip.moveTo(0);
    require(strip.missing() == 0, "the furthest page, row 0, was the one dropped");
    strip.moveTo(static_cast<std::int64_t>(screen));
    require(!strip.missing(), "its neighbour stayed");
}

void neighbours_are_fetched_ahead() {
    // Pages of four rows; the view of four sits on rows 40-43.
    HistoryStrip strip(rows_of(4, {"s0", "s1", "s2", "s3"}), 80);
    strip.moveTo(40);
    require(strip.ahead(2, 1) == std::nullopt, "nothing ahead while the view itself is missing");
    strip.addPage(rows_of(4, {"a", "b", "c", "d"}, 40), 11);
    require(strip.ahead(2, 1) == 39, "the screen just above comes first, nearest row first");
    strip.addPage(rows_of(4, {"a", "b", "c", "d"}, 36), 10);
    require(strip.ahead(2, 1) == 44, "then the screen just below");
    strip.addPage(rows_of(4, {"a", "b", "c", "d"}, 44), 12);
    require(strip.ahead(2, 1) == 35, "then the second screen above");
    strip.addPage(rows_of(4, {"a", "b", "c", "d"}, 32), 9);
    require(strip.ahead(2, 1) == std::nullopt, "the window is full");
    strip.moveTo(76);
    strip.addPage(rows_of(4, {"a", "b", "c", "d"}, 76), 20);
    require(strip.ahead(1, 3) == 75, "nothing below the newest kept row but the screen");
    HistoryStrip top(rows_of(4, {"s0", "s1", "s2", "s3"}), 8);
    top.moveTo(0);
    top.addPage(rows_of(4, {"a", "b", "c", "d"}, 0), 1);
    require(top.ahead(2, 2) == 4, "at the oldest row only below remains");

    // Taken pages keep their IDs and places, nearest the view first to stay.
    auto taken = strip.takePages(2);
    require(strip.pages() == 0 && taken.size() == 2, "two screens of named pages taken");
    require(taken[0].id == 12 && taken[0].snapshot.history.viewport_offset == 44 &&
                taken[1].id == 20 && taken[1].snapshot.history.viewport_offset == 76,
            "furthest dropped; IDs and places kept");
}
} // namespace

int main() {
    try {
        resized_screen_overlap_spans_archive_pages();
        hyperlinks_survive_history_composition();
        views_are_whole_screens_across_pages();
        pages_of_another_width_and_color_fit_the_screen();
        pages_that_do_not_say_where_they_sit_go_on_top();
        kept_rows_the_screen_shows_again_belong_to_it();
        pages_without_seam_evidence_stay_unsettled();
        many_screens_stay_and_the_furthest_go_first();
        neighbours_are_fetched_ahead();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "history strip tests passed\n";
    return 0;
}
