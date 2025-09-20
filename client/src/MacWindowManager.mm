#include "MacWindowManager.h"

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

#else

void MacWindowManager::setWindowAlwaysOnTop(QWidget* widget) {
    // No-op on other platforms
    Q_UNUSED(widget);
}

#endif