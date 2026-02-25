#include <QApplication>
#include <QSystemTrayIcon>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QCoreApplication>
#include <QQuickWindow>
#include <QMediaFormat>
#include "MainWindow.h"

// ── Dev flags ────────────────────────────────────────────────────────────────
// Set these to true to enable features without passing environment variables.
// Equivalent to: MOUFFETTE_USE_QUICK_CANVAS_RENDERER=1 MOUFFETTE_CURSOR_DEBUG=1
static constexpr bool DEV_USE_QUICK_CANVAS_RENDERER = true;
static constexpr bool DEV_CURSOR_DEBUG               = false;
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Remove the entire upload cache folder used by UploadManager:
//   base = QStandardPaths::CacheLocation (fallback ~/.cache)
//   path = base + "/Mouffette/Uploads"
void cleanUploadsFolder() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.cache";
    const QString uploadsPath = base + "/Mouffette/Uploads";
    QDir dir(uploadsPath);
    if (dir.exists()) {
        if (!dir.removeRecursively()) {
            qWarning() << "Failed to remove uploads cache folder:" << uploadsPath;
        } else {
            qDebug() << "Cleared uploads cache folder:" << uploadsPath;
        }
    }
}

void logRuntimeDiagnostics() {
    if (qEnvironmentVariableIntValue("MOUFFETTE_RUNTIME_DIAGNOSTICS") != 1) {
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
    if (DEV_USE_QUICK_CANVAS_RENDERER) qputenv("MOUFFETTE_USE_QUICK_CANVAS_RENDERER", "1");
    if (DEV_CURSOR_DEBUG)               qputenv("MOUFFETTE_CURSOR_DEBUG", "1");
    if (!qEnvironmentVariableIsSet("QT_MEDIA_BACKEND")) {
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    }

    QApplication app(argc, argv);
    
    // If you previously hid the dock icon via MacDockHider, that feature has been removed.
    
    // Set application properties
    app.setApplicationName("Mouffette");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Mouffette");
    app.setOrganizationDomain("mouffette.app");
    
    // Disable focus rectangle on all widgets (especially visible on Windows)
    app.setStyleSheet("* { outline: none; }");
    
    // Ensure uploads cache is empty on startup
    cleanUploadsFolder();
    logRuntimeDiagnostics();

    // Also clear on clean shutdown
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [](){ cleanUploadsFolder(); });

    // Keep application alive when window is closed (so user can reopen via other means later)
    app.setQuitOnLastWindowClosed(false);

    MainWindow window;
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &window, &MainWindow::handleApplicationStateChanged);
    window.show(); // Explicitly show main window since tray UX removed
    return app.exec();
}
