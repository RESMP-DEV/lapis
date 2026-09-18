#ifndef LAPIS_SESSION_POSIX_PTY_PROCESS_HPP
#define LAPIS_SESSION_POSIX_PTY_PROCESS_HPP

#include "unique_fd.hpp"
#include <lapis/session/terminal.hpp>

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QString>

#include <memory>

namespace lapis::session::posix {

struct PtyLaunch {
    QString shell;
    QString directory;
    TerminalSize size;
};

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
    void finished(int exit_code);
    void failure(const QString& message);

  private:
    [[nodiscard]] bool readReady();
    void finishWhenDrained(int exit_code);
    void writeReady();
    UniqueFd master_;
    UniqueFd slave_;
    QProcess process_;
    std::unique_ptr<QSocketNotifier> reader_;
    std::unique_ptr<QSocketNotifier> writer_;
    QByteArray pending_write_;
    qsizetype write_offset_{};
};

} // namespace lapis::session::posix
#endif // LAPIS_SESSION_POSIX_PTY_PROCESS_HPP
