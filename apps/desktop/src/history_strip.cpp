#include "history_strip.hpp"
#include <algorithm>
#include <utility>
namespace lapis::desktop {
namespace {
// Pages kept within this many screens of the view.
constexpr std::uint64_t kept_screens = 4;

// One row of `source` into row `row` of `out`, cut or padded to its width.
void copy_row(const session::TerminalSnapshot& source, std::size_t source_row,
              session::TerminalSnapshot& out, std::size_t row) {
    const std::size_t source_columns = source.size.columns;
    const std::size_t columns = std::min<std::size_t>(source_columns, out.size.columns);
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
        source.cells[source_row * source_columns + columns].kind == session::CellKind::wide_tail)
        out.cells[row * out.size.columns + columns - 1] = {};
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
        if (a_text != b_text)
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

void HistoryStrip::addPage(session::TerminalSnapshot page) {
    if (page.size.rows == 0 || page.size.columns == 0)
        return;
    const auto first = static_cast<std::uint64_t>(page.history.viewport_offset);
    if (first >= archived_)
        return; // archived after browsing began: below the screen kept here
    if (!seam_settled_ && first + page.size.rows >= archived_) {
        seam_settled_ = true;
        settleSeam(page, first);
    }
    pages_.insert_or_assign(first, std::move(page));
    forget();
}

void HistoryStrip::settleSeam(const session::TerminalSnapshot& page, std::uint64_t first) {
    // The most kept rows, ending at the newest, that the screen shows again at
    // its top; a run of blank rows alone proves nothing.
    const auto tail = archived_ - first; // kept rows of this page, the newest last
    const auto most = std::min<std::uint64_t>(tail, rows() > 0 ? rows() - 1 : 0);
    for (auto overlap = most; overlap > 0; --overlap) {
        bool same = true;
        bool shown = false;
        for (std::uint64_t row = 0; row < overlap && same; ++row) {
            const auto kept = tail - overlap + row;
            same = same_row(page, kept, screen_, row);
            shown = shown || !blank_row(screen_, row);
        }
        if (same && shown) {
            const bool at_screen = top_ == archived_;
            archived_ -= overlap;
            top_ = at_screen ? archived_ : std::min(top_, archived_);
            return;
        }
    }
}

void HistoryStrip::prependPage(session::TerminalSnapshot page) {
    const std::uint64_t rows = page.size.rows;
    if (rows == 0 || page.size.columns == 0)
        return;
    std::map<std::uint64_t, session::TerminalSnapshot> moved;
    for (auto& [first, kept] : pages_)
        moved.emplace(first + rows, std::move(kept));
    moved.emplace(0, std::move(page));
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
    if (row >= found->first + found->second.size.rows)
        return nullptr;
    *first = found->first;
    return &found->second;
}

std::optional<std::uint64_t> HistoryStrip::missing() const {
    const auto end = std::min(archived_, top_ + rows());
    for (auto row = top_; row < end;) {
        std::uint64_t first{};
        const auto* page = holding(row, &first);
        if (page == nullptr)
            return row;
        row = first + page->size.rows;
    }
    return std::nullopt;
}

session::TerminalSnapshot HistoryStrip::view() const {
    session::TerminalSnapshot out;
    out.revision = screen_.revision;
    out.size = screen_.size;
    out.accepts_wheel = screen_.accepts_wheel;
    out.foreground_rgb = screen_.foreground_rgb;
    out.background_rgb = screen_.background_rgb;
    out.cursor_rgb = screen_.cursor_rgb;
    out.palette = screen_.palette;
    out.cells.resize(static_cast<std::size_t>(out.size.columns) * out.size.rows);
    for (std::uint64_t row = 0; row < rows(); ++row) {
        const auto strip_row = top_ + row;
        if (strip_row >= archived_) {
            copy_row(screen_, strip_row - archived_, out, row);
            continue;
        }
        std::uint64_t first{};
        if (const auto* page = holding(strip_row, &first))
            copy_row(*page, strip_row - first, out, row);
    }
    out.history = {archived_ + rows(), top_, rows(), true};
    return out;
}

void HistoryStrip::forget() {
    const auto reach = kept_screens * std::max<std::uint64_t>(1, rows());
    const auto low = top_ > reach ? top_ - reach : 0;
    const auto high = top_ + rows() + reach;
    std::erase_if(pages_, [low, high](const auto& entry) {
        return entry.first + entry.second.size.rows <= low || entry.first >= high;
    });
}
} // namespace lapis::desktop
