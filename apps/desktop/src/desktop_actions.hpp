#ifndef LAPIS_DESKTOP_DESKTOP_ACTIONS_HPP
#define LAPIS_DESKTOP_DESKTOP_ACTIONS_HPP
#include <QObject>
#include <QString>
#include <QStringList>

namespace lapis::desktop {
class KeyMap;

// What the window asks of the desktop around it: the clipboard, Finder, the
// person's editor, starting at login and app updates. QML sees it as
// `desktop`; the parts a platform lacks report themselves unavailable.
class DesktopActions final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString editorName READ editorName NOTIFY editorChanged)
    Q_PROPERTY(bool launchAtLoginAvailable READ launchAtLoginAvailable CONSTANT)
    Q_PROPERTY(bool launchAtLogin READ launchAtLogin NOTIFY launchAtLoginChanged)
    Q_PROPERTY(bool updatesAvailable READ updatesAvailable CONSTANT)
  public:
    explicit DesktopActions(const KeyMap& config, QObject* parent = nullptr);
    Q_INVOKABLE void copyText(const QString& text);
    // Opens the folder in Finder (or the Linux file manager).
    Q_INVOKABLE bool revealFolder(const QString& path);
    // Opens the folder in the configured editor, or the first installed of a
    // few common ones; false when there is none.
    Q_INVOKABLE bool openInEditor(const QString& path);
    [[nodiscard]] QString editorName() const;
    // The downloaded app registers itself to restart agents at login; a
    // developer build uses scripts/restore_at_login.py instead.
    [[nodiscard]] bool launchAtLoginAvailable() const;
    [[nodiscard]] bool launchAtLogin() const;
    Q_INVOKABLE bool setLaunchAtLogin(bool on);
    [[nodiscard]] bool updatesAvailable() const;
    Q_INVOKABLE void checkForUpdates();

    // The editor to use: a configured app name, .app path or command, else
    // the first of `installed` in preference order. Empty when none.
    [[nodiscard]] static QString chooseEditor(const QString& configured,
                                              const QStringList& installed);
    // Editors this Mac (or Linux host) has, by display name.
    [[nodiscard]] static QStringList installedEditors();

  signals:
    void editorChanged();
    void launchAtLoginChanged();

  private:
    const KeyMap& config_;
};
} // namespace lapis::desktop
#endif
