#include "handlers/ScreenEventHandler.h"
#include "MainWindow.h"
#include "network/WebSocketClient.h"
#include "managers/system/SystemMonitor.h"
#include "rendering/canvas/ScreenCanvas.h"
#include "ui/pages/CanvasViewPage.h"
#include "rendering/navigation/ScreenNavigationManager.h"
#include "network/UploadManager.h"
#include "files/FileManager.h"
#include "domain/session/SessionManager.h"
#include "managers/ui/RemoteClientInfoManager.h"
#include <QDebug>
#include <QStackedWidget>
#include <QGuiApplication>
#include <QScreen>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
struct WinMonRect { std::wstring name; RECT rc; RECT rcWork; bool primary; };
static BOOL CALLBACK ScreenEventEnumMonProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<WinMonRect>*>(lParam);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        WinMonRect r; r.name = mi.szDevice; r.rc = mi.rcMonitor; r.rcWork = mi.rcWork;
        r.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        out->push_back(r);
    }
    return TRUE;
}
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
        std::vector<WinMonRect> mons; 
        mons.reserve(8);
        EnumDisplayMonitors(nullptr, nullptr, ScreenEventEnumMonProc, reinterpret_cast<LPARAM>(&mons));
        
        auto findMatchingMon = [&](const ScreenInfo& s) -> const WinMonRect* {
            for (const auto &m : mons) {
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
            
            QRect geom = qs->geometry();
            QRect avail = qs->availableGeometry();
            
            // Menu bar
            if (avail.y() > geom.y()) {
                int h = avail.y() - geom.y(); 
                if (h > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("menu_bar"), 0, 0, geom.width(), h});
                }
            }
            
            // Dock: one differing edge
            if (avail.bottom() < geom.bottom()) { // bottom dock
                int h = geom.bottom() - avail.bottom(); 
                if (h > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), 0, geom.height() - h, geom.width(), h});
                }
            } else if (avail.x() > geom.x()) { // left dock
                int w = avail.x() - geom.x(); 
                if (w > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), 0, 0, w, geom.height()});
                }
            } else if (avail.right() < geom.right()) { // right dock
                int w = geom.right() - avail.right(); 
                if (w > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), geom.width() - w, 0, w, geom.height()});
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
        session->serverAssignedId = clientInfo.getId();
        session->lastClientInfo = clientInfo;
        session->lastClientInfo.setClientId(persistentId);
        session->lastClientInfo.setFromMemory(true);
        session->lastClientInfo.setOnline(true);
    }

    if (!session->canvas) {
        QStackedWidget* canvasHostStack = m_mainWindow->getCanvasViewPage() ? 
            m_mainWindow->getCanvasViewPage()->getCanvasHostStack() : nullptr;
        if (!canvasHostStack) {
            qWarning() << "ScreenEventHandler: Cannot create canvas - CanvasViewPage not initialized";
            return;
        }
        
        session->canvas = new ScreenCanvas(canvasHostStack);
        session->canvas->setWebSocketClient(m_webSocketClient);
        session->canvas->setUploadManager(m_mainWindow->getUploadManager());
        session->canvas->setFileManager(m_mainWindow->getFileManager());
        session->connectionsInitialized = false;
        
        if (canvasHostStack->indexOf(session->canvas) == -1) {
            canvasHostStack->addWidget(session->canvas);
        }
        
        m_mainWindow->configureCanvasSession(*session);
    }

    const QList<ScreenInfo> screens = clientInfo.getScreens();
    const bool hasScreens = !screens.isEmpty();

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

        // Add volume indicator if we have screens
        if (hasScreens && m_mainWindow->getRemoteClientInfoManager() && 
            m_mainWindow->getRemoteClientInfoManager()->getVolumeIndicator()) {
            m_mainWindow->addVolumeIndicatorToLayout();
            m_mainWindow->updateVolumeIndicator();
        }

        m_mainWindow->addRemoteStatusToLayout();
        m_mainWindow->setRemoteConnectionStatus(session->lastClientInfo.isOnline() ? "CONNECTED" : "DISCONNECTED");
        m_mainWindow->updateClientNameDisplay(session->lastClientInfo);
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
        std::vector<WinMonRect> mons; 
        mons.reserve(8);
        EnumDisplayMonitors(nullptr, nullptr, ScreenEventEnumMonProc, reinterpret_cast<LPARAM>(&mons));
        
        auto findMatchingMon = [&](const ScreenInfo& s) -> const WinMonRect* {
            for (const auto &m : mons) {
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
            
            QRect geom = qs->geometry(); 
            QRect avail = qs->availableGeometry();
            
            if (avail.y() > geom.y()) { 
                int h = avail.y() - geom.y(); 
                if (h > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{"menu_bar", 0, 0, geom.width(), h}); 
                }
            }
            
            if (avail.bottom() < geom.bottom()) { 
                int h = geom.bottom() - avail.bottom(); 
                if (h > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{"dock", 0, geom.height() - h, geom.width(), h}); 
                }
            } else if (avail.x() > geom.x()) { 
                int w = avail.x() - geom.x(); 
                if (w > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{"dock", 0, 0, w, geom.height()}); 
                }
            } else if (avail.right() < geom.right()) { 
                int w = geom.right() - avail.right(); 
                if (w > 0) {
                    screen.uiZones.append(ScreenInfo::UIZone{"dock", geom.width() - w, 0, w, geom.height()}); 
                }
            }
        }
    }
#endif

    m_webSocketClient->sendStateSnapshot(screens, volumePercent);
}
