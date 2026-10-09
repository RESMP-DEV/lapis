#ifndef LAPIS_SESSION_UPDATE_PACING_HPP
#define LAPIS_SESSION_UPDATE_PACING_HPP

#include <QtGlobal>

#include <algorithm>

namespace lapis::session::wire {

// Publish-to-publish pacing for coalesced service and view publishers.
//
// This is a rate limit, not a debounce. It is measured from the last processed
// update: once the interval has elapsed the newest state publishes at once,
// and a denial arms only the remaining time, so a burst is never taxed the
// whole interval after a quiet period. Waiting for quiet is a separate,
// deliberate feature and never belongs here.
class UpdatePace final {
  public:
    explicit UpdatePace(qint64 interval_ms) : interval_ms_(interval_ms) {}

    // How long to wait before publishing the newest state, given the last
    // publish. Zero means publish now. Callers arm only this remainder.
    [[nodiscard]] qint64 wait_ms(qint64 now_ms) const {
        if (interval_ms_ <= 0 || last_publish_ms_ < 0)
            return 0;
        // Clamp a backward clock to zero elapsed time. The service clock is
        // monotonic, but this helper must never return more than its interval
        // or otherwise misbehave for a test-injected clock.
        const qint64 since = std::max(qint64{0}, now_ms - last_publish_ms_);
        return since >= interval_ms_ ? 0 : interval_ms_ - since;
    }
    // The next deadline in absolute terms, for a timeout deadline that never
    // re-arms from the first event of a burst.
    [[nodiscard]] qint64 deadline_ms(qint64 now_ms) const {
        const qint64 wait = wait_ms(now_ms);
        return wait == 0 ? now_ms : last_publish_ms_ + interval_ms_;
    }
    void published(qint64 now_ms) { last_publish_ms_ = now_ms; }
    [[nodiscard]] qint64 last_publish_ms() const { return last_publish_ms_; }
    [[nodiscard]] qint64 interval_ms() const { return interval_ms_; }
    // A never-published stream has no last publish; the next update is the
    // first of a burst and publishes immediately.
    void reset() { last_publish_ms_ = -1; }

  private:
    qint64 interval_ms_;
    qint64 last_publish_ms_{-1};
};

} // namespace lapis::session::wire

#endif // LAPIS_SESSION_UPDATE_PACING_HPP
