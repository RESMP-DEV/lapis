#ifndef LAPIS_SESSION_PLATFORM_POSIX_PROCESS_GROUP_GUARD_HPP
#define LAPIS_SESSION_PLATFORM_POSIX_PROCESS_GROUP_GUARD_HPP

#include "platform/posix/unique_fd.hpp"

#include <array>

namespace lapis::session::posix {

// Internal to the service's POSIX backends. The caller invokes this only in a
// fork child before exec, and only with async-signal-safe operations. The
// caller remains responsible for creating the session and process group with
// setsid() before spawning the guard.
//
// The detached guard remains in that group until every write end of control is
// closed. Ownership EOF (including abrupt parent death) makes it kill the
// guarded process group. In the parent, keep only the write end alive for as
// long as guarded descendants may run; close it when their leader is reaped.
// In the fork child, start_group_guard consumes both supplied descriptors and
// closes them after spawning; the caller must _exit on failure. The parent
// retains its original descriptors.
// Optional completion is a pipe writer retained only by the guard. Its reader
// observes EOF after the guard signals the group and exits; the caller closes
// its parent-side writer after successful startup (or startup failure).
bool start_group_guard(std::array<int, 2> control, int descriptor_limit, int completion = -1);

// Replaces the descriptors owned by read and write with the two ends of a new
// CLOEXEC pipe. On failure, errno is preserved from the failed operation and
// both references remain unchanged.
bool open_guard_pipe(UniqueFd& read, UniqueFd& write);

} // namespace lapis::session::posix

#endif // LAPIS_SESSION_PLATFORM_POSIX_PROCESS_GROUP_GUARD_HPP
