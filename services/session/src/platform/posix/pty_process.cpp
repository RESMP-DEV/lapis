#include "pty_process.hpp"

#include "platform/posix/process_group_guard.hpp"

#include <QProcessEnvironment>
#include <QTimer>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace lapis::session::posix {
namespace {
constexpr qsizetype queue_limit = qsizetype{1024} * 1024;
QString system_error(const char* operation) {
    return QString::fromLatin1(operation) + QStringLiteral(": ") +
           QString::fromStdString(std::error_code(errno, std::generic_category()).message());
}
int resolve_pty_name(int descriptor, std::array<char, 128>& name) {
#ifdef __APPLE__
    return ::ioctl(descriptor, TIOCPTYGNAME, name.data());
#else
    const int status = ::ptsname_r(descriptor, name.data(), name.size());
    if (status != 0)
        errno = status;
    return status;
#endif
}
} // namespace
PtyProcess::PtyProcess(QObject* parent) : QObject(parent) {
    connect(&process_, &QProcess::started, this, [this] {
        slave_.reset();
        guard_read_.reset();
        reader_->setEnabled(!output_paused_ && !final_leader_exited_);
        emit started();
        writeReady();
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        guard_control_.reset();
        if (reader_)
            reader_->setEnabled(false);
        if (!final_leader_exited_)
            recordLeaderExit(code, status);
        if (!final_drain_active_) {
            final_drain_active_ = true;
            final_drain_clock_.start();
        }
        finishWhenDrained(code, status, 256);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        if (reader_)
            reader_->setEnabled(false);
        if (writer_)
            writer_->setEnabled(false);
        clearPendingWrite();
        guard_control_.reset();
        guard_read_.reset();
        reader_.reset();
        writer_.reset();
        master_.reset();
        slave_.reset();
        emit failure(process_.errorString());
    });
}
PtyProcess::~PtyProcess() {
    disconnect(&process_, nullptr, this, nullptr);
    reader_.reset();
    writer_.reset();
    guard_control_.reset();
    guard_read_.reset();
    // This is explicit service teardown. Signal the verified, still-owned
    // process group before reaping its leader; never reuse a saved PID afterward.
    if (process_.state() != QProcess::NotRunning && !terminateProcessGroup())
        process_.kill();
    master_.reset();
    slave_.reset();
    if (process_.state() != QProcess::NotRunning && !process_.waitForFinished(200)) {
        process_.kill();
        static_cast<void>(process_.waitForFinished(200));
    }
}
void PtyProcess::start(const PtyLaunch& launch) {
    if (process_.state() != QProcess::NotRunning || master_) {
        emit failure(QStringLiteral("PTY already started"));
        return;
    }
    PtyLaunch validated_launch;
    try {
        validated_launch = validate_launch(launch);
    } catch (const std::invalid_argument& error) {
        emit failure(QString::fromUtf8(error.what()));
        return;
    }
    master_.reset(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (!master_ || ::grantpt(master_.get()) != 0 || ::unlockpt(master_.get()) != 0) {
        emit failure(system_error("Open PTY"));
        master_.reset();
        return;
    }
    std::array<char, 128> name{};
    const int name_status = resolve_pty_name(master_.get(), name);
    if (name_status != 0) {
        emit failure(system_error("Resolve PTY"));
        master_.reset();
        return;
    }
    slave_.reset(::open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC));
    const int flags = ::fcntl(master_.get(), F_GETFL);
    if (!slave_ || flags < 0 ||
        ::fcntl(master_.get(), F_SETFL,
                static_cast<int>(static_cast<unsigned int>(flags) |
                                 static_cast<unsigned int>(O_NONBLOCK))) < 0 ||
        !resize(validated_launch.size)) {
        emit failure(system_error("Configure PTY"));
        slave_.reset();
        master_.reset();
        return;
    }
    const int descriptor_limit = ::getdtablesize();
    if (descriptor_limit <= 0 || !open_guard_pipe(guard_read_, guard_control_)) {
        emit failure(system_error("Create process-group guard pipe"));
        guard_read_.reset();
        guard_control_.reset();
        slave_.reset();
        master_.reset();
        return;
    }
    reader_ = std::make_unique<QSocketNotifier>(master_.get(), QSocketNotifier::Read);
    writer_ = std::make_unique<QSocketNotifier>(master_.get(), QSocketNotifier::Write);
    reader_->setEnabled(false);
    writer_->setEnabled(false);
    connect(reader_.get(), &QSocketNotifier::activated, this,
            [this] { static_cast<void>(readReady()); });
    connect(writer_.get(), &QSocketNotifier::activated, this, [this] { writeReady(); });
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(validated_launch.directory);
    process_.setProgram(validated_launch.program);
    process_.setArguments(validated_launch.arguments);
    const int master = master_.get();
    const int slave = slave_.get();
    const int guard_read = guard_read_.get();
    const int guard_control = guard_control_.get();
    // No UseVFork: the modifier forks a guard and must have its own address space.
    process_.setUnixProcessParameters(QProcess::UnixProcessParameters{});
    process_.setChildProcessModifier(
        [this, master, slave, guard_read, guard_control, descriptor_limit] {
            if (::setsid() < 0)
                process_.failChildProcessModifier("setsid", errno);
            if (!start_group_guard({guard_read, guard_control}, descriptor_limit))
                process_.failChildProcessModifier("process-group guard", errno);
            if (::ioctl(slave, TIOCSCTTY, 0) < 0)
                process_.failChildProcessModifier("TIOCSCTTY", errno);
            for (int target = 0; target < 3; ++target)
                if (::dup2(slave, target) < 0)
                    process_.failChildProcessModifier("dup2", errno);
            if (master > 2)
                ::close(master);
            if (slave > 2)
                ::close(slave);
        });
    process_.start();
}
bool PtyProcess::resize(TerminalSize size) {
    if (!master_ || size.columns == 0 || size.rows == 0)
        return false;
    winsize dimensions{};
    dimensions.ws_col = size.columns;
    dimensions.ws_row = size.rows;
    return ::ioctl(master_.get(), TIOCSWINSZ, &dimensions) == 0;
}
bool PtyProcess::writeBytes(const QByteArray& bytes) {
    if (!master_ || process_.state() == QProcess::NotRunning)
        return false;
    if (bytes.size() > queue_limit - (pending_write_.size() - write_offset_))
        return false;
    if (write_offset_ != 0) {
        pending_write_.remove(0, write_offset_);
        write_offset_ = 0;
    }
    pending_write_ += bytes;
    if (process_.state() == QProcess::Running)
        writeReady();
    return true;
}
bool PtyProcess::terminateProcessGroup() { return signalProcessGroup(SIGKILL); }
// Resolve the leader from the live QProcess each time: once it is reaped,
// processId() is zero, so a later escalation cannot reach a reused PID.
bool PtyProcess::signalProcessGroup(int signal) {
    if (process_.state() == QProcess::NotRunning)
        return false;
    const qint64 process_id = process_.processId();
    if (process_id <= 0 || process_id > std::numeric_limits<pid_t>::max())
        return false;
    const pid_t child = static_cast<pid_t>(process_id);
    if (::getsid(child) != child)
        return false;
    return ::kill(-child, signal) == 0;
}
bool PtyProcess::hangup() {
    if (!signalProcessGroup(SIGHUP))
        return false;
    constexpr int terminate_after_ms = 1500;
    constexpr int kill_after_ms = 3000;
    QTimer::singleShot(terminate_after_ms, this,
                       [this] { static_cast<void>(signalProcessGroup(SIGTERM)); });
    QTimer::singleShot(kill_after_ms, this,
                       [this] { static_cast<void>(signalProcessGroup(SIGKILL)); });
    return true;
}
void PtyProcess::clearPendingWrite() {
    pending_write_.clear();
    write_offset_ = 0;
}
void PtyProcess::writeReady() {
    if (!master_)
        return;
    while (write_offset_ < pending_write_.size()) {
        const auto count = ::write(master_.get(), pending_write_.constData() + write_offset_,
                                   static_cast<std::size_t>(pending_write_.size() - write_offset_));
        if (count > 0) {
            write_offset_ += count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            writer_->setEnabled(true);
            return;
        }
        writer_->setEnabled(false);
        if (count < 0 && errno == EIO) {
            clearPendingWrite();
            return;
        }
        emit failure(system_error("PTY write"));
        return;
    }
    pending_write_.clear();
    write_offset_ = 0;
    writer_->setEnabled(false);
}
void PtyProcess::pauseOutput(bool paused) {
    output_paused_ = paused;
    if (reader_)
        reader_->setEnabled(!paused);
    if (!paused) {
        // Once the leader is known to have exited, output is in final drain.
        // Resuming the sink lets already-emitted bytes drain, but it must not
        // restart the bounded final-drain deadline for a wedge.
        if (!final_leader_exited_)
            final_drain_active_ = false;
        return;
    }
    schedulePausedLeaderWatchdog();
}

