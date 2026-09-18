#include "platform_preferences.hpp"
#import <AppKit/NSAccessibility.h>
#import <AppKit/NSWorkspace.h>
namespace lapis::desktop {
bool system_reduced_motion() {
    return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
}
} // namespace lapis::desktop
