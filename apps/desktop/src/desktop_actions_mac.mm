#include "platform_desktop.hpp"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>
#ifdef LAPIS_SPARKLE
#import <Sparkle/Sparkle.h>
#endif
#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <utility>

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
SMAppService* login_item() API_AVAILABLE(macos(13.0)) {
    return [SMAppService agentServiceWithPlistName:@"dev.lapis.desktop.restore.plist"];
}
} // namespace

bool secure_input_enabled() { return IsSecureEventInputEnabled(); }

double seconds_since_input() {
    // The HID state counts the person's own input, not events other apps post.
    return CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState,
                                                  kCGAnyInputEventType);
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
