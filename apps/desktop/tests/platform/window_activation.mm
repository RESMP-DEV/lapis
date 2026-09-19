#include "window_activation.hpp"
#import <AppKit/AppKit.h>
#include <stdexcept>

namespace lapis::desktop::test {
void activate_test_window(QWindow& window) {
    window.show();
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
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
    [native makeKeyAndOrderFront:nil];
    window.requestActivate();
}
} // namespace lapis::desktop::test
