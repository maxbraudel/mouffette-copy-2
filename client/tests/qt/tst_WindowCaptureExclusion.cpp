#include "backend/platform/WindowCaptureExclusion.h"

#include <QGuiApplication>
#include <QScopeGuard>
#include <QWindow>
#include <QtTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class WindowCaptureExclusionTest final : public QObject
{
    Q_OBJECT
private slots:
    void capturePreparationDoesNotCreateHiddenSurfaces()
    {
        QWindow control;
        QWindow scene;
        QString error;
        QVERIFY2(WindowCaptureExclusion::instance().prepareForCapture(&error), qPrintable(error));
        QVERIFY(!control.handle());
        QVERIFY(!scene.handle());
    }

    void windowsAffinityIsAppliedBeforeShowAndAfterRecreation()
    {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("Requires native Windows window affinity");
        auto& policy = WindowCaptureExclusion::instance();
        QWindow control;
        QWindow scene;
        // The getter requires layered windows; capture exclusion itself also
        // works on ordinary opaque control windows through the setter.
        control.setOpacity(0.95);
        scene.setOpacity(0.95);
        for (int generation = 0; generation < 2; ++generation) {
            control.create();
            scene.create();
            QString error;
            QVERIFY2(policy.prepareForCapture(&error), qPrintable(error));
            QVERIFY(policy.captureAllowed());
            DWORD affinity = 0;
            QVERIFY(GetWindowDisplayAffinity(reinterpret_cast<HWND>(control.winId()), &affinity));
            QCOMPARE(affinity, DWORD(0x11));
            QVERIFY(GetWindowDisplayAffinity(reinterpret_cast<HWND>(scene.winId()), &affinity));
            QCOMPARE(affinity, DWORD(0x11));
            QVERIFY(!control.isVisible());
            control.destroy();
            scene.destroy();
        }
#else
        QSKIP("Requires Windows");
#endif
    }

    void windowsCreatedDuringCaptureAreExcludedBeforeShow()
    {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("Requires native Windows window affinity");
        QString error;
        QVERIFY2(WindowCaptureExclusion::instance().prepareForCapture(&error), qPrintable(error));
        QWindow scene;
        scene.setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        scene.setOpacity(0.95);
        scene.create();
        DWORD affinity = WDA_NONE;
        QVERIFY(GetWindowDisplayAffinity(reinterpret_cast<HWND>(scene.winId()), &affinity));
        QCOMPARE(affinity, DWORD(0x11));
        QVERIFY(!scene.isVisible());
        // A dialog created by a scene has the same process exclusion policy.
        QWindow dialog;
        dialog.setTransientParent(&scene);
        dialog.setOpacity(0.95);
        dialog.create();
        QVERIFY(GetWindowDisplayAffinity(reinterpret_cast<HWND>(dialog.winId()), &affinity));
        QCOMPARE(affinity, DWORD(0x11));
        QVERIFY(WindowCaptureExclusion::instance().captureAllowed());
#else
        QSKIP("Requires Windows");
#endif
    }

    void nativeWindowIsExcludedWithoutQtEventDispatch()
    {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("Requires native Windows window affinity");
        QString error;
        QVERIFY2(WindowCaptureExclusion::instance().prepareForCapture(&error), qPrintable(error));
        const HWND window = CreateWindowExW(WS_EX_LAYERED, L"STATIC", L"Native dialog",
            WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        QVERIFY(window);
        const auto cleanup = qScopeGuard([&] { DestroyWindow(window); });
        DWORD affinity = WDA_NONE;
        QVERIFY(GetWindowDisplayAffinity(window, &affinity));
        QCOMPARE(affinity, DWORD(WDA_NONE));
        // Simulate a native modal loop: ShowWindow dispatches directly to its
        // native window procedure, without pumping the Qt event dispatcher.
        ShowWindow(window, SW_SHOWNOACTIVATE);
        QVERIFY(IsWindowVisible(window));
        QVERIFY(GetWindowDisplayAffinity(window, &affinity));
        QCOMPARE(affinity, DWORD(0x11));
        QVERIFY(WindowCaptureExclusion::instance().captureAllowed());
#else
        QSKIP("Requires Windows");
#endif
    }

    void hiddenNativeInfrastructureIsNotTreatedAsAControlWindow()
    {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("Requires native Windows window affinity");
        const HWND infrastructure = CreateWindowExW(WS_EX_LAYERED, L"STATIC", L"",
            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        QVERIFY(infrastructure);
        const auto cleanup = qScopeGuard([&] { DestroyWindow(infrastructure); });
        QVERIFY(SetWindowDisplayAffinity(infrastructure, WDA_MONITOR));
        QString error;
        QVERIFY2(WindowCaptureExclusion::instance().prepareForCapture(&error), qPrintable(error));
        DWORD affinity = WDA_NONE;
        QVERIFY(GetWindowDisplayAffinity(infrastructure, &affinity));
        QCOMPARE(affinity, DWORD(WDA_MONITOR));
#else
        QSKIP("Requires Windows");
#endif
    }
};

QTEST_MAIN(WindowCaptureExclusionTest)
#include "tst_WindowCaptureExclusion.moc"
