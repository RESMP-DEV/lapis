#ifndef LAPIS_DESKTOP_OPEN_REQUEST_HPP
#define LAPIS_DESKTOP_OPEN_REQUEST_HPP
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <functional>

namespace lapis::desktop {
// Ultra Tab asks the window to show an agent by writing a small request
// beside the registry: {"agent": "<session id>", "atMs": <epoch ms>}. Each
// fresh request (at most `max_age_ms` old, newer than the last one followed)
// calls `open` once with the agent's id. The file is only read: a regular
// file under 4 KiB, never a link.
class OpenRequests final : public QObject {
    Q_OBJECT
  public:
    OpenRequests(QString path, std::function<void(const QString&)> open, QObject* parent = nullptr);
    static constexpr qint64 max_age_ms = 10000;
    // Reads the request now (the watcher calls this; a test seam).
    void check();

  private:
    QString path_;
    std::function<void(const QString&)> open_;
    QFileSystemWatcher watcher_;
    qint64 last_at_ms_{};
    qint64 started_ms_{};
};
} // namespace lapis::desktop
#endif
