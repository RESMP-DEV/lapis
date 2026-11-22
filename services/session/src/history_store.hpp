#ifndef LAPIS_SESSION_HISTORY_STORE_HPP
#define LAPIS_SESSION_HISTORY_STORE_HPP
#include <QByteArray>
#include <QString>
#include <QStringView>
#include <deque>
#include <lapis/session/terminal.hpp>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>
namespace lapis::session {
// Compressed page bytes on disk. A page of terminal text compresses to a few
// kilobytes, so these keep a session back to its start in practice.
struct HistoryLimits {
    quint64 session_bytes{quint64{1} * 1024U * 1024U * 1024U};
    quint64 global_bytes{quint64{8} * 1024U * 1024U * 1024U};
    // Compressed pages this store keeps in RAM, the least recently used
    // dropped first; zero keeps none.
    quint64 cache_bytes{quint64{64} * 1024U * 1024U};
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
// The whole archive is scanned for the global budget only when this session is
// over its own, starts a segment, or has written a slice of the global budget
// since the last scan, not on every append.
//
// Pages this store appended or verified are kept compressed in RAM. A kept
// page is read without the archive lock, a file read or a digest, as long as
// its segment file is unchanged since (same inode, size and change time, one
// lstat); anything else takes the locked, verified disk path.
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
    // Pages and compressed bytes kept in RAM.
    [[nodiscard]] std::size_t cachedPages() const { return cache_.size(); }
    [[nodiscard]] quint64 cachedBytes() const { return cache_bytes_; }
    // Reads served from RAM and from disk since construction.
    [[nodiscard]] quint64 cacheHits() const { return cache_hits_; }
    [[nodiscard]] quint64 diskReads() const { return disk_reads_; }
    // Whole-archive scans for the budgets since construction.
    [[nodiscard]] quint64 budgetScans() const { return budget_scans_; }

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
    void enforceBudgets(bool new_segment);
    // A segment file as last seen by this store; a kept page is trusted only
    // while its segment still matches.
    struct Stamp {
        quint64 device{};
        quint64 inode{};
        quint64 size{};
        qint64 changed_s{};
        qint64 changed_ns{};
        bool operator==(const Stamp&) const = default;
    };
    struct Cached {
        QByteArray payload;
        quint64 segment{};
        std::list<quint64>::iterator use;
    };
    [[nodiscard]] HistoryPage decode(const Record& record, const QByteArray& payload) const;
    [[nodiscard]] std::optional<HistoryPage> cachedRead(const Record& record);
    void keep(const Record& record, QByteArray payload);
    void dropCached(quint64 id);
    void dropSegment(quint64 segment);
    void dropCache();
    // Sets the segment's stamp; a change from the one known drops its pages.
    void restamp(quint64 segment, const Stamp& stamp);
    QString root_;
    QString session_id_;
    QString directory_;
    HistoryLimits limits_;
    std::vector<Record> records_;
    std::deque<quint64> segments_; // first page IDs, oldest first
    quint64 next_id_{1};
    // Own committed bytes as counted here: exact after a scan or load, then
    // advanced by appends. Another session's eviction only lowers the truth.
    quint64 session_bytes_{};
    quint64 written_since_scan_{};
    std::unordered_map<quint64, Cached> cache_; // by page ID
    std::list<quint64> cache_use_;              // most recently used first
    std::unordered_map<quint64, Stamp> stamps_; // by segment
    quint64 cache_bytes_{};
    quint64 cache_hits_{};
    quint64 disk_reads_{};
    quint64 budget_scans_{};
};
} // namespace lapis::session
#endif
