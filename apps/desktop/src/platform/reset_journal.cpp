#include "reset_journal.hpp"

#include "platform/posix/unique_fd.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <fcntl.h>
#include <unistd.h>

namespace lapis::desktop::platform {
bool write_reset_journal(const QString& path, const QJsonObject& state) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner))
        return false;
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (bytes.size() > 65536)
        return false;
    if (file.write(bytes) != bytes.size() || !file.flush() || ::fsync(file.handle()) != 0 ||
        !file.commit())
        return false;
    const auto parent = QFile::encodeName(QFileInfo(path).absolutePath());
    session::posix::UniqueFd directory(
        ::open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return directory && ::fsync(directory.get()) == 0;
}
} // namespace lapis::desktop::platform
