#include "history_strip.hpp"
#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>
namespace lapis::desktop {
namespace {
// One row of `source` into row `row` of `out`, cut or padded to its width.
void copy_row(const session::TerminalSnapshot& source, std::size_t source_row,
              session::TerminalSnapshot& out, std::size_t row, std::size_t& hyperlink_bytes) {
    const std::size_t source_columns = source.size.columns;
    std::size_t columns = std::min<std::size_t>(source_columns, out.size.columns);
    for (std::size_t column = 0; column < columns; ++column) {
        const auto index = source_row * source_columns + column;
        auto cell = source.cells[index];
        const auto text = source.text(index);
        cell.text_offset = static_cast<std::uint32_t>(out.graphemes.size());
        cell.text_length = static_cast<std::uint32_t>(text.size());
        out.graphemes.append(text);
        out.cells[row * out.size.columns + column] = cell;
    }
    // A wide character cut at the edge would lose its second half.
    if (columns > 0 && columns < source_columns &&
        source.cells[source_row * source_columns + columns].kind == session::CellKind::wide_tail) {
        out.cells[row * out.size.columns + columns - 1] = {};
        --columns;
    }
    const auto begin = source_row * source_columns;
    for (const auto& link : source.hyperlinks) {
        const auto first = std::max<std::size_t>(link.first_cell, begin);
        const auto end =
            std::min<std::size_t>(std::size_t{link.first_cell} + link.cell_count, begin + columns);
        if (end <= first)
            continue;
        const auto target = row * out.size.columns + first - begin;
        const auto count = static_cast<std::uint32_t>(end - first);
        if (!out.hyperlinks.empty()) {
            auto& previous = out.hyperlinks.back();
            if (std::size_t{previous.first_cell} + previous.cell_count == target &&
                previous.uri == link.uri) {
                previous.cell_count += count;
                continue;
            }
        }
        if (link.uri.size() > session::max_hyperlink_uri_bytes ||
            hyperlink_bytes > session::max_hyperlink_bytes - link.uri.size() ||
            out.hyperlinks.size() == session::max_hyperlink_spans)
            continue;
        out.hyperlinks.push_back({static_cast<std::uint32_t>(target), count, link.uri});
        hyperlink_bytes += link.uri.size();
    }
}

std::string_view uri_at(const session::TerminalSnapshot& snapshot, std::size_t cell) {
    const auto found = std::upper_bound(
        snapshot.hyperlinks.begin(), snapshot.hyperlinks.end(), cell,
        [](std::size_t index, const auto& link) { return index < link.first_cell; });
    if (found == snapshot.hyperlinks.begin())
        return {};
    const auto& link = *std::prev(found);
    return cell - link.first_cell < link.cell_count ? std::string_view(link.uri)
                                                    : std::string_view();
}

// Whether two rows show the same cells; columns past a row's width are blank.
bool same_row(const session::TerminalSnapshot& a, std::size_t a_row,
              const session::TerminalSnapshot& b, std::size_t b_row) {
    const std::size_t columns = std::max(a.size.columns, b.size.columns);
    for (std::size_t column = 0; column < columns; ++column) {
        const bool in_a = column < a.size.columns;
        const bool in_b = column < b.size.columns;
        const auto a_index = a_row * a.size.columns + column;
        const auto b_index = b_row * b.size.columns + column;
        const auto a_text = in_a ? a.text(a_index) : std::u32string_view();
        const auto b_text = in_b ? b.text(b_index) : std::u32string_view();
        if (a_text != b_text || (in_a ? uri_at(a, a_index) : std::string_view()) !=
                                    (in_b ? uri_at(b, b_index) : std::string_view()))
            return false;
        if (in_a && in_b && a.cells[a_index].style != b.cells[b_index].style)
            return false;
    }
    return true;
}

bool blank_row(const session::TerminalSnapshot& snapshot, std::size_t row) {
    for (std::size_t column = 0; column < snapshot.size.columns; ++column)
        for (const auto value : snapshot.text(row * snapshot.size.columns + column))
            if (value != U' ')
                return false;
    return true;
}
} // namespace

HistoryStrip::HistoryStrip(session::TerminalSnapshot screen, std::uint64_t archived)
    : screen_(std::move(screen)), archived_(archived), top_(archived) {}

void HistoryStrip::addPage(session::TerminalSnapshot page, std::uint64_t id) {
    if (page.size.rows == 0 || page.size.columns == 0)
        return;
    const auto first = static_cast<std::uint64_t>(page.history.viewport_offset);
    if (first >= archived_)
        return; // archived after browsing began: below the screen kept here
    pages_.insert_or_assign(first, Page{id, std::move(page)});
    if (!seam_settled_)
        settleSeam();
    forget();
}

void HistoryStrip::settleSeam() {
    // A grown screen can overlap more than the newest archive page. Compare
    // known rows first, and fetch earlier rows only for a plausible overlap.
    seam_missing_.reset();
    const auto most = std::min<std::uint64_t>(archived_, rows() > 0 ? rows() - 1 : 0);
    bool established = false;
    bool unavailable = false;
    bool comparable = false;
    for (auto overlap = most; overlap > 0; --overlap) {
        bool same = true;
        bool shown = false;
        std::optional<std::uint64_t> missing;
        for (std::uint64_t row = 0; row < overlap && same; ++row) {
            const auto kept = archived_ - overlap + row;
            std::uint64_t first{};
            const auto* page = holding(kept, &first);
            if (!page) {
                unavailable = true;
                if (!missing)
                    missing = kept;
                continue;
            }
            comparable = comparable || !blank_row(screen_, row);
            same = same_row(*page, kept - first, screen_, row);
            shown = shown || !blank_row(screen_, row);
        }
        if (!same || !shown)
            continue;
        if (missing) {
            seam_missing_ = missing;
            return;
        }
        const auto distance = archived_ - top_;
        archived_ -= overlap;
        top_ = distance < archived_ ? archived_ - distance : 0;
        established = true;
        break;
    }
    // Rows no page holds, and blank rows alone, prove nothing about the seam.
    // Leave it open until a page offers comparable nonblank evidence.
    seam_settled_ = established || (!unavailable && comparable);
}

void HistoryStrip::prependPage(session::TerminalSnapshot page) {
    const std::uint64_t rows = page.size.rows;
    if (rows == 0 || page.size.columns == 0)
        return;
    std::map<std::uint64_t, Page> moved;
    for (auto& [first, kept] : pages_)
        moved.emplace(first + rows, std::move(kept));
    moved.emplace(0, Page{0, std::move(page)});
    pages_ = std::move(moved);
    archived_ += rows;
    top_ += rows;
    forget();
}

void HistoryStrip::moveTo(std::int64_t top) {
    top_ = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
        top, 0, static_cast<std::int64_t>(std::min<std::uint64_t>(archived_, INT64_MAX))));
    forget();
}

