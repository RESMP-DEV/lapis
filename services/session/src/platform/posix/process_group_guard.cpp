#include "platform/posix/process_group_guard.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lapis::session::posix {

// The detached guard holds group membership until the service closes its pipe.
// It signals its own group, so no saved/recycled PID is used after QProcess reaps
// the CLI leader. Double-fork and reap the intermediate before exec: the CLI must
// not inherit a hidden child that could interfere with wait()/SIGCHLD handling.
// Called only in QProcess's fork child; use async-signal-safe operations here.
bool start_group_guard(std::array<int, 2> control, int descriptor_limit) {
    sigset_t blocked{};
    sigset_t previous{};
    static_cast<void>(sigfillset(&blocked));
    if (::pthread_sigmask(SIG_SETMASK, &blocked, &previous) != 0)
        return false;
    const pid_t intermediate = ::fork();
    if (intermediate == 0) {
        const pid_t guard = ::fork();
        if (guard != 0)
            ::_exit(guard < 0 ? 1 : 0);
        for (int descriptor = 0; descriptor < descriptor_limit; ++descriptor)
            if (descriptor != control[0])
                ::close(descriptor);
        char command{};
        for (;;) {
            const auto count = ::read(control[0], &command, 1);
            if (count > 0 || (count < 0 && errno == EINTR))
                continue;
            break;
        }
        ::kill(0, SIGKILL);
        ::_exit(1);
    }
    int status{};
    pid_t waited = -1;
    if (intermediate > 0) {
        do {
            waited = ::waitpid(intermediate, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    const int saved_errno = errno;
    const bool restored = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0;
    ::close(control[0]);
    ::close(control[1]);
    errno = saved_errno != 0 ? saved_errno : EIO;
    return restored && waited == intermediate && intermediate > 0 && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

bool open_guard_pipe(UniqueFd& read, UniqueFd& write) {
    std::array<int, 2> control{};
    if (::pipe(control.data()) != 0)
        return false;
    read.reset(control[0]);
    write.reset(control[1]);
    return ::fcntl(read.get(), F_SETFD, FD_CLOEXEC) == 0 &&
           ::fcntl(write.get(), F_SETFD, FD_CLOEXEC) == 0;
}

} // namespace lapis::session::posix
