#include "native_input_driver.hpp"
#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>
#include <CoreGraphics/CoreGraphics.h>
#include <QWindow>
#include <stdexcept>
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
    ~Impl() {
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
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        throw std::runtime_error("Native test activation requires macOS 14 or later");
    [[view window] makeKeyAndOrderFront:nil];
    window.requestActivate();
}
void NativeInputDriver::key(std::uint16_t code, NativeModifiers flags) {
    for (const bool down : {true, false}) {
        Handle<CGEventRef> event(CGEventCreateKeyboardEvent(nullptr, code, down));
        if (!event)
            throw std::runtime_error("Could not construct native keyboard event");
        CGEventSetFlags(event.get(), static_cast<CGEventFlags>(flags));
        CGEventPostToPid(::getpid(), event.get());
    }
}
} // namespace lapis::desktop::test