void PtyProcess::recordLeaderExit(int exit_code, QProcess::ExitStatus exit_status) {
    final_leader_exited_ = true;
    leader_exit_code_ = exit_code;
    leader_exit_status_ = exit_status;
}

void PtyProcess::schedulePausedLeaderWatchdog() {
    if (!output_paused_ || final_leader_exited_ || paused_leader_watchdog_ ||
        process_.state() != QProcess::Running)
        return;
    paused_leader_watchdog_ = true;
    QTimer::singleShot(20, this, [this] {
        paused_leader_watchdog_ = false;
        checkPausedLeader();
    });
}

// QProcess delays `finished` until every inherited PTY writer closes. A paused
// reader must therefore probe the owned leader independently. macOS waitid
// WNOWAIT returned success with si_code=0 before and after exit in this exact
// fixture; waitpid is authoritative. Consuming here is intentional because
// this class owns the final status and suppresses its later duplicate finish.
bool PtyProcess::leaderExitStatus(int& exit_code, QProcess::ExitStatus& exit_status) {
    if (final_leader_exited_ || process_.state() != QProcess::Running)
        return final_leader_exited_;
    const auto process_id = process_.processId();
    if (process_id <= 0) {
        recordLeaderExit(process_.exitCode(), process_.exitStatus());
        exit_code = leader_exit_code_;
        exit_status = leader_exit_status_;
        return true;
    }
    const auto child = static_cast<pid_t>(process_id);
    int wait_status = 0;
    const auto waited = ::waitpid(child, &wait_status, WNOHANG);
    if (waited == child) {
        if (WIFEXITED(wait_status))
            recordLeaderExit(WEXITSTATUS(wait_status), QProcess::NormalExit);
        else if (WIFSIGNALED(wait_status))
            recordLeaderExit(WTERMSIG(wait_status), QProcess::CrashExit);
        else
            return false;
    } else {
        // A waiting failure (ECHILD: QProcess may have reaped the child but not
        // yet published `finished` while PTY writers remain open) or any other
        // unresolved state leaves exit ownership unknown, so this poll defers.
        return false;
    }
    exit_code = leader_exit_code_;
    exit_status = leader_exit_status_;
    return final_leader_exited_;
}

