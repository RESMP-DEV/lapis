#ifndef LAPIS_SESSION_HISTORY_STORE_HPP
#define LAPIS_SESSION_HISTORY_STORE_HPP
#include <QString>
#include <QStringView>
#include <lapis/session/terminal.hpp>
#include <optional>
namespace lapis::session {
struct HistoryLimits {
    quint64 session_bytes{quint64{64} * 1024U * 1024U};
    quint64 global_bytes{quint64{256} * 1024U * 1024U};
    quint32 max_pages{4096};
};
struct HistoryPage {
    quint64 id{};
    TerminalSnapshot snapshot;
};
struct HistoryStats {
    quint64 session_bytes{};
    quint64 global_bytes{};
    quint64 pages{};
};
// Blocking filesystem API: use on the dedicated history I/O worker only.
// Quotas count committed page-file bytes; metadata and atomic-write overhead
// are separately bounded by the page count and maximum single-record size.
class HistoryStore {
  public:
    HistoryStore(const QString& root, QStringView session_id, const HistoryLimits& limits = {});
    [[nodiscard]] quint64 append(const TerminalSnapshot& page);
    [[nodiscard]] std::optional<HistoryPage> older(quint64 before = 0);
    [[nodiscard]] std::optional<HistoryPage> newer(quint64 after);
    void clear();
    [[nodiscard]] HistoryStats stats();

  private:
    QString root_;
    QString session_id_;
    QString directory_;
    HistoryLimits limits_;
};
} // namespace lapis::session
#endif
