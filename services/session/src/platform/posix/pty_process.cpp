#include "pty_process.hpp"

#include <QProcessEnvironment>
#include <QTimer>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
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
        reader_->setEnabled(true);
        emit started();
        writeReady();
    });
    connect(&process_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus) { finishWhenDrained(code); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        if (reader_)
            reader_->setEnabled(false);
        if (writer_)
            writer_->setEnabled(false);
        master_.reset();
        slave_.reset();
        emit failure(process_.errorString());
    });
}
PtyProcess::~PtyProcess() {
    disconnect(&process_, nullptr, this, nullptr);
    reader_.reset();
    writer_.reset();
    master_.reset();
    slave_.reset();
    // Only explicit service shutdown owns this cleanup; desktop detachment never
    // destroys the service. Keep child reaping bounded even for a resistant shell.
    if (process_.state() != QProcess::NotRunning) {
        process_.terminate();
        if (!process_.waitForFinished(200)) {
            process_.kill();
            static_cast<void>(process_.waitForFinished(200));
        }
    }
}
void PtyProcess::start(const PtyLaunch& launch) {
    if (process_.state() != QProcess::NotRunning || master_) {
        emit failure(QStringLiteral("PTY already started"));
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
        !resize(launch.size)) {
        emit failure(system_error("Configure PTY"));
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
    process_.setWorkingDirectory(launch.directory);
    process_.setProgram(launch.shell);
    process_.setArguments({QStringLiteral("-i")});
    const int master = master_.get();
    const int slave = slave_.get();
    process_.setChildProcessModifier([this, master, slave] {
        if (::setsid() < 0)
            process_.failChildProcessModifier("setsid", errno);
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
        emit failure(system_error("PTY write"));
        return;
    }
    pending_write_.clear();
    write_offset_ = 0;
    writer_->setEnabled(false);
}
bool PtyProcess::readReady() {
    if (!master_)
        return true;
    std::array<char, 16384> bytes{};
    std::size_t consumed{};
    while (consumed < 65536U) {
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
void PtyProcess::finishWhenDrained(int exit_code) {
    if (!readReady()) {
        QTimer::singleShot(0, this, [this, exit_code] { finishWhenDrained(exit_code); });
        return;
    }
    if (reader_)
        reader_->setEnabled(false);
    if (writer_)
        writer_->setEnabled(false);
    master_.reset();
    slave_.reset();
    pending_write_.clear();
    write_offset_ = 0;
    emit finished(exit_code);
}
} // namespace lapis::session::posix
