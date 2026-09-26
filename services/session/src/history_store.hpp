#ifndef LAPIS_SESSION_HISTORY_STORE_HPP
#define LAPIS_SESSION_HISTORY_STORE_HPP
#include <QString>
#include <QStringView>
#include <deque>
#include <lapis/session/terminal.hpp>
#include <optional>
#include <vector>
namespace lapis::session {
// Compressed page bytes on disk. A page of terminal text compresses to a few
// kilobytes, so these keep a session back to its start in practice.
struct HistoryLimits {
    quint64 session_bytes{quint64{1} * 1024U * 1024U * 1024U};
    quint64 global_bytes{quint64{8} * 1024U * 1024U * 1024U};
};
struct HistoryPage {
    quint64 id{};
    // Its history fields place it: total_rows is every kept row,
    // viewport_offset this page's first row among them (0 is the oldest kept
    // row), viewport_rows its own rows.
    TerminalSnapshot snapshot;
};
struct HistoryStats {
    quint64 session_bytes{};
    quint64 global_bytes{};
    quint64 pages{};
};
// A session's archived pages, oldest first, compressed into append-only
// segment files under <root>/<session>/, with an index in memory so any row
// is one read away. Pages from before this format (*.page) are counted and
// evicted but not read.
//
// Blocking filesystem API: use on the dedicated history I/O worker only.
// Quotas count committed segment bytes. The newest segment of each session is
// never evicted by another session, so a writer's open segment stays whole.
class HistoryStore {
  public:
    HistoryStore(const QString& root, QStringView session_id, const HistoryLimits& limits = {});
    [[nodiscard]] quint64 append(const TerminalSnapshot& page);
    [[nodiscard]] std::optional<HistoryPage> older(quint64 before = 0);
    [[nodiscard]] std::optional<HistoryPage> newer(quint64 after);
    // The page holding `row`, counted from the oldest kept row; past the end,
    // the newest page.
    [[nodiscard]] std::optional<HistoryPage> at(quint64 row);
    void clear();
    [[nodiscard]] HistoryStats stats();

    struct Record {
        quint64 segment{}; // the segment's first page ID, its file name
        quint64 offset{};
        quint64 length{};
        quint64 id{};
        quint64 first{}; // rows written before it, over the session's life
        quint64 rows{};
    };

  private:
    void load();
    [[nodiscard]] QString segmentPath(quint64 first_id) const;
    [[nodiscard]] HistoryPage read(const Record& record);
    // Reads the selected record; if another session evicted its segment,
    // reloads the index and selects again.
    template <typename Select> std::optional<HistoryPage> readSelected(Select select);
    void enforceBudgets();
    QString root_;
    QString session_id_;
    QString directory_;
    HistoryLimits limits_;
    std::vector<Record> records_;
    std::deque<quint64> segments_; // first page IDs, oldest first
    quint64 next_id_{1};
};
} // namespace lapis::session
#endif
