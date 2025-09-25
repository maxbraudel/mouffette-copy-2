#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include "MainWindow.h"
#include "MacDockHider.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    // Hide from dock on macOS
    MacDockHider::hideDockIcon();
    
    // Set application properties
    app.setApplicationName("Mouffette");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Mouffette");
    app.setOrganizationDomain("mouffette.app");
    
    // Don't quit when last window is closed (for tray applications)
    app.setQuitOnLastWindowClosed(false);
    
    // Check if system tray is available
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "System Tray",
                             "System tray is not available on this system.");
        return 1;
    }
    
    MainWindow window;
    // Window will be hidden by default and shown in tray
    
    return app.exec();
}
