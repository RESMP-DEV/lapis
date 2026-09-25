#ifndef LAPIS_DESKTOP_UPDATER_PROCESS_HPP
#define LAPIS_DESKTOP_UPDATER_PROCESS_HPP

#include "platform/posix/unique_fd.hpp"

#include <QProcess>
#include <QSocketNotifier>
#include <functional>

namespace lapis::desktop {

// GUI-thread owner. The POSIX guard retains group membership after the updater
// leader exits, avoiding signals through a reaped/recycled leader PID.
class UpdaterProcess final : public QProcess {
  public:
    explicit UpdaterProcess(QObject* parent);
    ~UpdaterProcess() override;
    void stopGroup();
    void whenStopped(std::function<void()> completed);

  private:
    session::posix::UniqueFd control_read_;
    session::posix::UniqueFd control_write_;
    session::posix::UniqueFd completion_read_;
    session::posix::UniqueFd completion_write_;
    QSocketNotifier* completion_notifier_{}; // QObject-owned; disabled before closing its FD.
    std::function<void()> completed_;
    void acknowledgeStop();
};

} // namespace lapis::desktop
#endif
