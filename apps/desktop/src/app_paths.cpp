#include "app_paths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace lapis::desktop {
QString data_directory() {
    if (const auto set = qEnvironmentVariable("LAPIS_HOME"); !set.isEmpty())
        return QFileInfo(set).absoluteFilePath();
#ifdef LAPIS_PACKAGED
    const QString directory = QDir::home().filePath(QStringLiteral(".lapis"));
    // Private from the start: the workspace and window state refuse a
    // runtime folder other users can read or write.
    const auto owner_only = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    QDir().mkpath(QDir(directory).filePath(QStringLiteral("runtime")), owner_only);
    return directory;
#else
    return QFileInfo(QStringLiteral(LAPIS_PROJECT_ROOT)).absoluteFilePath();
#endif
}

QString session_service_program() {
#ifdef LAPIS_PACKAGED
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("lapis_session_service"));
#else
    return QStringLiteral(LAPIS_SESSION_SERVICE_PATH);
#endif
}

QString vulkan_library() {
#ifdef LAPIS_PACKAGED
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("../Frameworks/libMoltenVK.dylib"));
#else
    return QStringLiteral(LAPIS_VULKAN_LIBRARY);
#endif
}

QString default_working_directory() {
#ifdef LAPIS_PACKAGED
    return QDir::homePath();
#else
    return QStringLiteral(LAPIS_PROJECT_ROOT);
#endif
}
} // namespace lapis::desktop
