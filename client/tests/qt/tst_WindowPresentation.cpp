#include <QtTest>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QProcess>
#include <QScopeGuard>
#include "backend/platform/LocalScreenTopology.h"
#include "backend/managers/system/ScreenCoordinateMapping.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickView>
#include <QQuickItem>

#include "frontend/qml/WindowPresentation.h"
#include "backend/platform/WindowStackingCoordinator.h"

#ifdef Q_OS_MACOS
#import <Cocoa/Cocoa.h>
#include "backend/platform/macos/MacWindowManager.h"
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#endif

namespace {
#ifdef Q_OS_MACOS
bool nativeAbove(CGWindowID front, CGWindowID behind)
{
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list) return false;
    bool foundFront = false;
    bool ordered = false;
    for (NSDictionary* info in (__bridge NSArray*)list) {
        const auto number = [info[(__bridge NSString*)kCGWindowNumber] unsignedIntValue];
        if (number == front) foundFront = true;
        if (number == behind) { ordered = foundFront; break; }
    }
    CFRelease(list);
    return ordered;
}
#endif

bool nativeAbove(QWindow& front, QWindow& behind)
{
#ifdef Q_OS_MACOS
    const auto nativeFront = [(__bridge NSView*)reinterpret_cast<void*>(front.winId()) window];
    const auto nativeBehind = [(__bridge NSView*)reinterpret_cast<void*>(behind.winId()) window];
    return nativeAbove([nativeFront windowNumber], [nativeBehind windowNumber]);
#elif defined(Q_OS_WIN)
    const HWND frontHandle = reinterpret_cast<HWND>(front.winId());
    const HWND behindHandle = reinterpret_cast<HWND>(behind.winId());
    bool foundFront = false;
    for (HWND handle = GetTopWindow(nullptr); handle; handle = GetWindow(handle, GW_HWNDNEXT)) {
        if (handle == frontHandle) foundFront = true;
        if (handle == behindHandle) return foundFront;
    }
    return false;
#else
    Q_UNUSED(front);
    Q_UNUSED(behind);
    return false;
#endif
}
}

class WindowPresentationTest : public QObject
{
    Q_OBJECT
private slots:
    void geometry_data()
    {
        QTest::addColumn<QRect>("available");
        QTest::addColumn<QMargins>("margins");
        QTest::addColumn<QRect>("expected");
        QTest::newRow("mac-menu-and-dock") << QRect(0, 25, 1440, 815)
            << QMargins(0, 28, 0, 0) << QRect(72, 93, 1296, 706);
        QTest::newRow("windows-left-monitor") << QRect(-1920, 0, 1920, 1040)
            << QMargins(8, 31, 8, 8) << QRect(-1816, 83, 1712, 897);
        QTest::newRow("logical-pixels-on-retina") << QRect(1512, -900, 1280, 760)
            << QMargins(0, 28, 0, 0) << QRect(1576, -834, 1152, 656);
    }

    void geometry()
    {
        QFETCH(QRect, available);
        QFETCH(QMargins, margins);
        QFETCH(QRect, expected);
        QCOMPARE(WindowPresentation::openingGeometry(available, margins), expected);
    }

    void nativeTitleBarRemainsMovable()
    {
        QWindow window;
        WindowPresentation presentation;
        presentation.setWindow(&window);
        // Adding only WindowStaysOnTopHint bypasses Qt's default Windows title
        // bar hints: the resize border survives, but there is no drag surface.
#ifndef Q_OS_MACOS
        QVERIFY(window.flags().testFlag(Qt::WindowTitleHint));
        QVERIFY(window.flags().testFlag(Qt::WindowSystemMenuHint));
        QVERIFY(window.flags().testFlag(Qt::WindowMinimizeButtonHint));
        QVERIFY(window.flags().testFlag(Qt::WindowMaximizeButtonHint));
        QVERIFY(window.flags().testFlag(Qt::WindowCloseButtonHint));
#else
        QCOMPARE(window.type(), Qt::Window);
        QVERIFY(!window.flags().testFlag(Qt::CustomizeWindowHint));
#endif
        QVERIFY(window.flags().testFlag(Qt::WindowStaysOnTopHint));

#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            QSKIP("Native caption hit testing requires Windows");
        }
        presentation.open();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        QCOMPARE(style & WS_CAPTION, LONG_PTR(WS_CAPTION));
        QVERIFY(style & WS_THICKFRAME);
        QVERIFY(style & WS_SYSMENU);
        const HMENU menu = GetSystemMenu(hwnd, FALSE);
        QVERIFY(menu);
        const UINT moveState = GetMenuState(menu, SC_MOVE, MF_BYCOMMAND);
        QVERIFY(moveState != UINT(-1));
        QVERIFY(!(moveState & (MF_DISABLED | MF_GRAYED)));

