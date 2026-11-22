#ifndef LAPIS_DESKTOP_HISTORY_STRIP_HPP
#define LAPIS_DESKTOP_HISTORY_STRIP_HPP
#include <cstdint>
#include <lapis/session/terminal.hpp>
#include <map>
#include <optional>
#include <vector>
namespace lapis::desktop {
// Kept history as one strip, as other terminals scroll it: the rows archived
// before browsing began (row 0 the oldest kept), then the screen as it was
// then. The view is a whole screen of the strip from `top`, so it scrolls by
// rows however the service cut its pages. Pages are fetched as the view needs
// them, and those around it ahead of need; up to `kept_screens` screens of
// pages stay in RAM, the furthest from the view dropped first past that.
class HistoryStrip {
  public:
    static constexpr std::uint64_t kept_screens = 64;
    // A kept page and the service's ID for it (zero when it gave none).
    struct Page {
        std::uint64_t id{};
        session::TerminalSnapshot snapshot; // placed by its history fields
    };
    HistoryStrip(session::TerminalSnapshot screen, std::uint64_t archived);
    // Keeps a page placed by its history fields (viewport_offset its first row).
    // The page holding the newest kept rows also settles the seam: kept rows
    // the screen still shows at its top (a terminal that grew brings kept rows
    // back, and output can land between the screen and the archive) belong to
    // the screen.
    void addPage(session::TerminalSnapshot page, std::uint64_t id = 0);
    // A page from before the oldest kept one, from a service that does not
    // place its pages: every row moves down by its rows.
    void prependPage(session::TerminalSnapshot page);
    // Moves the view, clamped to the strip; `archived()` shows the screen.
    void moveTo(std::int64_t top);
    [[nodiscard]] std::uint64_t top() const { return top_; }
    [[nodiscard]] std::uint64_t archived() const { return archived_; }
    [[nodiscard]] std::uint64_t rows() const { return screen_.size.rows; }
    // The first archived row in view that no kept page holds.
    [[nodiscard]] std::optional<std::uint64_t> missing() const;
    // With the view whole, the nearest archived row no kept page holds within
    // `above` screens over the view or `below` screens under it, nearer
    // screens first and the one above before the one below.
    [[nodiscard]] std::optional<std::uint64_t> ahead(std::uint64_t above,
                                                     std::uint64_t below) const;
    // Pages kept and the rows they hold.
    [[nodiscard]] std::size_t pages() const { return pages_.size(); }
    [[nodiscard]] std::uint64_t keptRows() const;
    // The pages the service named, at most `screens` screens of them nearest
    // the view, placed as kept; the strip keeps none after.
    [[nodiscard]] std::vector<Page> takePages(std::uint64_t screens);
    // The view in the screen's size and colors; rows no page holds are blank.
    [[nodiscard]] session::TerminalSnapshot view() const;
    // The one row just above the view, drawn above it while a trackpad has
    // scrolled part of a row; none at the oldest row.
    [[nodiscard]] std::optional<session::TerminalSnapshot> rowAbove() const;

  private:
    [[nodiscard]] session::TerminalSnapshot rows_from(std::uint64_t first_row,
                                                      std::uint16_t count) const;
    [[nodiscard]] const session::TerminalSnapshot* holding(std::uint64_t row,
                                                           std::uint64_t* first) const;
    [[nodiscard]] std::optional<std::uint64_t> firstMissing(std::uint64_t from,
                                                            std::uint64_t to) const;
    // Drops the pages furthest from the view until at most `screens` screens
    // of rows stay; pages the view shows always stay.
    void forget(std::uint64_t screens = kept_screens);
    void settleSeam();
    session::TerminalSnapshot screen_;
    std::uint64_t archived_{};
    std::uint64_t top_{};
    bool seam_settled_{};
    std::optional<std::uint64_t> seam_missing_;
    std::map<std::uint64_t, Page> pages_; // by first row
};
} // namespace lapis::desktop
#endif
