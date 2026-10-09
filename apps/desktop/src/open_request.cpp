#include "open_request.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <utility>

namespace lapis::desktop {
OpenRequests::OpenRequests(QString path, std::function<void(const QString&)> open,
                           QObject* parent)
    : QObject(parent), path_(std::move(path)), open_(std::move(open)) {
    // Written by renaming, so the folder changes, not the old file.
    watcher_.addPath(QFileInfo(path_).absolutePath());
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &OpenRequests::check);
    // A request written before this window started is stale by now.
    started_ms_ = QDateTime::currentMSecsSinceEpoch();
}

void OpenRequests::check() {
    const QFileInfo info(path_);
    if (!info.exists() || info.isSymLink() || !info.isFile() || info.size() > 4096)
        return;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto object = QJsonDocument::fromJson(file.read(4096)).object();
    const auto agent = object.value(QStringLiteral("agent")).toString();
    const auto at = static_cast<qint64>(object.value(QStringLiteral("atMs")).toDouble());
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (agent.isEmpty() || QUuid::fromString(agent).isNull() || at <= last_at_ms_ ||
        at < started_ms_ ||
        now - at > max_age_ms || at > now + 1000)
        return;
    last_at_ms_ = at;
    open_(agent);
}
} // namespace lapis::desktop