        // Use native screen pixels, so this also exercises scaled displays.
        TITLEBARINFO titleBar{};
        titleBar.cbSize = sizeof(titleBar);
        QVERIFY(GetTitleBarInfo(hwnd, &titleBar));
        const POINT caption = {
            (titleBar.rcTitleBar.left + titleBar.rcTitleBar.right) / 2,
            (titleBar.rcTitleBar.top + titleBar.rcTitleBar.bottom) / 2
        };
        QCOMPARE(SendMessage(hwnd, WM_NCHITTEST, 0, MAKELPARAM(caption.x, caption.y)),
                 LRESULT(HTCAPTION));
        window.hide();
#elif defined(Q_OS_MACOS)
        if (QGuiApplication::platformName() != QLatin1String("cocoa")) {
            QSKIP("Native title bar metrics require Cocoa");
        }
        QWindow reference;
        reference.setFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                           | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
        reference.resize(400, 300);
        reference.create();
        const auto standard = [(__bridge NSView*)reinterpret_cast<void*>(reference.winId()) window];
        const auto titleBarHeight = [](NSWindow* native) {
            return NSHeight(native.frame) - NSHeight([native contentRectForFrameRect:native.frame]);
        };
        // Compare with AppKit's current standard metrics, not hardcoded pixels.
        // Changing priority rewrites Qt's native flags; recreation starts fresh.
        for (bool alwaysOnTop : {true, false}) {
            presentation.setAlwaysOnTop(alwaysOnTop);
            for (int opening = 0; opening < 2; ++opening) {
                presentation.open();
                QVERIFY(QTest::qWaitForWindowExposed(&window));
                const auto native = [(__bridge NSView*)reinterpret_cast<void*>(window.winId()) window];
                for (NSWindowButton type : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
                    NSButton* actual = [native standardWindowButton:type];
                    NSButton* expected = [standard standardWindowButton:type];
                    QVERIFY(actual && expected);
                    QVERIFY(!actual.hidden && actual.enabled);
                    if (type == NSWindowZoomButton)
                        QVERIFY(native.accessibilityFullScreenButton != nil);
                    QCOMPARE(actual.frame.size.width, expected.frame.size.width);
                    QCOMPARE(actual.frame.size.height, expected.frame.size.height);
                }
                QCOMPARE(titleBarHeight(native), titleBarHeight(standard));
                QVERIFY(native.collectionBehavior & NSWindowCollectionBehaviorCanJoinAllSpaces);
                QVERIFY(!(native.collectionBehavior & NSWindowCollectionBehaviorMoveToActiveSpace));
                QVERIFY(![native isKindOfClass:[NSPanel class]]);
                QVERIFY(!(native.styleMask & NSWindowStyleMaskNonactivatingPanel));
                QVERIFY(!(native.styleMask & NSWindowStyleMaskUtilityWindow));
                window.hide();
                window.destroy();
            }
        }
#endif
    }

    void greenButtonEntersNativeFullscreen_data()
    {
        QTest::addColumn<bool>("alwaysOnTop");
        QTest::newRow("topmost") << true;
        QTest::newRow("normal") << false;
    }

    void greenButtonEntersNativeFullscreen()
    {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != QLatin1String("cocoa"))
            QSKIP("Requires native macOS fullscreen");
        QFETCH(bool, alwaysOnTop);
        QWindow window;
        WindowPresentation presentation;
        presentation.setAlwaysOnTop(alwaysOnTop);
        presentation.setWindow(&window);
        presentation.open();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(700);
        const auto native = [(__bridge NSView*)reinterpret_cast<void*>(window.winId()) window];
        __block bool entered = false;
        __block bool exited = false;
        id enterObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidEnterFullScreenNotification object:native queue:nil
            usingBlock:^(NSNotification*) { entered = true; }];
        id exitObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidExitFullScreenNotification object:native queue:nil
            usingBlock:^(NSNotification*) { exited = true; }];
        const auto cleanup = qScopeGuard([&] {
            if (native.styleMask & NSWindowStyleMaskFullScreen) {
                window.showNormal();
                QTest::qWait(1000);
            }
            [[NSNotificationCenter defaultCenter] removeObserver:enterObserver];
            [[NSNotificationCenter defaultCenter] removeObserver:exitObserver];
        });
        const QRect geometry = window.geometry();
        NSButton* green = [native standardWindowButton:NSWindowZoomButton];
        QVERIFY(green && green.enabled);
        QVERIFY(native.accessibilityFullScreenButton != nil);
        QVERIFY(native.collectionBehavior & NSWindowCollectionBehaviorFullScreenPrimary);
        [green performClick:nil];
        QTRY_VERIFY_WITH_TIMEOUT(entered, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(window.windowState(), Qt::WindowFullScreen, 8000);
        QVERIFY(native.styleMask & NSWindowStyleMaskFullScreen);
        QTRY_COMPARE(window.size(), window.screen()->geometry().size());
        presentation.setAlwaysOnTop(!alwaysOnTop);
        QTest::qWait(650);
        QCOMPARE(window.windowState(), Qt::WindowFullScreen);
        QVERIFY(native.styleMask & NSWindowStyleMaskFullScreen);
        [[native standardWindowButton:NSWindowZoomButton] performClick:nil];
        QTRY_VERIFY_WITH_TIMEOUT(exited, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(window.windowState(), Qt::WindowNoState, 8000);
        QVERIFY(!(native.styleMask & NSWindowStyleMaskFullScreen));
        QTRY_COMPARE(window.geometry(), geometry);
        WindowStackingCoordinator::instance().enforce();
        QVERIFY(native.collectionBehavior & NSWindowCollectionBehaviorFullScreenPrimary);
        QVERIFY(!(native.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary));
        QVERIFY(native.collectionBehavior & NSWindowCollectionBehaviorCanJoinAllSpaces);
        // WindowServer retires the fullscreen Space after AppKit reports exit.
        // Keep its owner alive until that desktop transition has settled.
        QTest::qWait(1200);
        window.hide();
#else
        QSKIP("macOS traffic lights regression");
#endif
    }

    void qmlBindingAndReopen()
    {
        qmlRegisterType<WindowPresentation>("Mouffette.WindowTest", 1, 0, "WindowPresentation");
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import Mouffette.WindowTest
            Window {
                id: window
                visible: false
                property WindowPresentation presentation: WindowPresentation { window: window }
            }
        )", QUrl());
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* window = qobject_cast<QWindow*>(object.data());
        auto* presentation = qvariant_cast<WindowPresentation*>(object->property("presentation"));
        QVERIFY(window);
        QVERIFY(presentation);
        QCOMPARE(presentation->window(), window);
        QVERIFY(window->flags().testFlag(Qt::WindowStaysOnTopHint));
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);

        presentation->open();
        QTRY_VERIFY(window->isVisible());
        QTRY_COMPARE(window->screen(), screen);
        QTRY_COMPARE(window->geometry(), WindowPresentation::openingGeometry(
            screen->availableGeometry(), window->frameMargins()));
        QCOMPARE(window->windowState(), Qt::WindowNoState);

        // Neither priority enforcement nor raising an open window should undo
        // the user's movement/resizing. Wait across an enforcement tick.
        window->resize(520, 360);
        const QPoint movedPosition = window->position() + QPoint(12, 8);
        window->setPosition(movedPosition);
        QTest::qWait(650);
        QCOMPARE(window->position(), movedPosition);
        presentation->open();
        QCOMPARE(window->size(), QSize(520, 360));
        QCOMPARE(window->position(), movedPosition);

        window->hide();
        QTest::qWait(650);
        QVERIFY(!window->isVisible()); // enforcement must not resurrect it
        presentation->open();
        QTRY_COMPARE(window->geometry(), WindowPresentation::openingGeometry(
            screen->availableGeometry(), window->frameMargins()));
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == QLatin1String("cocoa")) {
            const auto native = [(__bridge NSView*)reinterpret_cast<void*>(window->winId()) window];
            QVERIFY([native styleMask] & NSWindowStyleMaskMiniaturizable);
            [native miniaturize:nil]; // Exercise the actual native title-bar action.
            QTRY_VERIFY([native isMiniaturized]);
            QTRY_COMPARE(window->windowState(), Qt::WindowMinimized);
        } else window->showMinimized();
