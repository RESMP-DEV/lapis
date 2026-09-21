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
#include <system_error>
#include <unistd.h>

namespace lapis::session::posix {
namespace {
constexpr qsizetype queue_limit = qsizetype{1024} * 1024;
QString system_error(const char* operation) {
    return QString::fromLatin1(operation) + QStringLiteral(": ") +
           QString::fromStdString(std::error_code(errno, std::generic_category()).message());
}
} // namespace
PtyProcess::PtyProcess(QObject* parent) : QObject(parent) {
    connect(&process_, &QProcess::started, this, [this] {
        slave_.reset();
        guard_read_.reset();
        reader_->setEnabled(!output_paused_);
        emit started();
        writeReady();
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        guard_control_.reset();
        if (reader_)
            reader_->setEnabled(false);
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
#ifdef __APPLE__
    const int name_status = ::ioctl(master_.get(), TIOCPTYGNAME, name.data());
#else
    const int name_status = ::ptsname_r(master_.get(), name.data(), name.size());
    if (name_status != 0)
        errno = name_status;
#endif
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
bool PtyProcess::terminateProcessGroup() {
    const qint64 process_id = process_.processId();
    if (process_id <= 0 || process_id > std::numeric_limits<pid_t>::max())
        return false;
    const pid_t child = static_cast<pid_t>(process_id);
    if (::getsid(child) != child)
        return false;
    return ::kill(-child, SIGKILL) == 0;
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
}
bool PtyProcess::readReady() {
    if (!master_)
        return true;
    std::array<char, 16384> bytes{};
    std::size_t consumed{};
    while (consumed < 65536U && !output_paused_) {
        const auto count = ::read(master_.get(), bytes.data(), bytes.size());
        if (count > 0) {
            consumed += static_cast<std::size_t>(count);
            emit output(QByteArray(bytes.data(), static_cast<qsizetype>(count)));
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        reader_->setEnabled(false);
        if (count < 0 && errno != EIO)
            emit failure(system_error("PTY read"));
        return true;
    }
    return false;
}
void PtyProcess::finishWhenDrained(int exit_code, QProcess::ExitStatus exit_status,
                                   int drain_budget) {
    if (output_paused_) {
        QTimer::singleShot(10, this, [this, exit_code, exit_status, drain_budget] {
            finishWhenDrained(exit_code, exit_status, drain_budget);
        });
        return;
    }
    if (!readReady()) {
        if (drain_budget > 1) {
            QTimer::singleShot(0, this, [this, exit_code, exit_status, drain_budget] {
                finishWhenDrained(exit_code, exit_status, drain_budget - 1);
            });
            return;
        }
        // QProcess has already reaped the leader. Its numeric PID is no longer
        // an owned signaling target; bound the tail and close the terminal.
        qWarning("PTY final output exceeded the 16 MiB drain budget; closing the terminal");
    }
    if (reader_)
        reader_->setEnabled(false);
    if (writer_)
        writer_->setEnabled(false);
    master_.reset();
    slave_.reset();
    clearPendingWrite();
    emit finished(exit_code, exit_status);
}
} // namespace lapis::session::posix
