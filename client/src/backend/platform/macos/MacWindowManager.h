#ifndef MACWINDOWMANAGER_H
#define MACWINDOWMANAGER_H

#include <QtCore/QtGlobal>
#include <QList>
namespace LocalScreenTopology { struct Screen; }

class QWindow;
class QScreen;
class QString;

class MacWindowManager {
public:
    static void configureControlWindow(QWindow* window);
    static void configureGlobalOverlay(QWindow* window, bool clickThrough = true);
    static void setWindowAsGlobalOverlay(QWindow* window, bool clickThrough = true);
    static QString screenIdentity(QScreen* screen);
    static QList<LocalScreenTopology::Screen> screens(bool includeIdentity, bool* success);
    static void orderOutWindow(QWindow* window);
};

#endif // MACWINDOWMANAGER_H
