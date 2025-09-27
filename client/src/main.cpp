#include <QApplication>
#include <QSystemTrayIcon>
#include "MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    // If you previously hid the dock icon via MacDockHider, that feature has been removed.
    
    // Set application properties
    app.setApplicationName("Mouffette");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Mouffette");
    app.setOrganizationDomain("mouffette.app");
    
    // Allow normal quit when last window is closed now that tray popups are removed
    app.setQuitOnLastWindowClosed(true);

    MainWindow window;
    window.show(); // Explicitly show main window since tray UX removed
    return app.exec();
}
