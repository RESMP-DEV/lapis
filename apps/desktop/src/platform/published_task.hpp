#ifndef LAPIS_DESKTOP_PUBLISHED_TASK_HPP
#define LAPIS_DESKTOP_PUBLISHED_TASK_HPP

#include <atomic>
#include <exception>
#include <functional>
#include <utility>

namespace lapis::desktop::platform {
// One-shot callable publication through external Qt queues. Qt owns the
// callable's lifetime; release/acquire makes payload transfer visible in C++
// even when Qt's queue implementation is not instrumented. Moved-from and
// cancelled callables publish/acquire their state before destruction too.
class PublishedTask final {
  public:
    explicit PublishedTask(std::function<void()> task) : task_(std::move(task)) { publish(); }
    PublishedTask(const PublishedTask& other) {
        other.acquire();
        task_ = other.task_;
        publish();
    }
    PublishedTask(PublishedTask&& other) noexcept {
        other.acquire();
        task_ = std::move(other.task_);
        other.publish();
        publish();
    }
    PublishedTask& operator=(const PublishedTask&) = delete;
    PublishedTask& operator=(PublishedTask&&) = delete;
    ~PublishedTask() { acquire(); }
    void operator()() {
        acquire();
        auto task = std::move(task_);
        publish();
        task();
    }

  private:
    void publish() noexcept { published_.store(true, std::memory_order_release); }
    void acquire() const noexcept {
        if (!published_.load(std::memory_order_acquire))
            std::terminate();
    }
    std::atomic<bool> published_{false};
    std::function<void()> task_;
};
} // namespace lapis::desktop::platform
#endif
