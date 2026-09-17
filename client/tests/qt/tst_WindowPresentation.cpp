#include <QtTest>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QQmlComponent>
#include <QQmlEngine>

#include "frontend/qml/WindowPresentation.h"

#ifdef Q_OS_MACOS
#import <Cocoa/Cocoa.h>
#include "backend/platform/macos/MacWindowManager.h"
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#endif

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
        QVERIFY(window.flags().testFlag(Qt::WindowTitleHint));
        QVERIFY(window.flags().testFlag(Qt::WindowSystemMenuHint));
        QVERIFY(window.flags().testFlag(Qt::WindowMinimizeButtonHint));
        QVERIFY(window.flags().testFlag(Qt::WindowMaximizeButtonHint));
        QVERIFY(window.flags().testFlag(Qt::WindowCloseButtonHint));
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
        window->showMinimized();
        presentation->open();
        QTRY_COMPARE(window->windowState(), Qt::WindowNoState);
        QTRY_COMPARE(window->geometry(), WindowPresentation::openingGeometry(
            screen->availableGeometry(), window->frameMargins()));
        window->hide();
    }

    void nativePrioritySurvivesDemotionAndRecreation()
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
        QTRY_COMPARE([native() level], NSScreenSaverWindowLevel);
        const auto behavior = [native() collectionBehavior];
        QVERIFY(behavior & NSWindowCollectionBehaviorCanJoinAllSpaces);
        QVERIFY(behavior & NSWindowCollectionBehaviorFullScreenAuxiliary);
        QVERIFY(behavior & NSWindowCollectionBehaviorStationary);
        if (@available(macOS 13.0, *)) {
            QVERIFY(behavior & NSWindowCollectionBehaviorCanJoinAllApplications);
        }
        QVERIFY(![native() hidesOnDeactivate]);
        [native() setLevel:NSNormalWindowLevel];
        [native() setCollectionBehavior:NSWindowCollectionBehaviorDefault];
        QTRY_COMPARE([native() level], NSScreenSaverWindowLevel);
        QVERIFY([native() collectionBehavior] & NSWindowCollectionBehaviorCanJoinAllSpaces);

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
        QTRY_VERIFY([nativeDialog level] > [native() level]);
        QVERIFY([nativeOverlay level] < [native() level]);
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
        QTRY_COMPARE([native() level], NSScreenSaverWindowLevel);
        QVERIFY([native() collectionBehavior] & NSWindowCollectionBehaviorCanJoinAllSpaces);
#else
        QTRY_VERIFY(GetWindowLongPtr(native(), GWL_EXSTYLE) & WS_EX_TOPMOST);
#endif
        window.hide();
#else
        QSKIP("Native priority regression covers macOS and Windows");
#endif
    }
};

QTEST_MAIN(WindowPresentationTest)
#include "tst_WindowPresentation.moc"
