#include "desktop_actions.hpp"
#include "keymap.hpp"
#include "platform_desktop.hpp"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <array>

namespace lapis::desktop {
namespace {
struct KnownEditor {
    const char* name;    // as the app is named, and shown
    const char* command; // its command-line launcher elsewhere
};
// In order of preference when none is configured.
constexpr std::array kEditors{
    KnownEditor{"Cursor", "cursor"},       KnownEditor{"Visual Studio Code", "code"},
    KnownEditor{"Zed", "zed"},             KnownEditor{"Windsurf", "windsurf"},
    KnownEditor{"Sublime Text", "subl"},
};

[[maybe_unused]] QString installed_app(const QString& name) {
    for (const auto& folder :
         {QStringLiteral("/Applications"), QDir::home().filePath(QStringLiteral("Applications"))}) {
        const QString path = QDir(folder).filePath(name + QStringLiteral(".app"));
        if (QFileInfo(path).isDir())
            return path;
    }
    return {};
}

[[maybe_unused]] QString command_for(const QString& editor) {
    for (const auto& known : kEditors)
        if (editor == QLatin1String(known.name))
            return QString::fromLatin1(known.command);
    return editor;
}
} // namespace

DesktopActions::DesktopActions(const KeyMap& config, QObject* parent)
    : QObject(parent), config_(config) {
    connect(&config, &KeyMap::changed, this, &DesktopActions::editorChanged);
}

void DesktopActions::copyText(const QString& text) { QGuiApplication::clipboard()->setText(text); }

bool DesktopActions::revealFolder(const QString& path) {
    if (!QFileInfo(path).isDir())
        return false;
#ifdef Q_OS_MACOS
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {path});
#else
    return QProcess::startDetached(QStringLiteral("xdg-open"), {path});
#endif
}

QStringList DesktopActions::installedEditors() {
    QStringList found;
    for (const auto& editor : kEditors) {
#ifdef Q_OS_MACOS
        if (!installed_app(QString::fromLatin1(editor.name)).isEmpty())
            found.append(QString::fromLatin1(editor.name));
#else
        if (!QStandardPaths::findExecutable(QString::fromLatin1(editor.command)).isEmpty())
            found.append(QString::fromLatin1(editor.name));
#endif
    }
    return found;
}

QString DesktopActions::chooseEditor(const QString& configured, const QStringList& installed) {
    if (const auto chosen = configured.trimmed(); !chosen.isEmpty())
        return chosen;
    return installed.isEmpty() ? QString() : installed.front();
}

QString DesktopActions::editorName() const {
    const auto chosen = chooseEditor(config_.editor(), installedEditors());
    return chosen.endsWith(QStringLiteral(".app")) ? QFileInfo(chosen).completeBaseName()
                                                   : QFileInfo(chosen).fileName();
}

bool DesktopActions::openInEditor(const QString& path) {
    const auto chosen = chooseEditor(config_.editor(), installedEditors());
    if (chosen.isEmpty() || !QFileInfo(path).isDir())
        return false;
#ifdef Q_OS_MACOS
    // `open -a` takes an app's name or its path.
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"),
                                   {QStringLiteral("-a"), chosen, path});
#else
    return QProcess::startDetached(command_for(chosen), {path});
#endif
}

bool DesktopActions::launchAtLoginAvailable() const {
#if defined(Q_OS_MACOS) && defined(LAPIS_PACKAGED)
    return true;
#else
    return false;
#endif
}

bool DesktopActions::launchAtLogin() const {
    return launchAtLoginAvailable() && platform::login_item_enabled();
}

bool DesktopActions::setLaunchAtLogin(bool on) {
    if (!launchAtLoginAvailable())
        return false;
    const bool done = platform::set_login_item(on);
    emit launchAtLoginChanged();
    return done;
}

bool DesktopActions::updatesAvailable() const { return platform::updater_available(); }

void DesktopActions::checkForUpdates() { platform::check_for_updates(); }
} // namespace lapis::desktop
