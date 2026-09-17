#include "backend/platform/macos/MacWindowManager.h"
#include "backend/platform/LocalScreenTopology.h"
#include "backend/managers/system/ScreenCoordinateMapping.h"
#include <algorithm>

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

void MacWindowManager::configureControlWindow(QWindow* qtWindow)
{
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window) return;
    // Join ordinary desktops without changing Qt's native fullscreen policy.
    const auto behavior = ([window collectionBehavior] & ~NSWindowCollectionBehaviorMoveToActiveSpace)
        | NSWindowCollectionBehaviorCanJoinAllSpaces;
    if ([window collectionBehavior] != behavior) [window setCollectionBehavior:behavior];
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
    [window setCanHide:NO]; // Hiding the control application must not hide a live scene.
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

QList<LocalScreenTopology::Screen> MacWindowManager::screens(bool includeIdentity, bool* success)
{
    // Query WindowServer on every reconciliation, independently of delivery of
    // Qt screen notifications. An API failure is not an empty desktop.
    uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess) {
        if (success) *success = false;
        return {};
    }
    QList<CGDirectDisplayID> displays(count);
    if (count && CGGetActiveDisplayList(count, displays.data(), &count) != kCGErrorSuccess) {
        if (success) *success = false;
        return {};
    }
    displays.resize(count);
    QList<LocalScreenTopology::Screen> result;
    for (CGDirectDisplayID id : displays) {
        if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay) continue;
        LocalScreenTopology::Screen entry;
        const CGRect bounds = CGDisplayBounds(id);
        entry.geometry = QRect(qRound(bounds.origin.x), qRound(bounds.origin.y),
                               qRound(bounds.size.width), qRound(bounds.size.height));
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
        if (!mode) {
            if (success) *success = false;
            return {};
        }
        const qreal scale = CGDisplayModeGetWidth(mode) > 0
            ? std::max<qreal>(1.0, qreal(CGDisplayModeGetPixelWidth(mode)) / CGDisplayModeGetWidth(mode)) : 1.0;
        CGDisplayModeRelease(mode);
        entry.advertisedGeometry = ScreenCoordinateMapping::scaledScreenGeometry(entry.geometry, scale);
        entry.advertisedAvailableGeometry = entry.advertisedGeometry;
        entry.primary = id == CGMainDisplayID();
        for (QScreen* screen : QGuiApplication::screens()) {
            const auto* native = screen->nativeInterface<QNativeInterface::QCocoaScreen>();
            if (!native) continue;
            NSNumber* number = [[native->nativeScreen() deviceDescription] objectForKey:@"NSScreenNumber"];
            if (!number || [number unsignedIntValue] != id) continue;
            entry.screen = screen;
            break;
        }
        // Read current Cocoa work areas instead of QScreen's event-driven
        // cache. NSScreen coordinates start at the bottom-left; protocol/Qt
        // rectangles start at the top-left of the primary desktop.
        for (NSScreen* native in [NSScreen screens]) {
            NSNumber* number = [[native deviceDescription] objectForKey:@"NSScreenNumber"];
            if ([number unsignedIntValue] != id) continue;
            const NSRect full = [native frame], work = [native visibleFrame];
            const QRect available(entry.geometry.x() + qRound(NSMinX(work) - NSMinX(full)),
                                  entry.geometry.y() + qRound(NSMaxY(full) - NSMaxY(work)),
                                  qRound(work.size.width), qRound(work.size.height));
            entry.advertisedAvailableGeometry = ScreenCoordinateMapping::scaledScreenGeometry(available, scale);
            break;
        }
        if (includeIdentity) {
            CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(id);
            if (uuid) {
                CFStringRef value = CFUUIDCreateString(kCFAllocatorDefault, uuid);
                entry.identity = QStringLiteral("mac:") + QString::fromCFString(value);
                CFRelease(value);
                CFRelease(uuid);
            }
        }
        result.append(entry);
    }
    if (success) *success = true;
    return result;
}

void MacWindowManager::orderOutWindow(QWindow* qtWindow) {
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window) return;

    [window orderOut:nil];
}

#else

void MacWindowManager::setWindowAsGlobalOverlay(QWindow* window, bool clickThrough) {
    Q_UNUSED(window);
    Q_UNUSED(clickThrough);
}

void MacWindowManager::orderOutWindow(QWindow* window) {
    Q_UNUSED(window);
}

#endif
