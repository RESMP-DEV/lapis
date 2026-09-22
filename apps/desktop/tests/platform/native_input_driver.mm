#include "native_input_driver.hpp"
#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>
#include <CoreGraphics/CoreGraphics.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QWindow>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>

namespace lapis::desktop::test {
namespace {
struct Release {
    void operator()(const void* value) const {
        if (value)
            CFRelease(value);
    }
};
template <typename T> using Handle = std::unique_ptr<std::remove_pointer_t<T>, Release>;
Handle<TISInputSourceRef> find_source(CFStringRef id) {
    Handle<CFArrayRef> sources(TISCreateInputSourceList(nullptr, true));
    if (!sources)
        throw std::runtime_error("Input source inventory unavailable");
    for (CFIndex index = 0; index < CFArrayGetCount(sources.get()); ++index) {
        auto source = static_cast<TISInputSourceRef>(
            const_cast<void*>(CFArrayGetValueAtIndex(sources.get(), index)));
        auto name =
            static_cast<CFStringRef>(TISGetInputSourceProperty(source, kTISPropertyInputSourceID));
        if (name && CFEqual(name, id)) {
            CFRetain(source);
            return Handle<TISInputSourceRef>(source);
        }
    }
    throw std::runtime_error("Required native input source is not installed");
}
void select(TISInputSourceRef source) {
    const auto status = TISSelectInputSource(source);
    if (status != noErr)
        throw std::runtime_error("Native input source selection failed: " + std::to_string(status));
}
bool enabled(TISInputSourceRef source) {
    const auto value = static_cast<CFBooleanRef>(
        TISGetInputSourceProperty(source, kTISPropertyInputSourceIsEnabled));
    return value && CFBooleanGetValue(value);
}
} // namespace
struct NativeInputDriver::Impl {
    Handle<TISInputSourceRef> previous{TISCopyCurrentKeyboardInputSource()};
    Handle<TISInputSourceRef> us{find_source(CFSTR("com.apple.keylayout.US"))};
    Handle<TISInputSourceRef> japanese_parent{
        find_source(CFSTR("com.apple.inputmethod.Kotoeri.RomajiTyping"))};
    Handle<TISInputSourceRef> japanese{
        find_source(CFSTR("com.apple.inputmethod.Kotoeri.RomajiTyping.Japanese"))};
    bool us_was_enabled{enabled(us.get())};
    bool parent_was_enabled{enabled(japanese_parent.get())};
    bool japanese_was_enabled{enabled(japanese.get())};
    id monitor{nil};
    std::int64_t sequence{0};
    std::int64_t delivered{0};
    ~Impl() {
        if (monitor != nil)
            [NSEvent removeMonitor:monitor];
        if (previous)
            static_cast<void>(TISSelectInputSource(previous.get()));
        if (!us_was_enabled)
            static_cast<void>(TISDisableInputSource(us.get()));
        if (!japanese_was_enabled)
            static_cast<void>(TISDisableInputSource(japanese.get()));
        if (!parent_was_enabled)
            static_cast<void>(TISDisableInputSource(japanese_parent.get()));
    }
};
NativeInputDriver::NativeInputDriver() : impl_(std::make_unique<Impl>()) {
    auto* state = impl_.get();
    impl_->monitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp
                                     handler:^NSEvent*(NSEvent* event) {
                                       if (event.CGEvent != nullptr &&
                                           CGEventGetIntegerValueField(event.CGEvent,
                                                                       kCGEventSourceUserData) ==
                                               state->sequence)
                                           state->delivered = state->sequence;
                                       if (qEnvironmentVariableIsSet("LAPIS_NATIVE_TRACE"))
                                           std::cerr << "AppKit key type=" << event.type
                                                     << " code=" << event.keyCode
                                                     << " flags=" << event.modifierFlags
                                                     << " active=" << NSApp.active << '\n';
                                       return event;
                                     }];
    if (impl_->monitor == nil)
        throw std::runtime_error("Native input event monitor unavailable");
    if (!CGPreflightPostEventAccess())
        throw std::runtime_error("macOS Accessibility event-posting permission is unavailable");
    if (TISEnableInputSource(impl_->us.get()) != noErr ||
        TISEnableInputSource(impl_->japanese_parent.get()) != noErr ||
        TISEnableInputSource(impl_->japanese.get()) != noErr)
        throw std::runtime_error("Could not temporarily enable test input sources");
}
NativeInputDriver::~NativeInputDriver() = default;
void NativeInputDriver::selectUS() { select(impl_->us.get()); }
void NativeInputDriver::selectJapanese() { select(impl_->japanese.get()); }
QString NativeInputDriver::selectedSource() {
    Handle<TISInputSourceRef> source(TISCopyCurrentKeyboardInputSource());
    const auto name = static_cast<CFStringRef>(
        TISGetInputSourceProperty(source.get(), kTISPropertyInputSourceID));
    char bytes[1024]{};
    if (!name || !CFStringGetCString(name, bytes, sizeof(bytes), kCFStringEncodingUTF8))
        throw std::runtime_error("Could not read selected input source");
    return QString::fromUtf8(bytes);
}
QStringList NativeInputDriver::enabledSources() {
    Handle<CFArrayRef> sources(TISCreateInputSourceList(nullptr, true));
    if (!sources)
        throw std::runtime_error("Input source inventory unavailable");
    QStringList result;
    for (CFIndex index = 0; index < CFArrayGetCount(sources.get()); ++index) {
        const auto source = static_cast<TISInputSourceRef>(
            const_cast<void*>(CFArrayGetValueAtIndex(sources.get(), index)));
        if (!enabled(source))
            continue;
        const auto name =
            static_cast<CFStringRef>(TISGetInputSourceProperty(source, kTISPropertyInputSourceID));
        char bytes[1024]{};
        if (!name || !CFStringGetCString(name, bytes, sizeof(bytes), kCFStringEncodingUTF8))
            throw std::runtime_error("Input source identity unavailable");
        result.append(QString::fromUtf8(bytes));
    }
    result.sort();
    return result;
}
void NativeInputDriver::activate(QWindow& window) {
    // Qt represents the native NSView pointer as an integer WId on macOS.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* view = reinterpret_cast<NSView*>(window.winId());
    NSWindow* native = [view window];
    if (native == nil)
        throw std::runtime_error("Native input fixture has no NSWindow");
    if (window.isActive() && NSApp.active && native.isKeyWindow)
        return;
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        throw std::runtime_error("Native test activation requires macOS 14 or later");
    [native makeKeyAndOrderFront:nil];
    window.requestActivate();
}
void NativeInputDriver::key(std::uint16_t code, NativeModifiers flags) {
    for (const bool down : {true, false}) {
        Handle<CGEventRef> event(CGEventCreateKeyboardEvent(nullptr, code, down));
        if (!event)
            throw std::runtime_error("Could not construct native keyboard event");
        auto event_flags = static_cast<CGEventFlags>(flags);
        // Physical macOS arrows carry the numeric-pad flag; omitting it tests
        // a different Qt modifier combination from real keyboard input.
        if (code >= 123 && code <= 126)
            event_flags |= kCGEventFlagMaskNumericPad;
        CGEventSetFlags(event.get(), event_flags);
        const auto sequence = ++impl_->sequence;
        CGEventSetIntegerValueField(event.get(), kCGEventSourceUserData, sequence);
        CGEventPostToPid(::getpid(), event.get());
        // AppKit delivery is asynchronous. Drain each edge before sending the
        // next, without resending a key that could duplicate terminal input.
        QElapsedTimer timer;
        timer.start();
        while (impl_->delivered != sequence) {
            if (timer.elapsed() >= 10000) {
                if (down) {
                    CGEventSetType(event.get(), kCGEventKeyUp);
                    CGEventPostToPid(::getpid(), event.get());
                }
                throw std::runtime_error("AppKit did not receive native key " +
                                         std::to_string(code) + (down ? " down" : " up"));
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
    }
}
} // namespace lapis::desktop::test
