#include "backend/platform/LocalScreenTopology.h"

#include "backend/managers/system/ScreenCoordinateMapping.h"
#include <QGuiApplication>
#include <algorithm>

#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacWindowManager.h"
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <QtGui/qscreen_platform.h>
#endif

namespace LocalScreenTopology {
namespace {
QString serialIdentity(QScreen* screen)
{
    if (!screen || screen->serialNumber().isEmpty()) return {};
    return QStringLiteral("serial:%1/%2/%3")
        .arg(screen->manufacturer(), screen->model(), screen->serialNumber());
}

#ifdef Q_OS_WIN
struct Enumeration {
    QList<Screen> result;
    QList<QScreen*> qtScreens;
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context)
{
    auto& enumeration = *reinterpret_cast<Enumeration*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    Screen entry;
    const RECT& bounds = info.rcMonitor;
    entry.advertisedGeometry = QRect(bounds.left, bounds.top,
                                    bounds.right - bounds.left, bounds.bottom - bounds.top);
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    entry.nativeWindowsCoordinates = true;
    for (QScreen* screen : enumeration.qtScreens) {
        auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (!native || native->handle() != monitor) continue;
        entry.screen = screen;
        entry.geometry = screen->geometry();
        entry.identity = serialIdentity(screen);
        break;
    }
    // DeviceID with this flag is the monitor interface path, not \\.\DISPLAYn
    // (which can be reused for a different monitor after hot-plug).
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)
        && device.DeviceID[0]) {
        entry.identity = QStringLiteral("win:") + QString::fromWCharArray(device.DeviceID);
    }
    enumeration.result.append(entry);
    return TRUE;
}
#endif
}

QList<Screen> screens()
{
    const QList<QScreen*> qtScreens = QGuiApplication::screens();
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QLatin1String("windows")) {
        Enumeration enumeration{{}, qtScreens};
        EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&enumeration));
        if (!enumeration.result.isEmpty()) return enumeration.result;
    }
#endif
    QList<Screen> result;
    for (QScreen* screen : qtScreens) {
        Screen entry;
        entry.screen = screen;
        entry.identity = serialIdentity(screen);
        entry.geometry = screen->geometry();
        entry.advertisedGeometry = entry.geometry;
        entry.primary = screen == QGuiApplication::primaryScreen();
#ifdef Q_OS_MACOS
        const QString nativeIdentity = MacWindowManager::screenIdentity(screen);
        if (!nativeIdentity.isEmpty()) entry.identity = nativeIdentity;
        entry.advertisedGeometry = ScreenCoordinateMapping::scaledScreenGeometry(
            entry.geometry, std::max<qreal>(1.0, screen->devicePixelRatio()));
#endif
        result.append(entry);
    }
    return result;
}
}
