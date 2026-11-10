#include "desktop_actions.hpp"
#include "platform_desktop.hpp"

namespace lapis::desktop {
DesktopActions::DesktopActions(QObject* parent) : QObject(parent) {}

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