void PtyProcess::checkPausedLeader() {
    paused_leader_watchdog_ = false;
    if (!output_paused_ || final_drain_emitted_ || process_.state() != QProcess::Running)
        return;
    int exit_code = 0;
    QProcess::ExitStatus exit_status = QProcess::NormalExit;
    if (!leaderExitStatus(exit_code, exit_status)) {
        schedulePausedLeaderWatchdog();
        return;
    }
    if (!final_drain_active_) {
        final_drain_active_ = true;
        final_drain_clock_.start();
    }
    QTimer::singleShot(10, this,
                       [this] { finishWhenDrained(leader_exit_code_, leader_exit_status_, 256); });
}
bool PtyProcess::readReady() {
    if (!master_)
        return true;
    QByteArray bytes;
    bytes.reserve(max_read_batch);
    bool drained = false;
    while (bytes.size() < max_read_batch && !output_paused_) {
        std::array<char, 16384> chunk{};
        const auto wanted = std::min(static_cast<std::size_t>(chunk.size()),
                                     static_cast<std::size_t>(max_read_batch - bytes.size()));
        const auto count = ::read(master_.get(), chunk.data(), wanted);
        if (count > 0) {
            bytes.append(chunk.data(), static_cast<qsizetype>(count));
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            drained = true;
            break;
        }
        reader_->setEnabled(false);
        if (count < 0 && errno != EIO)
            emit failure(system_error("PTY read"));
        if (!bytes.isEmpty())
            emit output(bytes);
        return true;
    }
    if (!bytes.isEmpty())
        emit output(bytes);
    // A quota-full batch is not evidence of a drained PTY. It may have stopped
    // only because this reader reached its admission quantum, and a consumer
    // can also pause synchronously while receiving the emitted bytes.
    return drained && !output_paused_;
}
void PtyProcess::finishWhenDrained(int exit_code, QProcess::ExitStatus exit_status,
                                   int drain_budget) {
    // The existing byte budget bounds a sustained producer. A sink paused for
    // backpressure cannot be allowed to suspend teardown forever, so the wall
    // clock is the second, independent stuck-drain bound.
    constexpr int drain_deadline_ms = 3000;
    const bool deadline_reached =
        final_drain_active_ && final_drain_clock_.elapsed() >= drain_deadline_ms;
    if (output_paused_ && !final_drain_closed_) {
        if (deadline_reached) {
            qWarning("PTY final output sink stayed paused for 3 seconds; "
                     "closing the terminal");
            if (reader_)
                reader_->setEnabled(false);
            if (writer_)
                writer_->setEnabled(false);
            master_.reset();
            slave_.reset();
            clearPendingWrite();
            final_drain_closed_ = true;
        } else {
            QTimer::singleShot(10, this, [this, exit_code, exit_status, drain_budget] {
                finishWhenDrained(exit_code, exit_status, drain_budget);
            });
            return;
        }
    }
    if (!readReady()) {
        if (drain_budget > 1 && !deadline_reached) {
            QTimer::singleShot(0, this, [this, exit_code, exit_status, drain_budget] {
                finishWhenDrained(exit_code, exit_status, drain_budget - 1);
            });
            return;
        }
        // QProcess has already reaped the leader. Its numeric PID is no longer
        // an owned signaling target; bound the tail and close the terminal.
        if (drain_budget <= 1) {
            qWarning("PTY final output exceeded the 16 MiB drain budget; "
                     "closing the terminal");
        } else {
            qWarning("PTY final output remained readable for 3 seconds; "
                     "closing the terminal");
        }
    }
    if (reader_)
        reader_->setEnabled(false);
    if (writer_)
        writer_->setEnabled(false);
    master_.reset();
    slave_.reset();
    clearPendingWrite();
    guard_control_.reset(); // Bound guarded descendants even if QProcess is still draining.
    if (final_drain_emitted_)
        return;
    final_drain_emitted_ = true;
    emit finished(exit_code, exit_status);
}
} // namespace lapis::session::posix
