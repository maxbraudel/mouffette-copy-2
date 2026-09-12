#include <QApplication>
#include <QFontDatabase>
#include <QSystemTrayIcon>
#include <QDir>
#include <QDebug>
#include <QCoreApplication>
#include <QQuickWindow>
#include <QMediaFormat>
#include <QSettings>
#include <cstdio>
#include "AppBuildConfig.h"
#include "backend/config/AppConfig.h"
#include "backend/managers/system/SystemLifecycleMonitor.h"
#include "backend/runtime/ApplicationInstanceManager.h"
#include "backend/runtime/RuntimeProfile.h"
#include "MainWindow.h"

// ── Dev flags ────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Protocol-v3 cutover only. Live v3 caches are owned by RemoteSession teardown;
// this one-time cleanup removes only pre-v3 cache state.
void cleanLegacyUploadsOnce() {
    const std::unique_ptr<QSettings> settings = RuntimeProfile::createSettings();
    if (settings->value(QStringLiteral("protocolV3LegacyUploadsCleaned"), false).toBool()) {
        return;
    }
    const QString uploadsPath =
        QDir(RuntimeProfile::cacheLocation()).absoluteFilePath(QStringLiteral("Mouffette/Uploads"));
    QDir dir(uploadsPath);
    if (dir.exists() && !dir.removeRecursively()) {
        qWarning() << "Protocol-v3 migration could not remove the legacy upload cache";
        return;
    }
    settings->setValue(QStringLiteral("protocolV3LegacyUploadsCleaned"), true);
    settings->sync();
    qInfo() << "Protocol-v3 legacy upload cache cleanup complete";
}

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
    if (!AppConfig::instance().initialize(arguments, &configError)) {
        std::fprintf(stderr, "Mouffette configuration error: %s\n",
                     configError.toLocal8Bit().constData());
        return 2;
    }
    AppConfig::instance().applyPreApplicationEnvironment();

    QApplication app(argc, argv);

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
    if (runtimeProfile.isSecondary()
        && !AppConfig::instance().initializeWithSettings(
            arguments, RuntimeProfile::readSettings(), &configError)) {
        std::fprintf(stderr, "Mouffette profile configuration error: %s\n",
                     configError.toLocal8Bit().constData());
        return 2;
    }
    
    // Disable focus rectangle on all widgets (especially visible on Windows)
    app.setStyleSheet("* { outline: none; }");
    
    cleanLegacyUploadsOnce();
    logRuntimeDiagnostics();

    // Keep application alive when window is closed (so user can reopen via other means later)
    app.setQuitOnLastWindowClosed(false);

    MainWindow window(runtimeProfile);
    QObject::connect(&instanceManager,
                     &ApplicationInstanceManager::activationRequested,
                     &window,
                     &MainWindow::showAndActivate);
    SystemLifecycleMonitor systemLifecycleMonitor;
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &window, &MainWindow::handleApplicationStateChanged);
    QObject::connect(&systemLifecycleMonitor,
                     &SystemLifecycleMonitor::systemSuspendedChanged,
                     &window,
                     &MainWindow::handleNativeSystemSuspendedChanged);
    systemLifecycleMonitor.startNativeMonitoring();
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &window, &MainWindow::handleApplicationAboutToQuit,
                     Qt::DirectConnection);
    window.show(); // Explicitly show main window since tray UX removed
    return app.exec();
}
