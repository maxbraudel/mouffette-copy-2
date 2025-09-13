#include "MacCursorHider.h"
#ifdef __APPLE__
#import <AppKit/AppKit.h>
#endif

void MacCursorHider::hide() {
#ifdef __APPLE__
    [NSCursor hide];
#endif
}

void MacCursorHider::show() {
#ifdef __APPLE__
    [NSCursor unhide];
#endif
}
