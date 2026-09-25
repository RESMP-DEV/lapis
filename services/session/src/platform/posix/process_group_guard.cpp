#include "platform/posix/process_group_guard.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace lapis::session::posix {

namespace {
[[noreturn]] void guard_process(std::array<int, 2> retained, int descriptor_limit) {
    for (int descriptor = 0; descriptor < descriptor_limit; ++descriptor)
        if (descriptor != retained[0] && descriptor != retained[1])
            ::close(descriptor);
    char command{};
    for (;;) {
        const auto count = ::read(retained[0], &command, 1);
        if (count > 0 || (count < 0 && errno == EINTR))
            continue;
        break;
    }
    ::kill(0, SIGKILL);
    ::_exit(1);
}
} // namespace

// The detached guard holds group membership until the service closes its pipe.
// It signals its own group, so no saved/recycled PID is used after QProcess reaps
// the CLI leader. Double-fork and reap the intermediate before exec: the CLI must
// not inherit a hidden child that could interfere with wait()/SIGCHLD handling.
// Called only in QProcess's fork child; use async-signal-safe operations here.
bool start_group_guard(std::array<int, 2> control, int descriptor_limit, int completion) {
    sigset_t blocked{};
    sigset_t previous{};
    static_cast<void>(sigfillset(&blocked));
    const int block_result = ::pthread_sigmask(SIG_SETMASK, &blocked, &previous);
    if (block_result != 0) {
        errno = block_result;
        return false;
    }
    const pid_t intermediate = ::fork();
    if (intermediate == 0) {
        const pid_t guard = ::fork();
        if (guard != 0)
            ::_exit(guard < 0 ? 1 : 0);
        guard_process({control[0], completion}, descriptor_limit);
    }
    int status{};
    pid_t waited = -1;
    if (intermediate > 0) {
        do {
            waited = ::waitpid(intermediate, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    int failure_errno = 0;
    if (intermediate < 0 || waited < 0)
        failure_errno = errno;
    const int restore_result = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    const bool restored = restore_result == 0;
    if (!restored && failure_errno == 0)
        failure_errno = restore_result;
    ::close(control[0]);
    ::close(control[1]);
    if (completion >= 0)
        ::close(completion);
    const bool started = restored && waited == intermediate && intermediate > 0 &&
                         WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!started)
        errno = failure_errno != 0 ? failure_errno : EIO;
    return started;
}

// POSIX pipe read/write ends are an intentional ordered pair.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool open_guard_pipe(UniqueFd& read, UniqueFd& write) {
    std::array<int, 2> control{};
    if (::pipe(control.data()) != 0)
        return false;
    UniqueFd new_read(control[0]);
    UniqueFd new_write(control[1]);
    if (::fcntl(new_read.get(), F_SETFD, FD_CLOEXEC) != 0 ||
        ::fcntl(new_write.get(), F_SETFD, FD_CLOEXEC) != 0) {
        const int saved_errno = errno;
        new_read.reset();
        new_write.reset();
        errno = saved_errno;
        return false;
    }
    read = std::move(new_read);
    write = std::move(new_write);
    return true;
}

} // namespace lapis::session::posix
