#ifndef LAPIS_DESKTOP_PLATFORM_DESKTOP_HPP
#define LAPIS_DESKTOP_PLATFORM_DESKTOP_HPP
#include <QByteArray>
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
// Command-Option-L from any app brings lapis to the front and runs the handler.
// An empty handler releases the key; false when the key could not be taken.
bool on_latest_attention_key(const std::function<void()>& handler);
// Claude Code's stored sign-in (its keychain item's JSON), empty when absent or
// refused. The first read asks the person's permission, so call it off the main
// thread.
[[nodiscard]] QByteArray claude_code_credentials();
// Secure event input is on: some field (in any app) is taking a password.
[[nodiscard]] bool secure_input_enabled();
#else
inline void post_notification(const QString&, const QString&, const QString&) {}
inline void on_notification_opened(const std::function<void(const QString&)>&) {}
inline bool login_item_enabled() { return false; }
inline bool set_login_item(bool) { return false; }
inline void start_updater() {}
inline void check_for_updates() {}
inline bool updater_available() { return false; }
inline void on_terminal_keys(const std::function<bool(bool)>&) {}
inline bool on_latest_attention_key(const std::function<void()>&) { return false; }
inline QByteArray claude_code_credentials() { return {}; }
inline bool secure_input_enabled() { return false; }
#endif
} // namespace lapis::desktop::platform
#endif
