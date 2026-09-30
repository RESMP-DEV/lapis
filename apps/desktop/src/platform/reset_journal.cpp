#include "reset_journal.hpp"

#include "platform/posix/unique_fd.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>

#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace lapis::desktop::platform {
namespace {
bool expiredClosedJournal(const QString& path) {
    QFile file(path);
    if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly) ||
        file.size() > reset_journal_max_bytes)
        return false;
    QJsonParseError error{};
    const auto state = QJsonDocument::fromJson(file.readAll(), &error).object();
    const auto attempts = state.value(QStringLiteral("attempts"));
    if (error.error != QJsonParseError::NoError ||
        state.value(QStringLiteral("v")).toInt() != reset_journal_version ||
        state.value(QStringLiteral("format")) != QLatin1String("lapis-reset-journal") ||
        state.contains(QStringLiteral("pending")) || !attempts.isObject() ||
        attempts.toObject().size() > reset_journal_max_attempts)
        return false;
    const auto now = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
    for (const auto& until : attempts.toObject())
        if (!until.isDouble() || until.toDouble() < 0 || until.toDouble() > now)
            return false;
    return true;
}
bool reserveFile(const QDir& folder) {
    const auto files = folder.entryList({QStringLiteral("*.json")}, QDir::Files);
    auto count = files.size();
    for (const auto& name : files) {
        if (count < reset_journal_max_files)
            return true;
        const auto path = folder.filePath(name);
        QLockFile target(path + QStringLiteral(".lock"));
        target.setStaleLockTime(0);
        if (target.tryLock(0) && expiredClosedJournal(path) && QFile::remove(path))
            --count;
    }
    return count < reset_journal_max_files;
}
} // namespace

JournalWrite write_reset_journal(const QString& path, const QJsonObject& state) {
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (bytes.size() > reset_journal_max_bytes)
        return JournalWrite::failed;
    const QDir parent(QFileInfo(path).absolutePath());
    std::unique_ptr<QLockFile> admission;
    if (!QFileInfo::exists(path)) {
        admission = std::make_unique<QLockFile>(parent.filePath(QStringLiteral(".admission.lock")));
        admission->setStaleLockTime(0);
        if (!admission->tryLock(1000))
            return admission->error() == QLockFile::LockFailedError ? JournalWrite::busy
                                                                    : JournalWrite::failed;
        if (!reserveFile(parent))
            return JournalWrite::full;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner))
        return JournalWrite::failed;
    if (file.write(bytes) != bytes.size() || !file.flush() || ::fsync(file.handle()) != 0 ||
        !file.commit())
        return JournalWrite::failed;
    const auto parent_path = QFile::encodeName(parent.absolutePath());
    session::posix::UniqueFd directory(
        ::open(parent_path.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return directory && ::fsync(directory.get()) == 0 ? JournalWrite::saved : JournalWrite::failed;
}
} // namespace lapis::desktop::platform
