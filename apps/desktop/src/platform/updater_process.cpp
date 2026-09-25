#include "platform/updater_process.hpp"
#include "platform/posix/process_group_guard.hpp"

#include <cerrno>
#include <unistd.h>
#include <utility>

namespace lapis::desktop {

UpdaterProcess::UpdaterProcess(QObject* parent) : QProcess(parent) {
    const int descriptor_limit = ::getdtablesize();
    const bool prepared = descriptor_limit > 0 &&
                          session::posix::open_guard_pipe(control_read_, control_write_) &&
                          session::posix::open_guard_pipe(completion_read_, completion_write_);
    const int prepare_error = prepared ? 0 : errno != 0 ? errno : EIO;
    // The modifier forks a guard; it must have its own address space.
    setUnixProcessParameters(QProcess::UnixProcessParameters{});
    setChildProcessModifier([this, descriptor_limit, prepare_error] {
        if (prepare_error != 0)
            failChildProcessModifier("updater guard pipe", prepare_error);
        if (::setsid() < 0)
            failChildProcessModifier("updater setsid", errno);
        if (!session::posix::start_group_guard({control_read_.get(), control_write_.get()},
                                               descriptor_limit, completion_write_.get()))
            failChildProcessModifier("updater process-group guard", errno);
    });
    connect(this, &QProcess::started, this, [this] {
        control_read_.reset();
        completion_write_.reset();
    });
    if (completion_read_) {
        completion_notifier_ =
            new QSocketNotifier(completion_read_.get(), QSocketNotifier::Read, this);
        completion_notifier_->setEnabled(false);
        connect(completion_notifier_, &QSocketNotifier::activated, this,
                [this] { acknowledgeStop(); });
    }
}

UpdaterProcess::~UpdaterProcess() {
    if (completion_notifier_)
        completion_notifier_->setEnabled(false);
    completed_ = {};
    stopGroup();
}

void UpdaterProcess::stopGroup() {
    // EOF makes the guard signal its own group, including a leader that has
    // already exited and installer children that ignore TERM.
    control_read_.reset();
    control_write_.reset();
}

void UpdaterProcess::whenStopped(std::function<void()> completed) {
    completed_ = std::move(completed);
    stopGroup();
    completion_write_.reset(); // Also covers failure before started().
    if (completion_notifier_) {
        completion_notifier_->setEnabled(true);
    } else {
        // Pipe preparation failed before any executable could be launched.
        auto callback = std::move(completed_);
        callback();
    }
}

void UpdaterProcess::acknowledgeStop() {
    char byte{};
    const auto count = ::read(completion_read_.get(), &byte, 1);
    if (count != 0)
        return;
    completion_notifier_->setEnabled(false);
    completion_read_.reset();
    auto callback = std::move(completed_);
    if (callback)
        callback();
}

} // namespace lapis::desktop
