#include "backend/platform/macos/MacWindowManager.h"
#include "backend/platform/LocalScreenTopology.h"
#include "backend/managers/system/ScreenCoordinateMapping.h"
#include <algorithm>

#ifdef Q_OS_MACOS
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <QtGui/qscreen_platform.h>
#include <QVariant>

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

namespace {
char originalControlChildLevel;
NSArray<NSWindow*>* controlChildren(QWindow* qtWindow, NSWindow* window)
{
    NSMutableArray<NSWindow*>* children = [NSMutableArray array];
    for (NSWindow* child in [NSApp windows]) {
        if (child == window) continue;
        bool ownedPopup = false;
        bool sceneSurface = false;
        for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
            if (!candidate->handle()) continue;
            if (candidate->property("mouffetteSceneSurface").toBool()
                && nativeWindowFor(candidate) == child) {
                sceneSurface = true;
                break;
            }
            for (QWindow* owner = candidate->transientParent(); owner; owner = owner->transientParent()) {
                if (owner == qtWindow) {
                    ownedPopup = nativeWindowFor(candidate) == child;
                    break;
                }
            }
            if (ownedPopup) break;
        }
        if (!sceneSurface && ([child parentWindow] == window || [child sheetParent] == window
            || child == [NSApp modalWindow] || ownedPopup
            || [child isKindOfClass:[NSColorPanel class]])) [children addObject:child];
    }
    return children;
}
}

void MacWindowManager::configureControlWindow(QWindow* qtWindow, bool alwaysOnTop)
{
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window) return;
    if ([window isKindOfClass:[NSPanel class]]) {
        // Keep the panel nonactivating for its entire native lifetime. It can
        // become key and accept text without activating the application's old
        // desktop. Priority changes must never toggle this style bit.
        const auto style = [window styleMask] | NSWindowStyleMaskNonactivatingPanel;
        if ([window styleMask] != style) [window setStyleMask:style];
        [(NSPanel*)window setBecomesKeyOnlyIfNeeded:NO];
    }
    // Set collection behavior before the level: AppKit validates the combination.
    const bool fullscreen = qtWindow->windowState() == Qt::WindowFullScreen
        || ([window styleMask] & NSWindowStyleMaskFullScreen);
    NSWindowCollectionBehavior behavior = NSWindowCollectionBehaviorFullScreenPrimary
        | NSWindowCollectionBehaviorFullScreenDisallowsTiling
        | NSWindowCollectionBehaviorParticipatesInCycle;
    behavior |= fullscreen ? NSWindowCollectionBehaviorManaged : alwaysOnTop
        ? NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary
        : NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorManaged;
    if (@available(macOS 13.0, *)) {
        if (!fullscreen) behavior |= NSWindowCollectionBehaviorCanJoinAllApplications;
    }
    if ([window collectionBehavior] != behavior) [window setCollectionBehavior:behavior];
    // Interactive windows must remain below the WindowServer drag layer.
    const NSWindowLevel topmostLevel = NSPopUpMenuWindowLevel + 1;
    const NSWindowLevel level = alwaysOnTop && !fullscreen ? topmostLevel : NSNormalWindowLevel;
    if ([window level] != level) [window setLevel:level];
    [window setHidesOnDeactivate:NO];
    for (NSWindow* child in controlChildren(qtWindow, window)) {
        NSNumber* original = objc_getAssociatedObject(child, &originalControlChildLevel);
        if (alwaysOnTop) {
            if (!original) {
                NSWindowLevel baseline = [child level];
                // AppKit can inherit the parent's elevated level before we
                // observe a sheet/picker. Do not preserve that inherited
                // priority as the child's normal level when the option is off.
                if (baseline >= topmostLevel)
                    baseline = [child isKindOfClass:[NSPanel class]]
                        ? NSFloatingWindowLevel : NSNormalWindowLevel;
                objc_setAssociatedObject(child, &originalControlChildLevel,
                    @(baseline), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
            if ([child level] != level + 1) [child setLevel:level + 1];
        } else if (original) {
            [child setLevel:[original integerValue]];
            objc_setAssociatedObject(child, &originalControlChildLevel, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
    }
}

void MacWindowManager::setWindowAlwaysOnTop(QWindow* qtWindow)
{
    configureControlWindow(qtWindow, true);
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window || ![window isVisible] || [window isMiniaturized] || [NSApp isHidden]) return;
    [window orderFrontRegardless];
    for (NSWindow* child in controlChildren(qtWindow, window)) {
        if ([child isVisible]) [child orderFrontRegardless];
    }
}

bool MacWindowManager::isOnCurrentSpace(QWindow* qtWindow)
{
    NSWindow* window = qtWindow && qtWindow->handle() ? nativeWindowFor(qtWindow) : nil;
    return window ? [window isOnActiveSpace] : false;
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

void MacWindowManager::activateApplicationWindow(QWindow* qtWindow) {
    NSWindow* window = nativeWindowFor(qtWindow);
    if (!window) return;
    // Nonactivating control panels take keyboard focus in the current Space.
    // Activating their whole process would select an old main window/Space.
    // Ordinary NSWindows still need application activation to receive input.
    if (!([window styleMask] & NSWindowStyleMaskNonactivatingPanel))
        [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
    // A nonactivating panel can become key while remaining below the active
    // application's windows. Explicitly raise it across applications without
    // activating the process (which could switch away from the current Space).
    [window orderFrontRegardless];
    [window makeFirstResponder:[window contentView]];
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
