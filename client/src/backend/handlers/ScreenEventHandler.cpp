#include "backend/handlers/ScreenEventHandler.h"
#include "MainWindow.h"
#include "frontend/managers/ui/RemoteClientState.h"
#include "backend/network/WebSocketClient.h"
#include "backend/managers/system/SystemMonitor.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/rendering/canvas/LegacyCanvasHost.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/ui/pages/CanvasViewPage.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "backend/network/UploadManager.h"
#include "backend/files/FileManager.h"
#include "backend/domain/session/SessionManager.h"
#include "frontend/managers/ui/RemoteClientInfoManager.h"
#include "backend/managers/app/MigrationTelemetryManager.h"
#include <QDebug>
#include <QStackedWidget>
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

ScreenEventHandler::ScreenEventHandler(MainWindow* mainWindow, QObject* parent)
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

    // Connect to screen-related signals
    connect(client, &WebSocketClient::screensInfoReceived, this, &ScreenEventHandler::onScreensInfoReceived);
    connect(client, &WebSocketClient::dataRequestReceived, this, &ScreenEventHandler::onDataRequestReceived);

    qDebug() << "ScreenEventHandler: Connections established";
}

void ScreenEventHandler::syncRegistration()
{
    if (!m_mainWindow || !m_webSocketClient) return;

    QString machineName = m_mainWindow->getMachineName();
    QString platform = m_mainWindow->getPlatformName();
    QList<ScreenInfo> screens;
    int volumePercent = -1;

    // Only include screens/volume when actively watched; otherwise identity-only
    if (m_mainWindow->isWatched()) {
        screens = m_mainWindow->getLocalScreenInfo();
        volumePercent = m_mainWindow->getSystemVolumePercent();
    }

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

void ScreenEventHandler::onScreensInfoReceived(const ClientInfo& clientInfo)
{
    if (!m_mainWindow) return;

    QString persistentId = clientInfo.clientId();
    if (persistentId.isEmpty()) {
        qWarning() << "ScreenEventHandler::onScreensInfoReceived: client has no persistentClientId";
        return;
    }

    MainWindow::CanvasSession* session = m_mainWindow->findCanvasSession(persistentId);
    if (!session && !clientInfo.getId().isEmpty()) {
        session = m_mainWindow->findCanvasSessionByServerClientId(clientInfo.getId());
    }

    if (!session) {
        session = &m_mainWindow->ensureCanvasSession(clientInfo);
    } else {
        if (m_mainWindow->getSessionManager()) {
            m_mainWindow->getSessionManager()->updateSessionServerId(persistentId, clientInfo.getId());
        } else {
            session->serverAssignedId = clientInfo.getId();
        }
        session->lastClientInfo = clientInfo;
        session->lastClientInfo.setClientId(persistentId);
        session->lastClientInfo.setFromMemory(true);
        session->lastClientInfo.setOnline(true);
    }

    if (!session->canvas) {
        session = &m_mainWindow->ensureCanvasSession(session->lastClientInfo);
        if (!session || !session->canvas) {
            qWarning() << "ScreenEventHandler: Cannot create canvas session for" << persistentId;
            return;
        }
    }

    const QList<ScreenInfo> screens = clientInfo.getScreens();
    const bool hasScreens = !screens.isEmpty();
    m_mainWindow->recordCanvasLoadReady(session->persistentClientId, screens.size());

    if (session->canvas) {
        if (!session->serverAssignedId.isEmpty()) {
            session->canvas->setRemoteSceneTarget(session->serverAssignedId, session->lastClientInfo.getMachineName());
        }
        session->canvas->setScreens(screens);
    }

    const bool isActiveSession = (session->persistentClientId == m_mainWindow->getActiveSessionIdentity());
    if (isActiveSession) {
        m_mainWindow->setActiveCanvas(session->canvas);
        m_mainWindow->setSelectedClient(session->lastClientInfo);
    }

    session->lastClientInfo.setScreens(screens);

    if (isActiveSession && session->canvas) {
        if (!m_mainWindow->isCanvasRevealedForCurrentClient() && hasScreens) {
            if (m_mainWindow->getNavigationManager()) {
                m_mainWindow->getNavigationManager()->revealCanvas();
            } else if (m_mainWindow->getCanvasViewPage()) {
                QStackedWidget* canvasStack = m_mainWindow->getCanvasViewPage()->getCanvasStack();
                if (canvasStack) {
                    canvasStack->setCurrentIndex(1);
                }
            }

            session->canvas->requestDeferredInitialRecenter(53);
            if (!m_mainWindow->shouldPreserveViewportOnReconnect()) {
                session->canvas->recenterWithMargin(53);
            }
            session->canvas->setFocus(Qt::OtherFocusReason);

            m_mainWindow->setPreserveViewportOnReconnect(false);
            m_mainWindow->setCanvasRevealedForCurrentClient(true);
            m_mainWindow->setCanvasContentEverLoaded(true);
        }

        m_mainWindow->stopInlineSpinner();

        // Apply complete remote client state (atomic, no flicker)
        RemoteClientState state = RemoteClientState::connected(
            session->lastClientInfo,
            session->lastClientInfo.getVolumePercent()
        );
        // Only show volume if we have screens
        state.volumeVisible = hasScreens && (state.volumePercent >= 0);
        
        m_mainWindow->setRemoteClientState(state);
    }
}

void ScreenEventHandler::onDataRequestReceived()
{
    if (!m_mainWindow || !m_webSocketClient || !m_webSocketClient->isConnected()) {
        return;
    }

    // Target-side: server asked us to send fresh state now (screens + volume)
    QList<ScreenInfo> screens = m_mainWindow->getLocalScreenInfo();
    int volumePercent = m_mainWindow->getSystemVolumePercent();

    // Build uiZones immediately for snapshot
#if defined(Q_OS_WIN)
    {
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
            
            if (workH < screenH) {
                const int h = screenH - workH; 
                if (h > 0) {
                    if (m.rcWork.top > m.rc.top) {
                        screen.uiZones.append(ScreenInfo::UIZone{"taskbar", 0, 0, screenW, h});
                    } else {
                        screen.uiZones.append(ScreenInfo::UIZone{"taskbar", 0, screenH - h, screenW, h});
                    }
                }
            } else if (workW < screenW) {
                const int w = screenW - workW; 
                if (w > 0) {
                    if (m.rcWork.left > m.rc.left) {
                        screen.uiZones.append(ScreenInfo::UIZone{"taskbar", 0, 0, w, screenH});
                    } else {
                        screen.uiZones.append(ScreenInfo::UIZone{"taskbar", screenW - w, 0, w, screenH});
                    }
                }
            }
        }
    }
#elif defined(Q_OS_MACOS)
    {
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
            
            if (avail.y() > geom.y()) { 
                int h = static_cast<int>(std::lround(static_cast<qreal>(avail.y() - geom.y()) * dpr)); 
                if (h > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{"menu_bar", 0, 0, geomWidthPx, h}); 
                }
            }
            
            if (avail.bottom() < geom.bottom()) { 
                int h = static_cast<int>(std::lround(static_cast<qreal>(geom.bottom() - avail.bottom()) * dpr)); 
                if (h > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{"dock", 0, geomHeightPx - h, geomWidthPx, h}); 
                }
            } else if (avail.x() > geom.x()) { 
                int w = static_cast<int>(std::lround(static_cast<qreal>(avail.x() - geom.x()) * dpr)); 
                if (w > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{"dock", 0, 0, w, geomHeightPx}); 
                }
            } else if (avail.right() < geom.right()) { 
                int w = static_cast<int>(std::lround(static_cast<qreal>(geom.right() - avail.right()) * dpr)); 
                if (w > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{"dock", geomWidthPx - w, 0, w, geomHeightPx}); 
                }
            }
        }
    }
#endif

    m_webSocketClient->sendStateSnapshot(screens, volumePercent);
}
