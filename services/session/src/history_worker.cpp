#include "history_worker.hpp"
#include <QPromise>
#include <exception>
#include <utility>

namespace lapis::session {
namespace {
constexpr std::size_t max_queue_bytes = std::size_t{16} * 1024U * 1024U;
constexpr std::size_t max_queue_pages = 128;
std::size_t page_bytes(const TerminalSnapshot& page) {
    return page.cells.size() * sizeof(TerminalCell) + page.graphemes.size() * sizeof(char32_t) +
           sizeof(TerminalSnapshot);
}
} // namespace
struct HistoryWorker::State {
    QString root;
    QString session_id;
    HistoryLimits limits;
    std::unique_ptr<HistoryStore> store;
};
HistoryWorker::HistoryWorker(QString root, QString session_id, HistoryLimits limits)
    : state_(std::make_shared<State>(std::move(root), std::move(session_id), limits, nullptr)) {
    pool_.setMaxThreadCount(1);
    drain_timer_.setSingleShot(true);
    connect(&drain_timer_, &QTimer::timeout, this, [this] { finishDrain(); });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        auto result = watcher_.result();
        auto operation = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= operation.bytes;
        active_ = false;
        if (!result.error.isEmpty() && failure)
            failure(result.error);
        if (!operation.page && received) {
            wire::HistoryReply reply{
                operation.attachment, operation.request.request_id, 0, result.error, {}};
            if (result.page) {
                reply.page_id = result.page->id;
                reply.snapshot = std::move(result.page->snapshot);
            } else if (reply.message.isEmpty())
                reply.message = QStringLiteral("No more archived history");
            received(std::move(reply));
        }
        if (progress)
            progress();
        startNext();
        if (draining_ && idle())
            finishDrain();
    });
}
HistoryWorker::~HistoryWorker() {
    disconnect(&watcher_, nullptr, this, nullptr);
    pool_.waitForDone();
}
bool HistoryWorker::append(std::vector<TerminalSnapshot> pages) {
    if (draining_)
        return false;
    std::size_t bytes{};
    for (const auto& page : pages)
        bytes += page_bytes(page);
    if (pages.size() + queue_.size() > max_queue_pages || bytes > max_queue_bytes - queued_bytes_)
        return false;
    for (auto& page : pages) {
        const auto size = page_bytes(page);
        queue_.push_back({std::move(page), {}, {}, size});
    }
    queued_bytes_ += bytes;
    startNext();
    return true;
}
bool HistoryWorker::read(wire::Attachment attachment, wire::HistoryRequest request) {
    if (draining_ || queue_.size() >= max_queue_pages)
        return false;
    queue_.push_back({{}, std::move(attachment), request, 0});
    startNext();
    return true;
}
void HistoryWorker::drain(std::function<void()> complete, int timeout_ms) {
    if (draining_)
        return;
    draining_ = true;
    drain_complete_ = std::move(complete);
    if (idle())
        finishDrain();
    else
        drain_timer_.start(timeout_ms);
}
void HistoryWorker::finishDrain() {
    drain_timer_.stop();
    if (drain_complete_) {
        auto complete = std::move(drain_complete_);
        complete();
    }
}
void HistoryWorker::startNext() {
    if (active_ || queue_.empty())
        return;
    active_ = true;
    QPromise<Result> promise;
    watcher_.setFuture(promise.future());
    // Only this one operation accesses State until its future completes.
    Operation operation = queue_.front();
    pool_.start([state = state_, operation = std::move(operation),
                 promise = std::move(promise)]() mutable {
        promise.start();
        Result result;
        try {
            if (!state->store)
                state->store =
                    std::make_unique<HistoryStore>(state->root, state->session_id, state->limits);
            if (operation.page)
                static_cast<void>(state->store->append(*operation.page));
            else if (operation.request.direction == wire::HistoryDirection::older)
                result.page = state->store->older(operation.request.reference);
            else
                result.page = state->store->newer(operation.request.reference);
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        }
        promise.addResult(std::move(result));
        promise.finish();
    });
}
} // namespace lapis::session
