#include "backend/platform/macos/MacWindowManager.h"

#ifdef Q_OS_MACOS
#import <Cocoa/Cocoa.h>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <QtGui/qscreen_platform.h>

namespace {
NSWindow* nativeWindowFor(QWindow* qtWindow)
{
    // Offscreen/minimal platform plugins expose synthetic WIds which are not
    // Cocoa objects. This guard also keeps headless renderer tests native-safe.
    if (!qtWindow || QGuiApplication::platformName() != QLatin1String("cocoa")) {
        return nil;
    }
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(qtWindow->winId());
    return [view window];
}
}

void MacWindowManager::setWindowAlwaysOnTop(QWindow* qtWindow) {
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window) return;
    // Keep the interactive editor and its dialogs below the
    // WindowServer drag layer. NSScreenSaverWindowLevel (1000) is above that
    // layer (500): Finder can advertise a copy cursor yet never deliver Drop.
    // Leave room for owned dialogs too; they must remain valid drop targets.
    const NSWindowLevel controlLevel = NSPopUpMenuWindowLevel + 1;
    if ([window level] != controlLevel) [window setLevel:controlLevel];
    NSWindowCollectionBehavior behavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary |
        NSWindowCollectionBehaviorFullScreenDisallowsTiling |
        NSWindowCollectionBehaviorStationary |
        NSWindowCollectionBehaviorParticipatesInCycle;
    if (@available(macOS 13.0, *)) {
        behavior |= NSWindowCollectionBehaviorCanJoinAllApplications;
    }
    if ([window collectionBehavior] != behavior) [window setCollectionBehavior:behavior];
    if ([window hidesOnDeactivate]) [window setHidesOnDeactivate:NO];
    if (![window isVisible] || [window isMiniaturized] || [NSApp isHidden]) return;

    [window orderFrontRegardless];
    // Native sheets/pickers and Qt popup windows must remain usable above the
    // control window. Raising priority must never bury our own modal UI.
    for (NSWindow* child in [NSApp windows]) {
        if (child == window || ![child isVisible]) continue;
        bool ownedPopup = false;
        for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
            if (!candidate->isVisible()) continue;
            for (QWindow* owner = candidate->transientParent(); owner; owner = owner->transientParent()) {
                if (owner == qtWindow) {
                    ownedPopup = nativeWindowFor(candidate) == child;
                    break;
                }
            }
            if (ownedPopup) break;
        }
        if ([child parentWindow] == window || [child sheetParent] == window
            || child == [NSApp modalWindow] || ownedPopup
            || [child isKindOfClass:[NSColorPanel class]]) {
            if ([child level] != controlLevel + 1) [child setLevel:controlLevel + 1];
            [child orderFrontRegardless];
        }
    }
}

void MacWindowManager::configureGlobalOverlay(QWindow* qtWindow, bool clickThrough) {
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window) return;

    // Ensure borderless non-opaque window with clear background
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [window setHasShadow:NO];

    // Passive, click-through media can sit above the drag layer; the editor
    // must stay below it to keep receiving real Finder drops.
    [window setLevel:CGWindowLevelForKey(kCGScreenSaverWindowLevelKey)];

    // Make it present across Spaces and as auxiliary in full-screen; avoid Mission Control/App Exposé and window cycling
    NSWindowCollectionBehavior behavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                          NSWindowCollectionBehaviorFullScreenAuxiliary |
                                          NSWindowCollectionBehaviorFullScreenDisallowsTiling |
                                          NSWindowCollectionBehaviorStationary |
                                          NSWindowCollectionBehaviorIgnoresCycle;
    if (@available(macOS 13.0, *)) {
        behavior |= NSWindowCollectionBehaviorCanJoinAllApplications;
    }
    [window setCollectionBehavior:behavior];

    // Do not activate or take focus, optionally ignore mouse
    if ([window isKindOfClass:[NSPanel class]]) {
        NSPanel* panel = (NSPanel*)window;
        [panel setWorksWhenModal:YES];
        [panel setBecomesKeyOnlyIfNeeded:YES];
    }
    [window setHidesOnDeactivate:NO];
    [window setAcceptsMouseMovedEvents:NO];
    [window setIgnoresMouseEvents:clickThrough];
}

void MacWindowManager::setWindowAsGlobalOverlay(QWindow* qtWindow, bool clickThrough) {
    configureGlobalOverlay(qtWindow, clickThrough);
    NSWindow* window = nativeWindowFor(qtWindow);
    // A native orderFront call also shows a hidden window, bypassing Qt's
    // visibility state. PREPARE and retired surfaces must never be ordered in.
    if (window && qtWindow->isVisible() && [window isVisible]
        && ![window isMiniaturized]) [window orderFrontRegardless];
}

QString MacWindowManager::screenIdentity(QScreen* screen) {
    if (!screen || QGuiApplication::platformName() != QLatin1String("cocoa")) return {};
    const auto* native = screen->nativeInterface<QNativeInterface::QCocoaScreen>();
    if (!native) return {};
    NSNumber* number = [[native->nativeScreen() deviceDescription] objectForKey:@"NSScreenNumber"];
    if (!number) return {};
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID([number unsignedIntValue]);
    if (!uuid) return {};
    CFStringRef value = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    const QString identity = QString::fromCFString(value);
    CFRelease(value);
    CFRelease(uuid);
    return QStringLiteral("mac:") + identity;
}

void MacWindowManager::orderOutWindow(QWindow* qtWindow) {
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window) return;

    [window orderOut:nil];
}

void MacWindowManager::activateApplicationWindow(QWindow* qtWindow) {
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window) return;
    [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
}

#else

void MacWindowManager::setWindowAlwaysOnTop(QWindow* window) {
    Q_UNUSED(window);
}

void MacWindowManager::setWindowAsGlobalOverlay(QWindow* window, bool clickThrough) {
    Q_UNUSED(window);
    Q_UNUSED(clickThrough);
}

void MacWindowManager::orderOutWindow(QWindow* window) {
    Q_UNUSED(window);
}

void MacWindowManager::activateApplicationWindow(QWindow* window) {
    Q_UNUSED(window);
}

#endif
