#ifndef LAPIS_SESSION_POSIX_PTY_PROCESS_HPP
#define LAPIS_SESSION_POSIX_PTY_PROCESS_HPP

#include "launch_spec.hpp"
#include "unique_fd.hpp"
#include <lapis/session/terminal.hpp>

#include <QByteArray>
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
    [[nodiscard]] qint64 processId() const { return process_.processId(); }
  signals:
    void output(const QByteArray& bytes);
    void started();
    void finished(int exit_code, QProcess::ExitStatus exit_status);
    void failure(const QString& message);

  private:
    [[nodiscard]] bool readReady();
    void finishWhenDrained(int exit_code, QProcess::ExitStatus exit_status, int drain_budget);
    [[nodiscard]] bool terminateProcessGroup();
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
};

} // namespace lapis::session::posix
#endif // LAPIS_SESSION_POSIX_PTY_PROCESS_HPP
