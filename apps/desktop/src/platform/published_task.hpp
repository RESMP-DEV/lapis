#ifndef LAPIS_DESKTOP_PUBLISHED_TASK_HPP
#define LAPIS_DESKTOP_PUBLISHED_TASK_HPP

#include <atomic>
#include <exception>
#include <functional>
#include <utility>

namespace lapis::desktop::platform {
// Callable storage publication through external Qt queues/connections. Qt owns
// the callable lifetime; release/acquire makes construction, moves and eventual
// destruction visible even when the Qt library itself is uninstrumented.
// This does not serialize calls or extend the lifetime of a captured raw pointer.
class PublishedCallback final {
  public:
    explicit PublishedCallback(std::function<void()> callback) : callback_(std::move(callback)) {
        publish();
    }
    PublishedCallback(const PublishedCallback& other) {
        other.acquire();
        callback_ = other.callback_;
        publish();
    }
    PublishedCallback(PublishedCallback&& other) noexcept {
        other.acquire();
        callback_ = std::move(other.callback_);
        other.publish();
        publish();
    }
    PublishedCallback& operator=(const PublishedCallback&) = delete;
    PublishedCallback& operator=(PublishedCallback&&) = delete;
    ~PublishedCallback() { acquire(); }
    void operator()() const {
        acquire();
        callback_();
    }

  private:
    friend class PublishedTask;
    std::function<void()> take() {
        acquire();
        auto callback = std::move(callback_);
        publish();
        return callback;
    }
    void publish() noexcept { published_.store(true, std::memory_order_release); }
    void acquire() const noexcept {
        if (!published_.load(std::memory_order_acquire))
            std::terminate();
    }
    std::atomic<bool> published_{false};
    std::function<void()> callback_;
};

// One-shot work consumes its callable. Repeated invocation is not supported;
// use PublishedCallback for a signal that intentionally fires more than once.
class PublishedTask final {
  public:
    explicit PublishedTask(std::function<void()> task) : callback_(std::move(task)) {}
    PublishedTask(const PublishedTask&) = default;
    PublishedTask(PublishedTask&&) noexcept = default;
    PublishedTask& operator=(const PublishedTask&) = delete;
    PublishedTask& operator=(PublishedTask&&) = delete;
    void operator()() {
        auto task = callback_.take();
        task();
    }

  private:
    PublishedCallback callback_;
};
} // namespace lapis::desktop::platform
#endif
