#include <QApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QSystemTrayIcon>
#include <QDebug>
#include <QCoreApplication>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QMediaFormat>
#include <QThreadPool>
#include <cstdio>
#include "AppBuildConfig.h"
#include "backend/config/AppConfig.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/managers/system/SystemLifecycleMonitor.h"
#include "backend/runtime/ApplicationInstanceManager.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/RuntimeStorageBootstrap.h"
#include "backend/runtime/storage/StorageRegistry.h"
#include "frontend/qml/ApplicationController.h"
#include "frontend/qml/QmlRuntime.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "backend/audiosharing/AudioWorker.h"
#include "backend/audiosharing/AudioWorkerClient.h"
#include "backend/audiosharing/AudioWorkerPaths.h"

// ── Dev flags ────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

namespace {
void logRuntimeDiagnostics() {
    if (!AppConfig::instance().runtimeDiagnostics()) {
        return;
    }
    const auto apiToString = [](QSGRendererInterface::GraphicsApi api) {
        switch (api) {
        case QSGRendererInterface::Unknown: return QStringLiteral("Unknown");
        case QSGRendererInterface::Software: return QStringLiteral("Software");
        case QSGRendererInterface::OpenVG: return QStringLiteral("OpenVG");
        case QSGRendererInterface::OpenGL: return QStringLiteral("OpenGL");
        case QSGRendererInterface::Direct3D11: return QStringLiteral("Direct3D11");
        case QSGRendererInterface::Vulkan: return QStringLiteral("Vulkan");
        case QSGRendererInterface::Metal: return QStringLiteral("Metal");
        case QSGRendererInterface::Null: return QStringLiteral("Null");
        default: return QStringLiteral("Other");
        }
    };

    qInfo() << "[Runtime] Qt library paths:" << QCoreApplication::libraryPaths();
    qInfo() << "[Runtime] QML2_IMPORT_PATH=" << qEnvironmentVariable("QML2_IMPORT_PATH");
    qInfo() << "[Runtime] QML_IMPORT_PATH=" << qEnvironmentVariable("QML_IMPORT_PATH");
    qInfo() << "[Runtime] QT_PLUGIN_PATH=" << qEnvironmentVariable("QT_PLUGIN_PATH");
    qInfo() << "[Runtime] QT_MEDIA_BACKEND=" << qEnvironmentVariable("QT_MEDIA_BACKEND");
    qInfo() << "[Runtime] QSG_RHI_BACKEND=" << qEnvironmentVariable("QSG_RHI_BACKEND");
    qInfo() << "[Runtime] Quick graphics API:" << apiToString(QQuickWindow::graphicsApi());
    QMediaFormat mediaFormat;
    qInfo() << "[Runtime] Supported video formats count:" << mediaFormat.supportedFileFormats(QMediaFormat::Decode).size();
}
}

int main(int argc, char *argv[]) {
    if (argc > 1 && QByteArray(argv[1]) == QByteArrayLiteral("--audio-worker")) {
#ifdef Q_OS_MACOS
        // Never fall back to this bundle: SCK would also exclude scene audio.
        std::fprintf(stderr, "Mouffette audio requires the dedicated audio helper application.\n");
        return 64;
#else
        return runAudioWorker(argc, argv);
#endif
    }
    QStringList arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        arguments.append(QString::fromLocal8Bit(argv[i]));
    }

    QString configError;
    if (!AppConfig::instance().initializePreApplication(arguments, &configError)) {
        std::fprintf(stderr, "Mouffette configuration error: %s\n",
                     configError.toLocal8Bit().constData());
        return 2;
    }
    AppConfig::instance().applyPreApplicationEnvironment();

    QApplication app(argc, argv);
    app.setProperty("mouffetteAudioWorkerExecutable",
        audioWorkerExecutablePath(QCoreApplication::applicationFilePath()));
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [] {
        AudioWorkerClient::instance()->shutdown();
    });
#ifdef Q_OS_MACOS
    // Prefer our bundled Qt backend, including the precise first-seek fix.
    QCoreApplication::addLibraryPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/../PlugIns"));
