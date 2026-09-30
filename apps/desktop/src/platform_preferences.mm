#include "platform_preferences.hpp"
#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QQuickWindow>
#import <objc/runtime.h>
namespace lapis::desktop {
namespace {
// Qt's view does not pass Qt::ImhNoPredictiveText on to AppKit, so macOS
// offered the terminal inline predictions, completion, autocorrection and
// smart substitutions; accepting a grey prediction typed it into the agent's
// prompt. AppKit may keep these answers for as long as the view has focus, so
// every lapis window declines them: its other fields are names and searches.
NSTextInputTraitType decline_text_service(id /*view*/, SEL /*selector*/) {
    return NSTextInputTraitTypeNo;
}
NSInteger decline_writing_tools(id /*view*/, SEL /*selector*/) {
    return -1; // NSWritingToolsBehaviorNone
}
// Adds the answers to Qt's view class once; a method Qt defines itself wins.
void decline_text_services(Class view_class) {
    static Class done = nil;
    if (view_class == nil || view_class == done)
        return;
    done = view_class;
    for (const char* trait :
         {"inlinePredictionType", "textCompletionType", "autocorrectionType", "spellCheckingType",
          "grammarCheckingType", "textReplacementType", "smartQuotesType", "smartDashesType",
          "smartInsertDeleteType", "mathExpressionCompletionType"})
        class_addMethod(view_class, sel_registerName(trait),
                        reinterpret_cast<IMP>(&decline_text_service), "q@:");
    class_addMethod(view_class, sel_registerName("writingToolsBehavior"),
                    reinterpret_cast<IMP>(&decline_writing_tools), "q@:");
    class_addProtocol(view_class, @protocol(NSTextInputTraits));
}
} // namespace
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
    decline_text_services([view class]);
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
bool play_sound(const QByteArray& wav, float volume, const QString& cacheKey) {
    // A bounded cache keeps one sound per recent synthesized or file version
    // for reuse; a chime still playing starts over rather than overlapping
    // itself. NSCache also bounds the memory if a user iterates on files.
    static NSCache<id, NSSound*>* sounds = [[NSCache alloc] init];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      sounds.countLimit = 8;
      sounds.totalCostLimit = 16 * 1024 * 1024;
    });
    NSData* data = [NSData dataWithBytes:wav.constData()
                                  length:static_cast<NSUInteger>(wav.size())];
    id key = cacheKey.isEmpty() ? static_cast<id>(data) : static_cast<id>(cacheKey.toNSString());
    NSSound* sound = [sounds objectForKey:key];
    if (sound == nil) {
        sound = [[[NSSound alloc] initWithData:data] autorelease];
        if (sound == nil) {
            if (cacheKey.isEmpty())
                qWarning("NSSound did not decode %lld bytes of sound data",
                         static_cast<long long>(wav.size()));
            else
                qWarning("NSSound did not decode sound file %s", qUtf8Printable(cacheKey));
            return false;
        }
        [sounds setObject:sound forKey:key cost:static_cast<NSUInteger>(wav.size())];
    }
    [sound stop];
    sound.volume = volume;
    [sound play];
    return true;
}
} // namespace lapis::desktop
