#include "chime_sounds.hpp"
#import <AppKit/AppKit.h>
#include <cmath>
#include <iostream>

int main() {
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    bool passed = true;
    for (const auto chime : {lapis::desktop::Chime::needsYou, lapis::desktop::Chime::finished}) {
        const auto bytes = lapis::desktop::chime_wav(chime);
        NSData* data = [NSData dataWithBytes:bytes.constData()
                                      length:static_cast<NSUInteger>(bytes.size())];
        NSSound* sound = [[NSSound alloc] initWithData:data];
        passed = passed && sound != nil && std::abs(sound.duration - 0.62) < 0.01;
        [sound release];
    }
    NSData* invalid = [@"not an audio file" dataUsingEncoding:NSUTF8StringEncoding];
    NSSound* rejected = [[NSSound alloc] initWithData:invalid];
    passed = passed && rejected == nil;
    [rejected release];
    [pool drain];
    std::cout << (passed ? "Native WAV decode and invalid-data rejection passed; no playback\n"
                         : "Native sound decode failed\n");
    return passed ? 0 : 1;
}
