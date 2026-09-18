#ifndef LAPIS_SESSION_HISTORY_WORKER_HPP
#define LAPIS_SESSION_HISTORY_WORKER_HPP
#include "history_store.hpp"
#include "transport/local_protocol.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <QThreadPool>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace lapis::session {
// Service-thread queue; filesystem operations run on exactly one I/O thread.
class HistoryWorker final : public QObject {
  public:
    HistoryWorker(QString root, QString session_id, HistoryLimits limits);
    ~HistoryWorker() override;
    [[nodiscard]] bool append(std::vector<TerminalSnapshot> pages);
    [[nodiscard]] bool read(wire::Attachment attachment, wire::HistoryRequest request);
    [[nodiscard]] bool idle() const { return queue_.empty() && !active_; }
    std::function<void()> progress;
    std::function<void(const QString&)> failure;
    std::function<void(wire::HistoryReply)> received;

  private:
    struct State;
    struct Operation {
        std::optional<TerminalSnapshot> page;
        wire::Attachment attachment;
        wire::HistoryRequest request;
        std::size_t bytes{};
    };
    struct Result {
        QString error;
        std::optional<HistoryPage> page;
    };
    void startNext();
    std::shared_ptr<State> state_;
    QThreadPool pool_;
    QFutureWatcher<Result> watcher_;
    std::deque<Operation> queue_;
    std::size_t queued_bytes_{};
    bool active_{};
};
} // namespace lapis::session
#endif
