#include "open_request.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <utility>

namespace lapis::desktop {
namespace {
// The request file is a tiny private sidecar; read only this much of it.
constexpr qsizetype max_bytes = 4096;
} // namespace
OpenRequests::OpenRequests(QString path, std::function<void(const QString&)> open, QObject* parent)
    : QObject(parent), path_(std::move(path)), open_(std::move(open)) {
    // Written by renaming, so the folder changes, not the old file.
    watcher_.addPath(QFileInfo(path_).absolutePath());
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &OpenRequests::check);
    // A request written before this window started is stale by now.
    started_ms_ = QDateTime::currentMSecsSinceEpoch();
}

void OpenRequests::check() {
    const QFileInfo info(path_);
    if (!info.exists() || info.isSymLink() || !info.isFile() || info.size() > max_bytes)
        return;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return;
    // The size check is a snapshot; read one byte past the bound so a file
    // that grew or was replaced after it still fails the 4 KiB contract.
    const auto bytes = file.read(max_bytes + 1);
    if (bytes.size() > max_bytes)
        return;
    const auto object = QJsonDocument::fromJson(bytes).object();
    const auto agent = object.value(QStringLiteral("agent")).toString();
    // atMs is external input: read it as an integer, never as a double whose
    // out-of-range or non-finite cast is undefined behavior.
    if (!object.value(QStringLiteral("atMs")).isDouble())
        return;
    const auto at = object.value(QStringLiteral("atMs")).toInteger(-1);
    if (at < 0)
        return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (agent.isEmpty() || QUuid::fromString(agent).isNull() || at <= last_at_ms_ ||
        at < started_ms_ || now - at > max_age_ms || at > now + 1000)
        return;
    last_at_ms_ = at;
    open_(agent);
}
} // namespace lapis::desktop
