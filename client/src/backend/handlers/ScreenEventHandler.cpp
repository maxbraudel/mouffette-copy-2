#include "backend/handlers/ScreenEventHandler.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/network/WebSocketClient.h"
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <array>

namespace {

constexpr size_t kMaxEnumeratedMonitors = 16;

struct WinMonRect {
    RECT rc;
    RECT rcWork;
    bool primary;
};

struct MonitorEnumContext {
    std::array<WinMonRect, kMaxEnumeratedMonitors> monitors{};
    size_t count = 0;
    bool overflow = false;
};

static BOOL CALLBACK ScreenEventEnumMonProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* ctx = reinterpret_cast<MonitorEnumContext*>(lParam);
    if (!ctx) {
        return FALSE;
    }

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        return TRUE;
    }

    if (ctx->count >= ctx->monitors.size()) {
        ctx->overflow = true;
        return TRUE;
    }

    WinMonRect entry{};
    entry.rc = mi.rcMonitor;
    entry.rcWork = mi.rcWork;
    entry.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    ctx->monitors[ctx->count++] = entry;
    return TRUE;
}
} // namespace
#endif

ScreenEventHandler::ScreenEventHandler(ApplicationRuntime* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
    , m_webSocketClient(nullptr)
{
}

void ScreenEventHandler::setupConnections(WebSocketClient* client)
{
    if (!client) {
        qWarning() << "ScreenEventHandler::setupConnections - No WebSocket client provided";
        return;
    }

    m_webSocketClient = client;

    qDebug() << "ScreenEventHandler: Connections established";
}

void ScreenEventHandler::syncRegistration()
{
    if (!m_mainWindow || !m_webSocketClient) return;

    QString machineName = m_mainWindow->getMachineName();
    QString platform = m_mainWindow->getPlatformName();
    // Protocol v4 publishes one complete authoritative device snapshot. Screen
    // and volume discovery is independent from projects and remote sessions;
    // there is deliberately no request/watch subscription protocol anymore.
    QList<ScreenInfo> screens = m_mainWindow->getLocalScreenInfo();
    const int volumePercent = m_mainWindow->getSystemVolumePercent();

    // Build per-screen uiZones (taskbar/menu/dock)
    if (!screens.isEmpty()) {
#if defined(Q_OS_WIN)
        // Build a list of physical monitors (rcMonitor/rcWork) to align with ScreenInfo (physical px)
        MonitorEnumContext ctx;
        EnumDisplayMonitors(nullptr, nullptr, ScreenEventEnumMonProc, reinterpret_cast<LPARAM>(&ctx));
        
        auto findMatchingMon = [&](const ScreenInfo& s) -> const WinMonRect* {
            for (size_t idx = 0; idx < ctx.count; ++idx) {
                const auto &m = ctx.monitors[idx];
                const int mw = m.rc.right - m.rc.left;
                const int mh = m.rc.bottom - m.rc.top;
                if (m.rc.left == s.x && m.rc.top == s.y && mw == s.width && mh == s.height) {
                    return &m;
                }
            }
            return nullptr;
        };
        
        for (auto &screen : screens) {
            const WinMonRect* mp = findMatchingMon(screen);
            if (!mp) continue;
            const auto &m = *mp;
            const int screenW = m.rc.right - m.rc.left;
            const int screenH = m.rc.bottom - m.rc.top;
            const int workW = m.rcWork.right - m.rcWork.left;
            const int workH = m.rcWork.bottom - m.rcWork.top;
            
            // Compute taskbar thickness and side by comparing rcMonitor and rcWork
            if (workH < screenH) {
                const int h = screenH - workH; 
                if (h > 0) {
                    if (m.rcWork.top > m.rc.top) {
                        screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("taskbar"), 0, 0, screenW, h});
                    } else {
                        screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("taskbar"), 0, screenH - h, screenW, h});
                    }
                }
            } else if (workW < screenW) {
                const int w = screenW - workW; 
                if (w > 0) {
                    if (m.rcWork.left > m.rc.left) {
                        screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("taskbar"), 0, 0, w, screenH});
                    } else {
                        screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("taskbar"), screenW - w, 0, w, screenH});
                    }
                }
            }
        }
#elif defined(Q_OS_MACOS)
        QList<QScreen*> qScreens = QGuiApplication::screens();
        for (auto &screen : screens) {
            if (screen.id < 0 || screen.id >= qScreens.size()) continue;
            QScreen* qs = qScreens[screen.id]; 
            if (!qs) continue;
            
            const qreal dpr = std::max<qreal>(1.0, qs->devicePixelRatio());
            QRect geom = qs->geometry();
            QRect avail = qs->availableGeometry();

            const int geomWidthPx = static_cast<int>(std::lround(static_cast<qreal>(geom.width()) * dpr));
            const int geomHeightPx = static_cast<int>(std::lround(static_cast<qreal>(geom.height()) * dpr));
            
            // Menu bar
            if (avail.y() > geom.y()) {
                int h = static_cast<int>(std::lround(static_cast<qreal>(avail.y() - geom.y()) * dpr)); 
                if (h > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("menu_bar"), 0, 0, geomWidthPx, h});
                }
            }
            
            // Dock: one differing edge
            if (avail.bottom() < geom.bottom()) { // bottom dock
                int h = static_cast<int>(std::lround(static_cast<qreal>(geom.bottom() - avail.bottom()) * dpr)); 
                if (h > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), 0, geomHeightPx - h, geomWidthPx, h});
                }
            } else if (avail.x() > geom.x()) { // left dock
                int w = static_cast<int>(std::lround(static_cast<qreal>(avail.x() - geom.x()) * dpr)); 
                if (w > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), 0, 0, w, geomHeightPx});
                }
            } else if (avail.right() < geom.right()) { // right dock
                int w = static_cast<int>(std::lround(static_cast<qreal>(geom.right() - avail.right()) * dpr)); 
                if (w > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), geomWidthPx - w, 0, w, geomHeightPx});
                }
            }
        }
#endif
    }
    
    qDebug() << "ScreenEventHandler: Sync registration:" << machineName << "on" << platform 
             << "with" << screens.size() << "screens";
    
    m_webSocketClient->registerClient(machineName, platform, screens, volumePercent);
}
