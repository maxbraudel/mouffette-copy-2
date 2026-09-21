#include "backend/platform/WindowCaptureExclusion.h"

#include <QGuiApplication>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QWindow>
#include <QtTest>
#include <memory>

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
    void onlyExplicitScenesAreCaptureExceptions()
    {
        auto& policy = WindowCaptureExclusion::instance();
        QWindow control;
        QWindow scene;
        QVERIFY(!policy.sceneWindows().contains(&control));
        QVERIFY(!policy.sceneWindows().contains(&scene));
        QSignalSpy changes(&policy, &WindowCaptureExclusion::sceneWindowsChanged);
        policy.setSceneWindow(&scene, true);
        QVERIFY(policy.sceneWindows().contains(&scene));
        QVERIFY(!policy.sceneWindows().contains(&control));
        QCOMPARE(changes.size(), 1);
        policy.setSceneWindow(&scene, true);
        QCOMPARE(changes.size(), 1);
        policy.setSceneWindow(&scene, false);
        QVERIFY(!policy.sceneWindows().contains(&scene));
        QCOMPARE(changes.size(), 2);
    }

    void destroyedAndRecreatedSceneSurfacesInvalidateExceptions()
    {
        auto& policy = WindowCaptureExclusion::instance();
        auto scene = std::make_unique<QWindow>();
        policy.setSceneWindow(scene.get(), true);
        QSignalSpy changes(&policy, &WindowCaptureExclusion::sceneWindowsChanged);
        scene->create();
        QVERIFY(!changes.isEmpty());
        changes.clear();
        scene->destroy();
        QVERIFY(!changes.isEmpty());
        QVERIFY(policy.sceneWindows().contains(scene.get()));
        changes.clear();
        scene->create();
        QVERIFY(!changes.isEmpty());
        scene.reset();
        QVERIFY(policy.sceneWindows().isEmpty());
    }

    void controlSurfaceDoesNotChangeMacSceneFilter()
    {
        auto& policy = WindowCaptureExclusion::instance();
        QSignalSpy changes(&policy, &WindowCaptureExclusion::sceneWindowsChanged);
        QWindow control;
        control.create();
        control.show();
        control.hide();
        control.destroy();
        QCOMPARE(changes.size(), 0);
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
        policy.setSceneWindow(&scene, true);
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
            QCOMPARE(affinity, DWORD(WDA_NONE));
            QVERIFY(!control.isVisible());
            control.destroy();
            scene.destroy();
        }
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
