#ifndef LAPIS_SESSION_POSIX_PTY_PROCESS_HPP
#define LAPIS_SESSION_POSIX_PTY_PROCESS_HPP

#include "launch_spec.hpp"
#include "unique_fd.hpp"
#include <lapis/session/terminal.hpp>

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QSocketNotifier>
#include <memory>

namespace lapis::session::posix {

using PtyLaunch = LaunchSpec;

// Owned by the separate service event loop, never by the desktop window.
class PtyProcess final : public QObject {
    Q_OBJECT
  public:
    explicit PtyProcess(QObject* parent = nullptr);
    ~PtyProcess() override;
    void start(const PtyLaunch& launch);
    [[nodiscard]] bool writeBytes(const QByteArray& bytes);
    [[nodiscard]] bool resize(TerminalSize size);
    void pauseOutput(bool paused);
    // One `output` signal is one admission batch. A consumer pauses inside that
    // notification to bound retained service-side bytes before the PTY supplies
    // another batch.
    static constexpr qsizetype max_read_batch = qsizetype{64} * 1024;
    [[nodiscard]] qint64 processId() const { return process_.processId(); }
    // End the agent the way closing its terminal would: SIGHUP to the owned
    // process group, then SIGTERM and SIGKILL if it is still running.
    [[nodiscard]] bool hangup();
  signals:
    void output(const QByteArray& bytes);
    void started();
    void finished(int exit_code, QProcess::ExitStatus exit_status);
    void failure(const QString& message);

  private:
    [[nodiscard]] bool readReady();
    void finishWhenDrained(int exit_code, QProcess::ExitStatus exit_status, int drain_budget);
    void recordLeaderExit(int exit_code, QProcess::ExitStatus exit_status);
    void schedulePausedLeaderWatchdog();
    void checkPausedLeader();
    [[nodiscard]] bool leaderExitStatus(int& exit_code, QProcess::ExitStatus& exit_status);
    [[nodiscard]] bool terminateProcessGroup();
    [[nodiscard]] bool signalProcessGroup(int signal);
    void clearPendingWrite();
    void writeReady();
    UniqueFd master_;
    UniqueFd slave_;
    UniqueFd guard_read_;
    UniqueFd guard_control_;
    QProcess process_;
    std::unique_ptr<QSocketNotifier> reader_;
    std::unique_ptr<QSocketNotifier> writer_;
    QByteArray pending_write_;
    qsizetype write_offset_{};
    bool output_paused_{};
    bool final_drain_active_{};
    bool final_drain_closed_{};
    bool final_drain_emitted_{};
    bool paused_leader_watchdog_{};
    bool final_leader_exited_{};
    int leader_exit_code_{};
    QProcess::ExitStatus leader_exit_status_{QProcess::NormalExit};
    QElapsedTimer final_drain_clock_;
};

} // namespace lapis::session::posix
#endif // LAPIS_SESSION_POSIX_PTY_PROCESS_HPP
