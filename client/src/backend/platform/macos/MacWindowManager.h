#ifndef MACWINDOWMANAGER_H
#define MACWINDOWMANAGER_H

#include <QtCore/QtGlobal>

class QWindow;

class MacWindowManager {
public:
    static void setWindowAlwaysOnTop(QWindow* window);
    static void setWindowAsGlobalOverlay(QWindow* window, bool clickThrough = true);
    static void orderOutWindow(QWindow* window);
    static void activateApplicationWindow(QWindow* window);
};

#endif // MACWINDOWMANAGER_H
