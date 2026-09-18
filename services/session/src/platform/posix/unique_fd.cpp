#include "platform/posix/unique_fd.hpp"

#include <unistd.h>
#include <utility>

namespace lapis::session::posix {

UniqueFd::UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}

UniqueFd::~UniqueFd() noexcept { reset(); }

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : descriptor_(other.release()) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFd::get() const noexcept { return descriptor_; }

UniqueFd::operator bool() const noexcept { return descriptor_ >= 0; }

int UniqueFd::release() noexcept { return std::exchange(descriptor_, -1); }

void UniqueFd::reset(int descriptor) noexcept {
    if (descriptor_ == descriptor) {
        return;
    }
    const int previous = std::exchange(descriptor_, descriptor);
    if (previous >= 0) {
        // Do not retry close(): another thread may already have reused the number.
        // Destruction cannot report close errors; protocols must finish I/O first.
        static_cast<void>(::close(previous));
    }
}

} // namespace lapis::session::posix
