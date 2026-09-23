#include "window_activation.hpp"
#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <stdexcept>

namespace lapis::desktop::test {
void activate_test_window(QWindow& window) {
    window.show();
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        // Logical Qt event tests can own a virtual window without taking the
        // macOS foreground. This does not exercise native activation or input.
        window.requestActivate();
        return;
    }
    // Test fixtures explicitly own desktop focus; product focus policy is unchanged.
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        throw std::runtime_error("GUI test activation requires macOS 14 or later");
    // Qt exposes the native NSView as an integer WId on macOS.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* view = reinterpret_cast<NSView*>(window.winId());
    NSWindow* native = [view window];
    if (native == nil)
        throw std::runtime_error("Test window has no native NSWindow for activation");
    if (window.isActive() && NSApp.active && native.isKeyWindow)
        return;
    if (!NSApp.active) {
        // The modern activate call was refused when this standalone test process
        // reacquired focus on the qualified Mac. These opt-in GUI fixtures own
        // the foreground; keep the compatibility fallback out of product code.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [[NSRunningApplication currentApplication]
            activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
    }
    [native makeKeyAndOrderFront:nil];
    window.requestActivate();
}
} // namespace lapis::desktop::test
