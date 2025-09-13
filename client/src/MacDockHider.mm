#include "MacDockHider.h"

#ifdef Q_OS_MACOS
#import <AppKit/AppKit.h>

void MacDockHider::hideDockIcon() {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

#else

void MacDockHider::hideDockIcon() {
    // No-op on other platforms
}

#endif
