#ifndef LAPIS_DESKTOP_PLATFORM_DESKTOP_HPP
#define LAPIS_DESKTOP_PLATFORM_DESKTOP_HPP
#include <QString>
#include <QtGlobal>
#include <functional>

// The macOS services behind DesktopActions and Notifier; elsewhere they do
// nothing and report themselves unavailable.
namespace lapis::desktop::platform {
#ifdef Q_OS_MACOS
// Posts a notification; the first one asks the person's permission.
void post_notification(const QString& id, const QString& title, const QString& body);
// Called with the agent id when the person clicks a notification.
void on_notification_opened(const std::function<void(const QString&)>& handler);
// The downloaded app's login item, which restarts agents at login.
[[nodiscard]] bool login_item_enabled();
bool set_login_item(bool on);
// Sparkle, in the downloaded app.
void start_updater();
void check_for_updates();
[[nodiscard]] bool updater_available();
// Command-` and Command-Shift-` reach the handler before AppKit can use them
// to cycle windows; it returns true when it took the key.
void on_terminal_keys(const std::function<bool(bool shifted)>& handler);
#else
inline void post_notification(const QString&, const QString&, const QString&) {}
inline void on_notification_opened(const std::function<void(const QString&)>&) {}
inline bool login_item_enabled() { return false; }
inline bool set_login_item(bool) { return false; }
inline void start_updater() {}
inline void check_for_updates() {}
inline bool updater_available() { return false; }
inline void on_terminal_keys(const std::function<bool(bool)>&) {}
#endif
} // namespace lapis::desktop::platform
#endif
