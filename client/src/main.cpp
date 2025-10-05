#include <QApplication>
#include <QSystemTrayIcon>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include "MainWindow.h"

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
    QApplication app(argc, argv);
    
    // If you previously hid the dock icon via MacDockHider, that feature has been removed.
    
    // Set application properties
    app.setApplicationName("Mouffette");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Mouffette");
    app.setOrganizationDomain("mouffette.app");
    
    // Ensure uploads cache is empty on startup
    cleanUploadsFolder();

    // Also clear on clean shutdown
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [](){ cleanUploadsFolder(); });

    // Keep application alive when window is closed (so user can reopen via other means later)
    app.setQuitOnLastWindowClosed(false);

    MainWindow window;
    window.show(); // Explicitly show main window since tray UX removed
    return app.exec();
}
