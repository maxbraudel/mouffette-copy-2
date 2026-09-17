#ifndef MACWINDOWMANAGER_H
#define MACWINDOWMANAGER_H

#include <QtCore/QtGlobal>

class QWindow;
class QScreen;
class QString;

class MacWindowManager {
public:
    static void setWindowAlwaysOnTop(QWindow* window);
    static void configureGlobalOverlay(QWindow* window, bool clickThrough = true);
    static void setWindowAsGlobalOverlay(QWindow* window, bool clickThrough = true);
    static QString screenIdentity(QScreen* screen);
    static void orderOutWindow(QWindow* window);
    static void activateApplicationWindow(QWindow* window);
};

#endif // MACWINDOWMANAGER_H
