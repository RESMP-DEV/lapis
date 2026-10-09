#include "platform_desktop.hpp"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UserNotifications/UserNotifications.h>
#ifdef LAPIS_SPARKLE
#import <Sparkle/Sparkle.h>
#endif
#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <utility>

namespace {
std::function<void(const QString&)>& opened_handler() {
    static std::function<void(const QString&)> handler;
    return handler;
}
} // namespace

// Clicking a notification shows its agent; notifications also show while
// lapis is in front, though lapis only posts them from the background.
@interface LapisNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end
@implementation LapisNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
    NSString* agent = response.notification.request.content.userInfo[@"agent"];
    const QString id = agent != nil ? QString::fromNSString(agent) : QString();
    QMetaObject::invokeMethod(
        qApp,
        [id] {
            if (const auto& handler = opened_handler())
                handler(id);
        },
        Qt::QueuedConnection);
    completionHandler();
}
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    completionHandler(UNNotificationPresentationOptionBanner |
                      UNNotificationPresentationOptionList);
}
@end

#ifdef LAPIS_SPARKLE
namespace {
SPUStandardUpdaterController* updater() {
    // Kept for the app's lifetime (no ARC here: alloc keeps it).
    static SPUStandardUpdaterController* controller =
        [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                      updaterDelegate:nil
                                                   userDriverDelegate:nil];
    return controller;
}
} // namespace
#endif

namespace lapis::desktop::platform {
namespace {
// Only an app bundle has a notification center; a test binary does not.
UNUserNotificationCenter* notification_center() {
    if (NSBundle.mainBundle.bundleIdentifier == nil)
        return nil;
    static LapisNotificationDelegate* delegate = [[LapisNotificationDelegate alloc] init];
    UNUserNotificationCenter* center = UNUserNotificationCenter.currentNotificationCenter;
    center.delegate = delegate;
    return center;
}

SMAppService* login_item() API_AVAILABLE(macos(13.0)) {
    return [SMAppService agentServiceWithPlistName:@"dev.lapis.desktop.restore.plist"];
}

std::function<void()>& latest_attention_handler() {
    static std::function<void()> handler;
    return handler;
}

} // namespace

void activate_application() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
}

namespace {
// A registered hot key reaches the app from whichever app is in front.
OSStatus latest_attention_pressed(EventHandlerCallRef, EventRef, void*) {
    if (const auto& handler = latest_attention_handler()) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
        handler();
    }
    return noErr;
}
} // namespace

bool secure_input_enabled() { return IsSecureEventInputEnabled(); }

double seconds_since_input() {
    // The HID state counts the person's own input, not events other apps post.
    return CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState,
                                                  kCGAnyInputEventType);
}

QByteArray claude_code_credentials() {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, CFSTR("Claude Code-credentials"));
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
    CFTypeRef found = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &found);
    CFRelease(query);
    if (status != errSecSuccess && status != errSecItemNotFound)
        qWarning() << "Claude Code keychain lookup failed with status" << status;
    if (status != errSecSuccess || found == nullptr || CFGetTypeID(found) != CFDataGetTypeID()) {
        if (found != nullptr)
            CFRelease(found);
        return {};
    }
    const auto* data = static_cast<CFDataRef>(found);
    QByteArray stored(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                      static_cast<qsizetype>(CFDataGetLength(data)));
    CFRelease(found);
    return stored;
}

bool on_latest_attention_key(const std::function<void()>& handler) {
    static EventHotKeyRef key = nullptr;
    static EventHandlerRef dispatch = nullptr;
    latest_attention_handler() = handler;
    if (!handler) {
        if (key != nullptr)
            UnregisterEventHotKey(key);
        key = nullptr;
        return true;
    }
    if (key != nullptr)
        return true;
    if (dispatch == nullptr) {
        const EventTypeSpec pressed{kEventClassKeyboard, kEventHotKeyPressed};
        if (InstallEventHandler(GetApplicationEventTarget(),
                                NewEventHandlerUPP(latest_attention_pressed), 1, &pressed, nullptr,
                                &dispatch) != noErr)
            return false;
    }
    constexpr OSType signature = 0x6C706973; // 'lpis'
    const EventHotKeyID id{signature, 1};
    return RegisterEventHotKey(kVK_ANSI_L, cmdKey | optionKey, id, GetApplicationEventTarget(), 0,
                               &key) == noErr;
}

void post_notification(const QString& id, const QString& title, const QString& body) {
    UNUserNotificationCenter* center = notification_center();
    if (center == nil)
        return;
    UNMutableNotificationContent* content =
        [[[UNMutableNotificationContent alloc] init] autorelease];
    content.title = title.toNSString();
    content.body = body.toNSString();
    content.userInfo = @{@"agent" : id.toNSString()};
    // One notification per agent: a newer one replaces the older.
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:id.toNSString()
                                                                          content:content
                                                                          trigger:nil];
    [request retain];
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                          completionHandler:^(BOOL granted, NSError*) {
                            if (granted)
                                [center addNotificationRequest:request withCompletionHandler:nil];
                            [request release];
                          }];
}

void on_notification_opened(const std::function<void(const QString&)>& handler) {
    opened_handler() = handler;
    static_cast<void>(notification_center());
}

bool login_item_enabled() {
    if (@available(macOS 13.0, *))
        return login_item().status == SMAppServiceStatusEnabled;
    return false;
}

bool set_login_item(bool on) {
    if (@available(macOS 13.0, *)) {
        NSError* error = nil;
        const BOOL done = on ? [login_item() registerAndReturnError:&error]
                             : [login_item() unregisterAndReturnError:&error];
        if (!done && error != nil)
            qWarning().noquote() << "Login item:"
                                 << QString::fromNSString(error.localizedDescription);
        return done == YES;
    }
    return false;
}

void start_updater() {
#ifdef LAPIS_SPARKLE
    static_cast<void>(updater());
#endif
}

void check_for_updates() {
#ifdef LAPIS_SPARKLE
    [updater() checkForUpdates:nil];
#endif
}

bool updater_available() {
#ifdef LAPIS_SPARKLE
    return true;
#else
    return false;
#endif
}

void on_terminal_keys(const std::function<bool(bool shifted)>& handler) {
    static std::function<bool(bool)> current;
    static id monitor = nil;
    if (!handler) {
        if (monitor != nil) {
            [NSEvent removeMonitor:monitor];
            monitor = nil;
        }
        current = {};
        return;
    }
    current = handler;
    if (monitor != nil)
        return;
    // A local monitor sees the key before the window cycling AppKit does
    // with Command-`. Match the character produced by the active layout, not
    // an ANSI hardware key code, because layouts place these keys differently.
    monitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                     handler:^NSEvent*(NSEvent* event) {
                                       const auto flags =
                                           event.modifierFlags &
                                           NSEventModifierFlagDeviceIndependentFlagsMask;
                                       const auto others =
                                           NSEventModifierFlagControl | NSEventModifierFlagOption;
                                       NSString* const characters =
                                           event.charactersIgnoringModifiers;
                                       const bool terminal_key =
                                           characters.length == 1 &&
                                           ([characters characterAtIndex:0] == '`' ||
                                            [characters characterAtIndex:0] == '~');
                                       if (!terminal_key || !(flags & NSEventModifierFlagCommand) ||
                                           (flags & others) || !current)
                                           return event;
                                       const bool shifted =
                                           (flags & NSEventModifierFlagShift) != 0 ||
                                           [characters characterAtIndex:0] == '~';
                                       return current(shifted) ? nil : event;
                                     }];
}
} // namespace lapis::desktop::platform
