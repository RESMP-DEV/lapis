#include "platform_desktop.hpp"

#import <Foundation/Foundation.h>
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
} // namespace

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
} // namespace lapis::desktop::platform
