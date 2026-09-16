#include <QApplication>
#include <QFontDatabase>
#include <QSystemTrayIcon>
#include <QDebug>
#include <QCoreApplication>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QMediaFormat>
#include <cstdio>
#include "AppBuildConfig.h"
#include "backend/config/AppConfig.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/managers/system/SystemLifecycleMonitor.h"
#include "backend/runtime/ApplicationInstanceManager.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/RuntimeStorageBootstrap.h"
#include "frontend/qml/ApplicationController.h"
#include "frontend/qml/QmlRuntime.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"

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

    QQmlApplicationEngine engine;
    QmlRuntime::setEngine(&engine);
    // The controller is deliberately constructed after the engine. It is then
    // destroyed first, allowing all controller-owned QQuickWindows to retire
    // before their shared QQmlEngine.
    ApplicationController controller(runtimeProfile, arguments);
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
    return app.exec();
}