#else
        window->showMinimized();
#endif
        presentation->open();
        QTRY_COMPARE(window->windowState(), Qt::WindowNoState);
        QTRY_COMPARE(window->geometry(), WindowPresentation::openingGeometry(
            screen->availableGeometry(), window->frameMargins()));
        window->hide();
    }

    void raisesAboveAnotherApplicationWithoutAlwaysOnTop()
    {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != QLatin1String("cocoa")) QSKIP("Requires native ordering");
        QWindow window;
        WindowPresentation presentation;
        presentation.setAlwaysOnTop(false);
        presentation.setWindow(&window);
        presentation.open();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto native = [(__bridge NSView*)reinterpret_cast<void*>(window.winId()) window];
        const CGWindowID windowId = [native windowNumber];
        const QRect geometry = window.geometry();

        QProcess coveringApp;
        coveringApp.start(QCoreApplication::applicationDirPath() + QStringLiteral("/tst_FullscreenHost"),
                          {QStringLiteral("--windowed")});
        const auto cleanup = qScopeGuard([&] {
            window.hide();
            coveringApp.write("QUIT\n");
            coveringApp.waitForBytesWritten(1000);
            if (!coveringApp.waitForFinished(3000)) {
                coveringApp.kill();
                coveringApp.waitForFinished(3000);
            }
        });
        QVERIFY(coveringApp.waitForStarted(3000));
        QTRY_VERIFY_WITH_TIMEOUT(coveringApp.canReadLine(), 3000);
        const QByteArray ready = coveringApp.readLine().trimmed();
        QVERIFY2(ready.startsWith("READY "), ready.constData());
        const CGWindowID coveringId = ready.mid(6).toUInt();
        QVERIFY(coveringId > 0);

        for (int attempt = 0; attempt < 2; ++attempt) {
            coveringApp.write("RAISE\n");
            QVERIFY(coveringApp.waitForBytesWritten(1000));
            QTRY_VERIFY_WITH_TIMEOUT(nativeAbove(coveringId, windowId), 3000);
            QTRY_COMPARE(qint64([[[NSWorkspace sharedWorkspace] frontmostApplication] processIdentifier]),
                         coveringApp.processId());

            presentation.open();
            QTRY_VERIFY_WITH_TIMEOUT(nativeAbove(windowId, coveringId), 2000);
            QTRY_VERIFY([native isKeyWindow]);
            QCOMPARE([native level], NSNormalWindowLevel);
            QCOMPARE(window.geometry(), geometry);
            QVERIFY(window.isVisible());
            presentation.open(); // Repeated tray clicks must leave it in front.
            QTest::qWait(650);
            QVERIFY(nativeAbove(windowId, coveringId));
            QVERIFY([native isKeyWindow]);
            QVERIFY(window.isVisible());
        }