const session::TerminalSnapshot* HistoryStrip::holding(std::uint64_t row,
                                                       std::uint64_t* first) const {
    auto found = pages_.upper_bound(row);
    if (found == pages_.begin())
        return nullptr;
    --found;
    if (row >= found->first + found->second.snapshot.size.rows)
        return nullptr;
    *first = found->first;
    return &found->second.snapshot;
}

std::optional<std::uint64_t> HistoryStrip::firstMissing(std::uint64_t from,
                                                        std::uint64_t to) const {
    for (auto row = from; row < std::min(to, archived_);) {
        std::uint64_t first{};
        const auto* page = holding(row, &first);
        if (page == nullptr)
            return row;
        row = first + page->size.rows;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> HistoryStrip::ahead(std::uint64_t above, std::uint64_t below) const {
    if (missing())
        return std::nullopt;
    const auto screen = std::max<std::uint64_t>(1, rows());
    for (std::uint64_t band = 0; band < std::max(above, below); ++band) {
        // The screen `band + 1` above: its nearest missing row is its last.
        if (band < above && top_ > band * screen) {
            const auto to = top_ - band * screen;
            const auto from = to > screen ? to - screen : 0;
            for (auto row = to; row > from; --row) {
                std::uint64_t first{};
                const auto* page = holding(row - 1, &first);
                if (page == nullptr)
                    return row - 1;
                row = first + 1; // skip the rest of this page
            }
        }
        if (band < below) {
            const auto from = top_ + rows() + band * screen;
            if (const auto row = firstMissing(from, from + screen))
                return row;
        }
    }
    return std::nullopt;
}

std::uint64_t HistoryStrip::keptRows() const {
    std::uint64_t total{};
    for (const auto& [first, page] : pages_)
        total += page.snapshot.size.rows;
    return total;
}

std::vector<HistoryStrip::Page> HistoryStrip::takePages(std::uint64_t screens) {
    forget(screens);
    std::vector<Page> taken;
    taken.reserve(pages_.size());
    for (auto& [first, page] : pages_) {
        if (page.id == 0)
            continue;
        page.snapshot.history.viewport_offset = first;
        taken.push_back(std::move(page));
    }
    pages_.clear();
    return taken;
}

std::optional<std::uint64_t> HistoryStrip::missing() const {
    if (seam_missing_)
        return seam_missing_;
    return firstMissing(top_, top_ + rows());
}

// Positional contract; callers and tests pin the order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
session::TerminalSnapshot HistoryStrip::rows_from(std::uint64_t first_row,
                                                  std::uint16_t count) const {
    session::TerminalSnapshot out;
    out.revision = screen_.revision;
    out.size = {screen_.size.columns, count};
    out.accepts_wheel = screen_.accepts_wheel;
    out.foreground_rgb = screen_.foreground_rgb;
    out.background_rgb = screen_.background_rgb;
    out.cursor_rgb = screen_.cursor_rgb;
    out.palette = screen_.palette;
    out.cells.resize(static_cast<std::size_t>(out.size.columns) * count);
    std::size_t hyperlink_bytes{};
    for (std::uint64_t row = 0; row < count; ++row) {
        const auto strip_row = first_row + row;
        if (strip_row >= archived_) {
            copy_row(screen_, strip_row - archived_, out, row, hyperlink_bytes);
            continue;
        }
        std::uint64_t first{};
        if (const auto* page = holding(strip_row, &first))
            copy_row(*page, strip_row - first, out, row, hyperlink_bytes);
    }
    return out;
}

session::TerminalSnapshot HistoryStrip::view() const {
    auto out = rows_from(top_, screen_.size.rows);
    out.history = {archived_ + rows(), top_, rows(), true};
    return out;
}

std::optional<session::TerminalSnapshot> HistoryStrip::rowAbove() const {
    if (top_ == 0)
        return std::nullopt;
    auto out = rows_from(top_ - 1, 1);
    out.cursor.visible = false;
    return out;
}

void HistoryStrip::forget(std::uint64_t screens) {
    const auto budget = screens * std::max<std::uint64_t>(1, rows());
    auto kept = keptRows();
    const auto view_end = top_ + rows();
    // Pages do not overlap, so the furthest from the view is at one end.
    const auto distance = [this, view_end](const auto& entry) -> std::uint64_t {
        const auto end = entry.first + entry.second.snapshot.size.rows;
        if (end <= top_)
            return top_ - end + 1;
        return entry.first >= view_end ? entry.first - view_end + 1 : 0;
    };
    while (kept > budget && !pages_.empty()) {
        const auto low = pages_.begin();
        const auto high = std::prev(pages_.end());
        const auto far = distance(*low) >= distance(*high) ? low : high;
        if (distance(*far) == 0)
            break; // only pages in view remain
        kept -= far->second.snapshot.size.rows;
        pages_.erase(far);
    }
}
} // namespace lapis::desktop
