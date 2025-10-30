#include "backend/platform/macos/MacWindowManager.h"

#ifdef Q_OS_MACOS
#import <Cocoa/Cocoa.h>
#include <QtGui/QWindow>

void MacWindowManager::setWindowAlwaysOnTop(QWidget* widget) {
    if (!widget || !widget->windowHandle()) {
        return;
    }
    
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(widget->windowHandle()->winId());
    NSWindow* window = [view window];
    
    if (window) {
        // Set window level to floating level (stays above normal windows)
        [window setLevel:NSFloatingWindowLevel];
        
        // Configure collection behavior to join all Spaces and stay visible during full screen
        [window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces | 
                                     NSWindowCollectionBehaviorFullScreenAuxiliary];
    }
}

void MacWindowManager::setWindowAsGlobalOverlay(QWidget* widget, bool clickThrough) {
    if (!widget || !widget->windowHandle()) {
        return;
    }
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(widget->windowHandle()->winId());
    NSWindow* window = [view window];
    if (!window) return;

    // Ensure borderless non-opaque window with clear background
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [window setHasShadow:NO];

    // Set high level so it stays above normal app windows and typical floating panels
    // NSPopUpMenuWindowLevel is above status and modal panel, but below screensaver
    [window setLevel:NSPopUpMenuWindowLevel];

    // Make it present across Spaces and as auxiliary in full-screen; avoid Mission Control/App Exposé and window cycling
    NSWindowCollectionBehavior behavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                          NSWindowCollectionBehaviorFullScreenAuxiliary |
                                          NSWindowCollectionBehaviorTransient |
                                          NSWindowCollectionBehaviorIgnoresCycle;
    [window setCollectionBehavior:behavior];

    // Ensure it comes to front now without activating the app
    [window orderFrontRegardless];

    // Do not activate or take focus, optionally ignore mouse
    if ([window isKindOfClass:[NSPanel class]]) {
        NSPanel* panel = (NSPanel*)window;
        [panel setWorksWhenModal:YES];
        [panel setBecomesKeyOnlyIfNeeded:YES];
    }
    [window setHidesOnDeactivate:NO];
    [window setAcceptsMouseMovedEvents:NO];
    if (clickThrough) {
        [window setIgnoresMouseEvents:YES];
    }
}

#else

void MacWindowManager::setWindowAlwaysOnTop(QWidget* widget) {
    // No-op on other platforms
    Q_UNUSED(widget);
}

void MacWindowManager::setWindowAsGlobalOverlay(QWidget* widget, bool clickThrough) {
    Q_UNUSED(widget);
    Q_UNUSED(clickThrough);
}

#endif