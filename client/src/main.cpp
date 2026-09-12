#include <QApplication>
#include <QFontDatabase>
#include <QSystemTrayIcon>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QCoreApplication>
#include <QQuickWindow>
#include <QMediaFormat>
#include <QSettings>
#include <cstdio>
#include "backend/config/AppConfig.h"
#include "backend/managers/system/SystemLifecycleMonitor.h"
#include "MainWindow.h"

// ── Dev flags ────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Protocol-v2 migration only. Live v2 caches are owned by RemoteSession
// teardown and must never be swept merely because the UI process exits.
void cleanLegacyUploadsOnce() {
    QSettings settings(QStringLiteral("Mouffette"), QStringLiteral("Client"));
    if (settings.value(QStringLiteral("protocolV2LegacyUploadsCleaned"), false).toBool()) {
        return;
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.cache";
    const QString uploadsPath =
        QDir(base).absoluteFilePath(QStringLiteral("Mouffette/Uploads"));
    QDir dir(uploadsPath);
    if (dir.exists() && !dir.removeRecursively()) {
        qWarning() << "Protocol-v2 migration could not remove the legacy upload cache";
        return;
    }
    settings.setValue(QStringLiteral("protocolV2LegacyUploadsCleaned"), true);
    settings.sync();
    qInfo() << "Protocol-v2 legacy upload cache migration complete";
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
    app.setApplicationName("Mouffette");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Mouffette");
    app.setOrganizationDomain("mouffette.app");
    
    // Disable focus rectangle on all widgets (especially visible on Windows)
    app.setStyleSheet("* { outline: none; }");
    
    cleanLegacyUploadsOnce();
    logRuntimeDiagnostics();

    // Keep application alive when window is closed (so user can reopen via other means later)
    app.setQuitOnLastWindowClosed(false);

    MainWindow window;
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
