#include "platform_desktop.hpp"

#import <AppKit/AppKit.h>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Only this process's event queue is used. No window is opened or activated,
// and no keyboard event is posted to the OS or another application.
void send_key(NSString* characters, unsigned short code, NSEventModifierFlags flags) {
    NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                      location:NSZeroPoint
                                 modifierFlags:flags
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                    characters:characters
                   charactersIgnoringModifiers:characters
                                     isARepeat:NO
                                       keyCode:code];
    [NSApp postEvent:event atStart:YES];
    NSEvent* queued = [NSApp nextEventMatchingMask:NSEventMaskKeyDown
                                         untilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES];
    require(queued != nil, "the local key event reaches the application queue");
    [NSApp sendEvent:queued];
}

struct MonitorGuard {
    ~MonitorGuard() { lapis::desktop::platform::on_terminal_keys({}); }
};

void terminal_monitor() {
    [NSApplication sharedApplication];
    int calls = 0;
    bool chosen = false;
    const MonitorGuard guard;
    lapis::desktop::platform::on_terminal_keys([&](bool shifted) {
        ++calls;
        chosen = shifted;
        return true;
    });
    send_key(@"`", 50, NSEventModifierFlagCommand);
    require(calls == 1 && !chosen, "Command-grave toggles the terminal");
    send_key(@"`", 10, NSEventModifierFlagCommand);
    require(calls == 2 && !chosen, "the logical key works at a different hardware code");
    send_key(@"~", 10, NSEventModifierFlagCommand);
    require(calls == 3 && chosen, "a logical tilde chooses a terminal without requiring Shift");
    send_key(@"`", 50, NSEventModifierFlagCommand | NSEventModifierFlagShift);
    require(calls == 4 && chosen, "Shift-grave chooses a terminal");
    send_key(@"x", 50, NSEventModifierFlagCommand);
    send_key(@"`", 50, NSEventModifierFlagCommand | NSEventModifierFlagOption);
    require(calls == 4, "other logical keys and modifier chords are left alone");
    lapis::desktop::platform::on_terminal_keys({});
    send_key(@"`", 50, NSEventModifierFlagCommand);
    require(calls == 4, "clearing the handler removes the installed monitor");
}
} // namespace

int main() {
    @autoreleasepool {
        try {
            terminal_monitor();
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "native terminal key monitor tests passed\n";
    return 0;
}
