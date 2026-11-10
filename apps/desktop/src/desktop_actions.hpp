#ifndef LAPIS_DESKTOP_DESKTOP_ACTIONS_HPP
#define LAPIS_DESKTOP_DESKTOP_ACTIONS_HPP
#include <QObject>

namespace lapis::desktop {
// What the window asks of the desktop around it: starting at login and app
// updates. QML sees it as `desktop`; the parts a platform lacks report
// themselves unavailable.
class DesktopActions final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool launchAtLoginAvailable READ launchAtLoginAvailable CONSTANT)
    Q_PROPERTY(bool launchAtLogin READ launchAtLogin NOTIFY launchAtLoginChanged)
    Q_PROPERTY(bool updatesAvailable READ updatesAvailable CONSTANT)
  public:
    explicit DesktopActions(QObject* parent = nullptr);
    // The downloaded app registers itself to restart agents at login; a
    // developer build uses scripts/restore_at_login.py instead.
    [[nodiscard]] bool launchAtLoginAvailable() const;
    [[nodiscard]] bool launchAtLogin() const;
    Q_INVOKABLE bool setLaunchAtLogin(bool on);
    [[nodiscard]] bool updatesAvailable() const;
    Q_INVOKABLE void checkForUpdates();

  signals:
    void launchAtLoginChanged();
};
} // namespace lapis::desktop
#endif
