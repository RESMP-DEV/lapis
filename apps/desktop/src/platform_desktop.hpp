#ifndef LAPIS_DESKTOP_PLATFORM_DESKTOP_HPP
#define LAPIS_DESKTOP_PLATFORM_DESKTOP_HPP
#include <QString>
#include <QtGlobal>
#include <functional>

// The macOS services behind DesktopActions; elsewhere they do nothing and
// report themselves unavailable.
namespace lapis::desktop::platform {
#ifdef Q_OS_MACOS
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
// Secure event input is on: some field (in any app) is taking a password.
[[nodiscard]] bool secure_input_enabled();
// Seconds since the last keyboard, mouse or trackpad input anywhere on this
// Mac (hardware events only), or a negative value when it cannot be read.
[[nodiscard]] double seconds_since_input();
#else
inline bool login_item_enabled() { return false; }
inline bool set_login_item(bool) { return false; }
inline void start_updater() {}
inline void check_for_updates() {}
inline bool updater_available() { return false; }
inline void on_terminal_keys(const std::function<bool(bool)>&) {}
inline bool secure_input_enabled() { return false; }
inline double seconds_since_input() { return -1; }
#endif
} // namespace lapis::desktop::platform
#endif