#else
        QSKIP("macOS cross-application ordering regression");
#endif
    }

    void nativeMacInventoryMatchesAdvertisedCoordinates()
    {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != QLatin1String("cocoa")) QSKIP("Requires native screens");
        bool valid = false;
        const auto screens = MacWindowManager::screens(false, &valid);
        QVERIFY(valid);
        for (const auto& screen : screens) {
            QVERIFY(screen.screen);
            QCOMPARE(screen.advertisedGeometry, ScreenCoordinateMapping::scaledScreenGeometry(
                screen.screen->geometry(), screen.screen->devicePixelRatio()));
            QCOMPARE(screen.advertisedAvailableGeometry, ScreenCoordinateMapping::scaledScreenGeometry(
                screen.screen->availableGeometry(), screen.screen->devicePixelRatio()));
        }
#endif
    }

    void normalModeSurvivesEnforcementAndRecreation()
    {
        QWindow window;
        WindowPresentation presentation;
        presentation.setWindow(&window);
        presentation.open();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QWindow dialog;
        dialog.setTransientParent(&window);
        dialog.show();
        WindowStackingCoordinator::instance().enforce();
        const QRect geometry = window.geometry();
        presentation.setAlwaysOnTop(false);
        QVERIFY(!window.flags().testFlag(Qt::WindowStaysOnTopHint));
        QCOMPARE(window.geometry(), geometry);
        auto verifyNormal = [&] {
            WindowStackingCoordinator::instance().enforce();
#ifdef Q_OS_MACOS
            if (QGuiApplication::platformName() != QLatin1String("cocoa")) return;
            const auto native = [(__bridge NSView*)reinterpret_cast<void*>(window.winId()) window];
            const auto child = [(__bridge NSView*)reinterpret_cast<void*>(dialog.winId()) window];
            QCOMPARE([native level], NSNormalWindowLevel);
            QVERIFY([child level] < NSPopUpMenuWindowLevel);
            QVERIFY([native collectionBehavior] & NSWindowCollectionBehaviorCanJoinAllSpaces);
            QVERIFY([native collectionBehavior] & NSWindowCollectionBehaviorFullScreenPrimary);
#elif defined(Q_OS_WIN)
            if (QGuiApplication::platformName() != QLatin1String("windows")) return;
            QVERIFY(!(GetWindowLongPtr(reinterpret_cast<HWND>(window.winId()), GWL_EXSTYLE) & WS_EX_TOPMOST));
#endif
        };
        verifyNormal();
        QTest::qWait(550);
        verifyNormal();
        dialog.hide();
        window.hide();
        window.destroy();
        presentation.open();
        verifyNormal();
        presentation.setAlwaysOnTop(true);
        QVERIFY(window.flags().testFlag(Qt::WindowStaysOnTopHint));
        presentation.setAlwaysOnTop(false);
        verifyNormal();
        window.hide();
    }

    void nativePrioritySurvivesRecreation()
    {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        if (QGuiApplication::platformName() == QLatin1String("offscreen")
            || QGuiApplication::platformName() == QLatin1String("minimal")) {
            QSKIP("Requires the native window system");
        }
        QWindow window;
        WindowPresentation presentation;
        presentation.setWindow(&window);
        presentation.open();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
#ifdef Q_OS_MACOS
        const auto native = [&window] {
            return [(__bridge NSView*)reinterpret_cast<void*>(window.winId()) window];
        };
        const NSWindowLevel expectedLevel = [native() level];
        QVERIFY(expectedLevel > NSNormalWindowLevel);
        const auto behavior = [native() collectionBehavior];
        QVERIFY(behavior & NSWindowCollectionBehaviorFullScreenPrimary);
        QVERIFY(!(behavior & NSWindowCollectionBehaviorFullScreenAuxiliary));
        QVERIFY(behavior & NSWindowCollectionBehaviorCanJoinAllSpaces);

        QWindow remoteOverlay;
        remoteOverlay.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        remoteOverlay.show();
        MacWindowManager::setWindowAsGlobalOverlay(&remoteOverlay);
        QWindow dialog;
        dialog.setTransientParent(&window);
        dialog.setModality(Qt::WindowModal);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const auto nativeDialog = [(__bridge NSView*)reinterpret_cast<void*>(dialog.winId()) window];
        const auto nativeOverlay = [(__bridge NSView*)reinterpret_cast<void*>(remoteOverlay.winId()) window];
        QTRY_VERIFY([nativeDialog level] >= [native() level]);
        QVERIFY([nativeOverlay level] > [nativeDialog level]);
        dialog.hide();
        remoteOverlay.hide();
#else
        const auto native = [&window] { return reinterpret_cast<HWND>(window.winId()); };
        QTRY_VERIFY(GetWindowLongPtr(native(), GWL_EXSTYLE) & WS_EX_TOPMOST);
        SetWindowPos(native(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        QTRY_VERIFY(GetWindowLongPtr(native(), GWL_EXSTYLE) & WS_EX_TOPMOST);
#endif
        window.hide();
        window.destroy();
        presentation.open();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
#ifdef Q_OS_MACOS
        QTRY_COMPARE([native() level], expectedLevel);
        QVERIFY([native() collectionBehavior] & NSWindowCollectionBehaviorCanJoinAllSpaces);
#else
        QTRY_VERIFY(GetWindowLongPtr(native(), GWL_EXSTYLE) & WS_EX_TOPMOST);
#endif
        window.hide();
#else
        QSKIP("Native priority regression covers macOS and Windows");
#endif
    }

    void scenesStayAboveControlDialogsAndRecreation()
    {
        if (QGuiApplication::platformName() != QLatin1String("cocoa")
            && QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("Requires a native desktop");
        QWindow control;
        WindowPresentation presentation;
        presentation.setWindow(&control);
        presentation.open();
        QVERIFY(QTest::qWaitForWindowExposed(&control));
        QWindow scene;
        scene.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        scene.setGeometry(20, 20, 160, 120);
        auto& stacking = WindowStackingCoordinator::instance();
        stacking.registerSceneWindow(&scene);
        stacking.enforce();
        QVERIFY(!scene.isVisible());
#ifdef Q_OS_MACOS
        const auto nativeScene = [&scene] {
            return [(__bridge NSView*)reinterpret_cast<void*>(scene.winId()) window];
        };
        QVERIFY(![nativeScene() isVisible]);
        MacWindowManager::setWindowAsGlobalOverlay(&scene);
        QVERIFY(![nativeScene() isVisible]);
#endif
        scene.show();
        stacking.setSceneWindowActive(&scene, true);
        QVERIFY(QTest::qWaitForWindowExposed(&scene));
        QVERIFY(!scene.isActive());

        QWindow dialog;
        dialog.setTransientParent(&control);
        dialog.setModality(Qt::WindowModal);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const auto ordered = [&]() {
#ifdef Q_OS_MACOS
            auto* nativeControl = [(__bridge NSView*)reinterpret_cast<void*>(control.winId()) window];
            auto* nativeDialog = [(__bridge NSView*)reinterpret_cast<void*>(dialog.winId()) window];
            return [nativeScene() level] == CGWindowLevelForKey(kCGScreenSaverWindowLevelKey)
                && [nativeScene() level] > [nativeDialog level]
                && [nativeDialog level] >= [nativeControl level]
                && [nativeDialog level] < CGWindowLevelForKey(kCGDraggingWindowLevelKey)
                && [nativeScene() ignoresMouseEvents] && ![nativeScene() isKeyWindow]
                && nativeAbove(scene, dialog) && nativeAbove(dialog, control);
#elif defined(Q_OS_WIN)
            const HWND overlay = reinterpret_cast<HWND>(scene.winId());
            const HWND main = reinterpret_cast<HWND>(control.winId());
            const HWND popup = reinterpret_cast<HWND>(dialog.winId());
            if (!(GetWindowLongPtr(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST)) return false;
            bool sawScene = false;
            bool sawDialog = false;
            for (HWND handle = GetTopWindow(nullptr); handle; handle = GetWindow(handle, GW_HWNDNEXT)) {
                if (handle == overlay) sawScene = true;
                if (handle == popup) { if (!sawScene) return false; sawDialog = true; }
                if (handle == main) return sawScene && sawDialog;
            }
            return false;
#else
            return true;
#endif
        };
        QTRY_VERIFY(ordered());
        presentation.open();
        dialog.raise();
        dialog.requestActivate();
        QTRY_VERIFY(ordered());
        QTest::qWait(1100); // Multiple enforcement ticks, not just initial show.
#ifdef Q_OS_MACOS
        if (!ordered()) {
            auto* main = [(__bridge NSView*)reinterpret_cast<void*>(control.winId()) window];
            auto* popup = [(__bridge NSView*)reinterpret_cast<void*>(dialog.winId()) window];
            qWarning() << "Native ordering failure; levels/visible/activeSpace/key:"
                << [nativeScene() level] << [nativeScene() isVisible] << [nativeScene() isOnActiveSpace] << [nativeScene() isKeyWindow]
                << [popup level] << [popup isVisible] << [popup isOnActiveSpace] << [popup isKeyWindow]
                << [main level] << [main isVisible] << [main isOnActiveSpace] << [main isKeyWindow]
                << "ordered pairs" << nativeAbove(scene, dialog) << nativeAbove(dialog, control);
        }
#endif
        QVERIFY(ordered());
        QWindow contender;
        contender.setFlags(Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        contender.setGeometry(30, 30, 100, 100);
        contender.show();
        QVERIFY(QTest::qWaitForWindowExposed(&contender));
#ifdef Q_OS_MACOS
        auto* nativeContender = [(__bridge NSView*)reinterpret_cast<void*>(contender.winId()) window];
        // Model an independent global overlay: the control panel can be key
        // while this process is inactive, so a default tool would auto-hide.
        [nativeContender setHidesOnDeactivate:NO];
        [nativeContender setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces
            | NSWindowCollectionBehaviorFullScreenAuxiliary];
        [nativeContender setLevel:CGWindowLevelForKey(kCGScreenSaverWindowLevelKey)];
        [nativeContender orderFrontRegardless];
#elif defined(Q_OS_WIN)
        SetWindowPos(reinterpret_cast<HWND>(contender.winId()), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        QVERIFY(GetWindowLongPtr(reinterpret_cast<HWND>(scene.winId()), GWL_EXSTYLE) & WS_EX_TRANSPARENT);
#endif
        QTRY_VERIFY(nativeAbove(scene, contender));
        contender.hide();
#ifdef Q_OS_MACOS
        const auto behavior = [nativeScene() collectionBehavior];
        QVERIFY(behavior & NSWindowCollectionBehaviorCanJoinAllSpaces);
        QVERIFY(behavior & NSWindowCollectionBehaviorFullScreenAuxiliary);
        QVERIFY(behavior & NSWindowCollectionBehaviorStationary);
        if (@available(macOS 13.0, *)) {
            QVERIFY(behavior & NSWindowCollectionBehaviorCanJoinAllApplications);
        }
        QVERIFY(![nativeScene() canHide]);
        [NSApp hide:nil];
        QTest::qWait(50);
        QVERIFY([nativeScene() isVisible]);
        [NSApp unhideWithoutActivation];
        [nativeScene() setLevel:NSNormalWindowLevel];
#elif defined(Q_OS_WIN)
        SetWindowPos(reinterpret_cast<HWND>(scene.winId()), HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
        QTRY_VERIFY(ordered());
        scene.hide();
        scene.destroy();
        scene.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene));
        QTRY_VERIFY(ordered());
        QVERIFY(scene.flags().testFlag(Qt::WindowTransparentForInput));
        QVERIFY(scene.flags().testFlag(Qt::WindowDoesNotAcceptFocus));
        stacking.unregisterWindow(&scene);
        scene.hide();
        scene.destroy();
        QTest::qWait(600);
        QVERIFY(!scene.isVisible());
        QVERIFY(!scene.handle()); // No stale enforcement recreates the surface.
    }

    void nativePriorityAllowsFinderDrops()
    {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != QLatin1String("cocoa"))
            QSKIP("Requires Cocoa window levels");
        QWindow window;
        WindowPresentation presentation;
        presentation.setWindow(&window);
        presentation.open();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto native = [(__bridge NSView*)reinterpret_cast<void*>(window.winId()) window];
        const auto draggingLevel = CGWindowLevelForKey(kCGDraggingWindowLevelKey);
        // WindowServer routes external drops only at/below the dragging level.
        // Sending synthetic QDropEvents directly to Qt bypasses this boundary.
        QVERIFY2([native level] < draggingLevel,
                 "The interactive canvas must stay below the native drag layer");
        QVERIFY([native level] > NSNormalWindowLevel);
        QTest::qWait(650);
        QVERIFY([native level] < draggingLevel);

        QWindow dialog;
        dialog.setTransientParent(&window);
        dialog.setModality(Qt::WindowModal);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const auto nativeDialog = [(__bridge NSView*)reinterpret_cast<void*>(dialog.winId()) window];
        QTRY_VERIFY([nativeDialog level] >= [native level]);
        QVERIFY([nativeDialog level] < draggingLevel);
#else
        QSKIP("Finder's native drop routing is macOS-specific");
#endif
    }
};

QTEST_MAIN(WindowPresentationTest)
#include "tst_WindowPresentation.moc"
