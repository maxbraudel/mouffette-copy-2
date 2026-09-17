#pragma once

#include <QList>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QString>

namespace LocalScreenTopology {
struct Screen {
    QPointer<QScreen> screen;
    QString identity; // Hardware identity, never an enumeration index/position.
    QRect geometry; // Qt logical pixels for window placement.
    QRect advertisedGeometry; // Existing protocol coordinates.
    bool primary = false;
    bool nativeWindowsCoordinates = false;
};

// Shared by discovery, cursor mapping and the renderer. Windows' monitor
// enumeration order need not be QGuiApplication::screens() order.
QList<Screen> screens(bool includeIdentity = true);
}
