#include <QApplication>
#include <QSystemTrayIcon>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include "MainWindow.h"

// ── Dev flags ────────────────────────────────────────────────────────────────
// Set these to true to enable features without passing environment variables.
// Equivalent to: MOUFFETTE_USE_QUICK_CANVAS_RENDERER=1 MOUFFETTE_CURSOR_DEBUG=1
static constexpr bool DEV_USE_QUICK_CANVAS_RENDERER = true;
static constexpr bool DEV_CURSOR_DEBUG               = true;
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
}

int main(int argc, char *argv[]) {
    if (DEV_USE_QUICK_CANVAS_RENDERER) qputenv("MOUFFETTE_USE_QUICK_CANVAS_RENDERER", "1");
    if (DEV_CURSOR_DEBUG)               qputenv("MOUFFETTE_CURSOR_DEBUG", "1");

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

    // Also clear on clean shutdown
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [](){ cleanUploadsFolder(); });

    // Keep application alive when window is closed (so user can reopen via other means later)
    app.setQuitOnLastWindowClosed(false);

    MainWindow window;
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &window, &MainWindow::handleApplicationStateChanged);
    window.show(); // Explicitly show main window since tray UX removed
    return app.exec();
}
