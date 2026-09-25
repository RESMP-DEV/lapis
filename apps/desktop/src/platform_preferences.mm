#include "platform_preferences.hpp"
#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QQuickWindow>
namespace lapis::desktop {
bool system_reduced_motion() {
    return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
}
void style_window_chrome(QQuickWindow& window) {
    // Other Qt platform plugins use opaque IDs that are not Cocoa objects.
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    if (!window.isVisible())
        return; // Do not create a native window while it is hidden or closing.
    // Qt exposes its native NSView through WId. This is a borrowed view/window;
    // retain the normal titled window and its native controls and drag region.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* view = reinterpret_cast<NSView*>(window.winId());
    NSWindow* native = [view window];
    if (native == nil)
        return; // Applied again when the window becomes visible.
    const QColor color = window.color();
    native.titlebarAppearsTransparent = YES;
    native.titleVisibility = NSWindowTitleHidden;
    native.backgroundColor = [NSColor colorWithSRGBRed:color.redF()
                                                 green:color.greenF()
                                                  blue:color.blueF()
                                                 alpha:1.0];
    native.appearance = [NSAppearance
        appearanceNamed:color.lightnessF() > 0.5 ? NSAppearanceNameAqua : NSAppearanceNameDarkAqua];
    if (@available(macOS 11.0, *))
        native.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
}
} // namespace lapis::desktop
