#ifndef MACWINDOWMANAGER_H
#define MACWINDOWMANAGER_H

#include <QtWidgets/QWidget>

class MacWindowManager {
public:
    static void setWindowAlwaysOnTop(QWidget* widget);
};

#endif // MACWINDOWMANAGER_H