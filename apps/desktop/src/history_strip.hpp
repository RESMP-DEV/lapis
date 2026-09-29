#ifndef LAPIS_DESKTOP_HISTORY_STRIP_HPP
#define LAPIS_DESKTOP_HISTORY_STRIP_HPP
#include <cstdint>
#include <lapis/session/terminal.hpp>
#include <map>
#include <optional>
namespace lapis::desktop {
// Kept history as one strip, as other terminals scroll it: the rows archived
// before browsing began (row 0 the oldest kept), then the screen as it was
// then. The view is a whole screen of the strip from `top`, so it scrolls by
// rows however the service cut its pages. Pages are fetched as the view needs
// them; those near it are kept.
class HistoryStrip {
  public:
    HistoryStrip(session::TerminalSnapshot screen, std::uint64_t archived);
    // Keeps a page placed by its history fields (viewport_offset its first row).
    // The page holding the newest kept rows also settles the seam: kept rows
    // the screen still shows at its top (a terminal that grew brings kept rows
    // back, and output can land between the screen and the archive) belong to
    // the screen.
    void addPage(session::TerminalSnapshot page);
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
    // The view in the screen's size and colors; rows no page holds are blank.
    [[nodiscard]] session::TerminalSnapshot view() const;

  private:
    [[nodiscard]] const session::TerminalSnapshot* holding(std::uint64_t row,
                                                           std::uint64_t* first) const;
    void forget();
    void settleSeam();
    session::TerminalSnapshot screen_;
    std::uint64_t archived_{};
    std::uint64_t top_{};
    bool seam_settled_{};
    std::optional<std::uint64_t> seam_missing_;
    std::map<std::uint64_t, session::TerminalSnapshot> pages_; // by first row
};
} // namespace lapis::desktop
#endif