#endif

    // Load bundled Impact font so it is available on all platforms
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/impact.ttf"));

    // If you previously hid the dock icon via MacDockHider, that feature has been removed.
    
    // Set application properties
    app.setApplicationName(QStringLiteral(MOUFFETTE_APPLICATION_NAME));
    app.setApplicationVersion(QStringLiteral(MOUFFETTE_VERSION_STRING));
    app.setOrganizationName("Mouffette");
    app.setOrganizationDomain("mouffette.app");

    // Share the same logo between windows, the taskbar/Dock and the Windows tray.
    // Embedded PNG sizes also work without an SVG/ICO image plugin at runtime.
    QIcon applicationIcon;
    for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256, 512, 1024}) {
        applicationIcon.addFile(
            QStringLiteral(":/icons/logo/mouffette-%1.png").arg(size), QSize(size, size));
    }
    app.setWindowIcon(applicationIcon);

    ApplicationInstanceManager instanceManager(
        QStringLiteral(MOUFFETTE_BUNDLE_IDENTIFIER ":" MOUFFETTE_BUILD_CHANNEL),
        AppConfig::instance().allowMultipleInstances());
    QString instanceError;
    const ApplicationInstanceManager::StartResult instanceResult =
        instanceManager.start(&instanceError);
    if (instanceResult == ApplicationInstanceManager::StartResult::ActivatedExisting) {
        return 0;
    }
    if (instanceResult == ApplicationInstanceManager::StartResult::Failed) {
        std::fprintf(stderr, "Mouffette instance error: %s\n",
                     instanceError.toLocal8Bit().constData());
        return 3;
    }
    const RuntimeProfileContext runtimeProfile = instanceManager.profile();
    RuntimeProfile::configure(runtimeProfile);
    MediaResidencyManager::instance().setSafetyReserve(
        AppConfig::instance().mediaRamReservePercent(),
        AppConfig::instance().mediaRamReserveMinMiB());

    // Keep application alive when window is closed (so user can reopen via other means later)
    app.setQuitOnLastWindowClosed(false);

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    registerCanvasQmlTypes();

    bool clearStorageOnExit = false;
    int exitCode = 0;
    {
        QQmlApplicationEngine engine;
        QmlRuntime::setEngine(&engine);
        // Keep the shared engine alive until controller-owned QQuickWindows
        // retire. The application shell must be destroyed before the controller
        // so its QML bindings never observe a destroyed controller.
        ApplicationController controller(runtimeProfile, arguments);
        QObject::connect(&controller, &ApplicationController::clearStorageOnExitRequested,
                         &app, [&clearStorageOnExit] { clearStorageOnExit = true; });
        engine.setInitialProperties({
            { QStringLiteral("controller"), QVariant::fromValue(&controller) }
        });
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                         &app, []() { QCoreApplication::exit(5); },
                         Qt::QueuedConnection);
        engine.loadFromModule(QStringLiteral("Mouffette.App"),
                              QStringLiteral("Main"));
        if (engine.rootObjects().isEmpty()) {
            return 5;
        }

        QObject::connect(&instanceManager,
                         &ApplicationInstanceManager::activationRequested,
                         &controller,
                         &ApplicationController::raiseRequested);
        SystemLifecycleMonitor systemLifecycleMonitor;
        QObject::connect(&app, &QGuiApplication::applicationStateChanged,
                         &controller, &ApplicationController::handleApplicationStateChanged);
        QObject::connect(&systemLifecycleMonitor,
                         &SystemLifecycleMonitor::systemSuspendedChanged,
                         &controller,
                         &ApplicationController::handleNativeSystemSuspendedChanged);
        systemLifecycleMonitor.startNativeMonitoring();
        QObject::connect(&app, &QCoreApplication::aboutToQuit,
                         &controller, &ApplicationController::handleApplicationAboutToQuit,
                         Qt::DirectConnection);
        QObject::connect(&controller, &ApplicationController::readyChanged, &app, [&controller] {
            if (controller.ready()) logRuntimeDiagnostics();
        });
        controller.start();
        exitCode = app.exec();
        qDeleteAll(engine.rootObjects());
    } // Runtime services and QML finish their last writes before removal.
    // Temporary profiles are removed by instanceManager as well. No worker
    // may still write into them after runtime/QML consumers have retired.
    QThreadPool::globalInstance()->waitForDone();
    if (clearStorageOnExit) {
        instanceManager.releaseProfileLockForRemoval();
        const auto cleared = RuntimeStorage::clearProfileStorage(runtimeProfile);
        if (!cleared.succeeded()) {
            std::fprintf(stderr, "Mouffette storage removal failed: %s\n",
                         cleared.reason.toLocal8Bit().constData());
            return 6;
        }
    }
    return exitCode;
}
