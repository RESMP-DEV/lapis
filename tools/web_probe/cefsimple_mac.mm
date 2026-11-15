// Copyright (c) 2026 RESMP-DEV. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "include/cef_application_mac.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_library_loader.h"
#include "tests/cefsimple/simple_app.h"
#include "tests/cefsimple/simple_handler.h"

@interface W0AppDelegate : NSObject <NSApplicationDelegate>
- (void)tryToTerminateApplication:(NSApplication*)app;
@end

@interface W0Application : NSApplication <CefAppProtocol> {
  @private
    BOOL handlingSendEvent_;
}
@end

@implementation W0Application
- (BOOL)isHandlingSendEvent {
    return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
    handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
    CefScopedSendingEvent sending_event_scoper;
    [super sendEvent:event];
}

- (void)terminate:(id)sender {
    W0AppDelegate* delegate = static_cast<W0AppDelegate*>([NSApp delegate]);
    [delegate tryToTerminateApplication:self];
}
@end

@implementation W0AppDelegate
- (void)tryToTerminateApplication:(NSApplication*)app {
    SimpleHandler* handler = SimpleHandler::GetInstance();
    if (handler) {
        handler->CloseAllBrowsers(false);
    }
}
@end

int main(int argc, char* argv[]) {
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain()) {
        return 1;
    }

    CefMainArgs main_args(argc, argv);

    @autoreleasepool {
        [W0Application sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        CHECK([NSApp isKindOfClass:[W0Application class]]);

        CefSettings settings;
        settings.windowless_rendering_enabled = true;
        settings.no_sandbox = true;
        if (const char* output_directory = std::getenv("LAPIS_W0_OUT")) {
            CefString(&settings.root_cache_path) = output_directory;
            CefString(&settings.cache_path) = std::string(output_directory) + "/cache";
        }

        CefRefPtr<SimpleApp> app(new SimpleApp);
        if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
            return CefGetExitCode();
        }

        W0AppDelegate* delegate = [[W0AppDelegate alloc] init];
        NSApp.delegate = delegate;
        CefRunMessageLoop();
        CefShutdown();
        if (const char* output_directory = std::getenv("LAPIS_W0_OUT")) {
            std::ofstream stream(std::string(output_directory) + "/teardown-complete",
                                 std::ios::binary | std::ios::trunc);
            stream << "cef-message-loop-and-shutdown-returned\n";
        }
    }
    return 0;
}
