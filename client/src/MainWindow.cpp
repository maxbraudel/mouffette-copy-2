#include "MainWindow.h"
#include "WebSocketClient.h"
#include "ClientInfo.h"
#include "ScreenNavigationManager.h"
#include "UploadManager.h"
#include "WatchManager.h"
#include "FileWatcher.h"
#include "SpinnerWidget.h"
#include "ScreenCanvas.h"
#include "MediaItems.h"
#include "MediaSettingsPanel.h"
#include "OverlayPanels.h"
#include "Theme.h"
#include "AppColors.h"
#include "FileManager.h"
#include "RemoteSceneController.h"
#include "SessionManager.h"
#include "ui/widgets/RoundedContainer.h"
#include "ui/widgets/ClippedContainer.h"
#include "ui/widgets/ClientListDelegate.h"
#include "ui/pages/ClientListPage.h"
    #include "ui/pages/CanvasViewPage.h"
#include "managers/ThemeManager.h"
#include "managers/RemoteClientInfoManager.h"
#include "managers/SystemMonitor.h"
#include "managers/TopBarManager.h"
#include "managers/SystemTrayManager.h"
#include "managers/MenuBarManager.h"
#include "handlers/WebSocketMessageHandler.h"
#include "handlers/ScreenEventHandler.h"
#include "handlers/ClientListEventHandler.h"
#include "handlers/UploadEventHandler.h"
#include <QMenuBar>
#include <QHostInfo>
#include "ClientInfo.h"

// Forward declaration for system UI extraction
#include <QDebug>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QStyleOption>
#include <algorithm>
#include <cmath>
#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QSettings>
#include <QNativeGestureEvent>
#include <QCursor>
#include <QRandomGenerator>
#include <algorithm>
#include <QPaintEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QTimer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDragMoveEvent>
#include <QMimeData>
#include <QStatusBar>
#include <QGraphicsPixmapItem>
#include <QGraphicsSceneMouseEvent>
#include <QPen>
#include <QBrush>
#include <QUrl>
#include <QImage>
#include <QAbstractItemView>
#include <QPixmap>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSysInfo>
#include <QGraphicsTextItem>
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QtSvgWidgets/QGraphicsSvgItem>
#include <QtSvg/QSvgRenderer>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <vector>
#  include <string>
// WinAPI monitor enumeration helper
struct WinMonRect { std::wstring name; RECT rc; RECT rcWork; bool primary; };
static BOOL CALLBACK MouffetteEnumMonProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<WinMonRect>*>(lParam);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        WinMonRect r; r.name = mi.szDevice; r.rc = mi.rcMonitor; r.rcWork = mi.rcWork; r.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0; out->push_back(r);
    }
    return TRUE;
}
#endif
#include <QPainterPathStroker>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QUuid>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QFileDialog>
#include <climits>
#include <memory>
#ifdef Q_OS_MACOS
#include "MacVideoThumbnailer.h"
#include "MacWindowManager.h"
#endif
#include "ResponsiveLayoutManager.h"
#include <QGraphicsItem>
#include <QSet>
#include <QElapsedTimer>
#include <QDateTime>
#include <QThreadPool>
#include <QRunnable>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QObject>
#include <atomic>
#include <thread>
#include <optional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <combaseapi.h>
// Ensure GUIDs (IIDs/CLSIDs) are defined in this translation unit for MinGW linkers
#ifndef INITGUID
#define INITGUID
#endif
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif
#ifdef Q_OS_MACOS
#include <QProcess>
#endif

static RemoteSceneController* g_remoteSceneController = nullptr; // Remote scene controller global instance

const QString MainWindow::DEFAULT_SERVER_URL = "ws://192.168.0.188:8080";

// Global style configuration variables (accessible to UI widgets)
// TODO: Move to ThemeManager singleton in future refactoring
int gWindowContentMarginTop = 20;       // Top margin for all window content
int gWindowContentMarginRight = 20;     // Right margin for all window content
int gWindowContentMarginBottom = 20;    // Bottom margin for all window content
int gWindowContentMarginLeft = 20;      // Left margin for all window content
int gWindowBorderRadiusPx = 10;         // Rounded corner radius (px)
int gInnerContentGap = 20;              // Gap between sections inside the window
int gDynamicBoxMinWidth = 80;           // Minimum width for buttons/status boxes
int gDynamicBoxHeight = 24;             // Fixed height for all elements
int gDynamicBoxBorderRadius = 6;        // Border radius for rounded corners
int gDynamicBoxFontPx = 13;             // Standard font size for buttons/status boxes
int gRemoteClientContainerPadding = 6;  // Horizontal padding for elements in remote client container
int gTitleTextFontSize = 16;            // Title font size (px)
int gTitleTextHeight = 24;              // Title fixed height (px)

// Z-ordering constants used throughout the scene
namespace {
constexpr qreal Z_SCREENS = -1000.0;
constexpr qreal Z_MEDIA_BASE = 1.0;
constexpr qreal Z_REMOTE_CURSOR = 10000.0;
constexpr qreal Z_SCENE_OVERLAY = 12000.0; // above all scene content

// Temporary wrapper functions for backward compatibility during migration
// TODO: Remove these and use ThemeManager::instance() directly everywhere
inline void applyPillBtn(QPushButton* b) {
    ThemeManager::instance()->applyPillButton(b);
}

inline void applyPrimaryBtn(QPushButton* b) {
    ThemeManager::instance()->applyPrimaryButton(b);
}

inline void applyStatusBox(QLabel* l, const QString& borderColor, const QString& bgColor, const QString& textColor) {
    ThemeManager::instance()->applyStatusBox(l, borderColor, bgColor, textColor);
}

inline void applyTitleText(QLabel* l) {
    ThemeManager::instance()->applyTitleText(l);
}

inline int uploadButtonMaxWidth() {
    return ThemeManager::instance()->getUploadButtonMaxWidth();
}
}


bool MainWindow::event(QEvent* event) {
    if (event->type() == QEvent::PaletteChange) {
        // Theme changed - update stylesheets that use ColorSource
        updateStylesheetsForTheme();
    }
    return QMainWindow::event(event);
}

// [PHASE 1.2] Delegate to CanvasViewPage + handle MainWindow-specific logic
void MainWindow::setRemoteConnectionStatus(const QString& status, bool propagateLoss) {
    // Delegate UI updates to CanvasViewPage
    if (m_canvasViewPage) {
        m_canvasViewPage->setRemoteConnectionStatus(status, propagateLoss);
    }
    
    // Update MainWindow state
    const QString up = status.toUpper();
    if (up == "CONNECTED") {
        m_remoteClientConnected = true;
    } else if (up == "DISCONNECTED" || up.startsWith("CONNECTING") || up == "ERROR") {
        m_remoteClientConnected = false;
    }
    
    // Stop inline spinner if connected
    if (up == "CONNECTED") {
        if (m_inlineSpinner && m_inlineSpinner->isSpinning()) {
            m_inlineSpinner->stop();
            m_inlineSpinner->hide();
        } else if (m_inlineSpinner) {
            m_inlineSpinner->hide();
        }
    }
    
    // Show client list placeholder when connecting
    if (up == "CONNECTING" || up.startsWith("CONNECTING") || up.startsWith("RECONNECTING")) {
        if (m_clientListPage) {
            m_clientListPage->ensureClientListPlaceholder();
        }
    }

    refreshOverlayActionsState(up == "CONNECTED", propagateLoss);
}

void MainWindow::refreshOverlayActionsState(bool remoteConnected, bool propagateLoss) {
    m_remoteOverlayActionsEnabled = remoteConnected;

    if (m_screenCanvas) {
        if (!remoteConnected && propagateLoss) {
            m_screenCanvas->handleRemoteConnectionLost();
        }
        m_screenCanvas->setOverlayActionsEnabled(remoteConnected);
    }

    if (!m_uploadButton) return;

    if (m_uploadButtonInOverlay) {
        if (!remoteConnected) {
            m_uploadButton->setEnabled(false);
            m_uploadButton->setCheckable(false);
            m_uploadButton->setChecked(false);
            m_uploadButton->setStyleSheet(ScreenCanvas::overlayDisabledButtonStyle());
            AppColors::applyCanvasButtonFont(m_uploadButtonDefaultFont);
            m_uploadButton->setFont(m_uploadButtonDefaultFont);
            m_uploadButton->setFixedHeight(40);
            m_uploadButton->setMaximumWidth(uploadButtonMaxWidth());
        } else if (m_uploadManager) {
            QTimer::singleShot(0, this, [this]() {
                if (m_uploadManager) emit m_uploadManager->uiStateChanged();
            });
        }
    } else {
        m_uploadButton->setEnabled(remoteConnected);
    }
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_fileManager(new FileManager()),  // Phase 4.3: Inject FileManager
      m_sessionManager(new SessionManager(this)),  // Phase 4.1
      m_centralWidget(nullptr),
      m_mainLayout(nullptr),
      m_stackedWidget(nullptr),
      m_clientListPage(nullptr), // Phase 1.1: ClientListPage
      m_canvasViewPage(nullptr), // Phase 1.2: CanvasViewPage
      m_connectionLayout(nullptr),
      m_settingsButton(nullptr),
      m_connectToggleButton(nullptr),
      m_connectionStatusLabel(nullptr),
      m_backButton(nullptr),
      m_remoteClientInfoManager(new RemoteClientInfoManager(this)), // Phase 5
      m_systemMonitor(new SystemMonitor(this)), // Phase 3
      m_topBarManager(new TopBarManager(this)), // Phase 6.1
      m_remoteClientInfoWrapper(nullptr),
      m_screenCanvas(nullptr),
      m_uploadButton(nullptr),
      m_uploadButtonInOverlay(false),
      m_remoteOverlayActionsEnabled(false),
      m_responsiveLayoutManager(new ResponsiveLayoutManager(this)),
      m_cursorTimer(nullptr),
      m_menuBarManager(new MenuBarManager(this, this)), // Phase 6.3
      m_systemTrayManager(new SystemTrayManager(this)), // Phase 6.2
      m_webSocketClient(new WebSocketClient(this)),
      m_webSocketMessageHandler(new WebSocketMessageHandler(this, this)), // Phase 7.1
      m_screenEventHandler(new ScreenEventHandler(this, this)), // Phase 7.2
      m_uploadEventHandler(new UploadEventHandler(this, this)), // Phase 7.4
      m_clientListEventHandler(new ClientListEventHandler(this, m_webSocketClient, this)), // Phase 7.3
      m_statusUpdateTimer(new QTimer(this)),
      m_displaySyncTimer(new QTimer(this)),
      m_reconnectTimer(new QTimer(this)),
      m_reconnectAttempts(0),
      m_maxReconnectDelay(15000),
      m_uploadManager(new UploadManager(m_fileManager, this)),
      m_watchManager(new WatchManager(this)),
      m_fileWatcher(new FileWatcher(this)),
      m_navigationManager(nullptr)
{
    setWindowTitle("Mouffette");
#if defined(Q_OS_WIN)
    setWindowIcon(QIcon(":/icons/appicon.ico"));
#endif
    // Load persisted settings (server URL, auto-upload, persistent client ID)
    {
        QSettings settings("Mouffette", "Client");
        m_serverUrlConfig = settings.value("serverUrl", DEFAULT_SERVER_URL).toString();
        m_autoUploadImportedMedia = settings.value("autoUploadImportedMedia", false).toBool();
        
        // NEW: Generate or load persistent client ID
        QString machineId = QSysInfo::machineUniqueId();
        if (machineId.isEmpty()) {
            machineId = QHostInfo::localHostName();
        }
        if (machineId.isEmpty()) {
            machineId = QStringLiteral("unknown-machine");
        }
        QString sanitizedMachineId = machineId;
        sanitizedMachineId.replace(QRegularExpression("[^A-Za-z0-9_]"), "_");

        QString instanceSuffix = qEnvironmentVariable("MOUFFETTE_INSTANCE_SUFFIX");
        if (instanceSuffix.isEmpty()) {
            const QStringList args = QCoreApplication::arguments();
            for (int i = 1; i < args.size(); ++i) {
                const QString& arg = args.at(i);
                if (arg.startsWith("--instance-suffix=")) {
                    instanceSuffix = arg.section('=', 1);
                    break;
                }
                if (arg == "--instance-suffix" && (i + 1) < args.size()) {
                    instanceSuffix = args.at(i + 1);
                    break;
                }
            }
        }
        QString sanitizedInstanceSuffix = instanceSuffix;
        sanitizedInstanceSuffix.replace(QRegularExpression("[^A-Za-z0-9_]"), "_");

        const QByteArray installHashBytes = QCryptographicHash::hash(QCoreApplication::applicationDirPath().toUtf8(), QCryptographicHash::Sha1);
        QString installFingerprint = QString::fromUtf8(installHashBytes.toHex());
        if (installFingerprint.isEmpty()) {
            installFingerprint = QStringLiteral("unknowninstall");
        }
        installFingerprint = installFingerprint.left(16); // keep key compact

        QString settingsKey = QStringLiteral("persistentClientId_%1_%2").arg(sanitizedMachineId, installFingerprint);
        if (!sanitizedInstanceSuffix.isEmpty()) {
            settingsKey += QStringLiteral("_%1").arg(sanitizedInstanceSuffix);
        }

        QString persistentClientId = settings.value(settingsKey).toString();
        if (persistentClientId.isEmpty()) {
            persistentClientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            settings.setValue(settingsKey, persistentClientId);
            qDebug() << "MainWindow: Generated new persistent client ID:" << persistentClientId
                     << "using key" << settingsKey << "machineId:" << machineId
                     << "instanceSuffix:" << sanitizedInstanceSuffix;
        } else {
            qDebug() << "MainWindow: Loaded persistent client ID:" << persistentClientId
                     << "using key" << settingsKey << "machineId:" << machineId
                     << "instanceSuffix:" << sanitizedInstanceSuffix;
        }
        m_webSocketClient->setPersistentClientId(persistentClientId);
    }
    // Use standard OS window frame and title bar (no custom frameless window)
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    // Optional: keep a clean look (no menu bar) while using native title bar
    setContextMenuPolicy(Qt::NoContextMenu);
    setMenuBar(nullptr);
#endif
    // Remove any minimum height constraint to allow full flexibility
    setMinimumHeight(0);
    // Set reasonable minimum width to prevent window from becoming unusable and avoid UI element compression
    setMinimumWidth(600);
    // Set window to maximized state to fill available workspace
    setWindowState(Qt::WindowMaximized);
    
    setupUI();
    // Initialize remote scene controller once
    if (!g_remoteSceneController) {
        g_remoteSceneController = new RemoteSceneController(m_fileManager, m_webSocketClient, this);
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    // Ensure no status bar is shown at the bottom
    if (QStatusBar* sb = findChild<QStatusBar*>()) {
        sb->deleteLater();
    }
#endif
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    // [PHASE 6.3] Setup menu bar
    if (m_menuBarManager) {
        m_menuBarManager->setup();
        connect(m_menuBarManager, &MenuBarManager::quitRequested, this, &MainWindow::onMenuQuitRequested);
        connect(m_menuBarManager, &MenuBarManager::aboutRequested, this, &MainWindow::onMenuAboutRequested);
    }
#endif
    
    // [PHASE 6.2] Setup system tray
    if (m_systemTrayManager) {
        m_systemTrayManager->setup();
        connect(m_systemTrayManager, &SystemTrayManager::activated, this, &MainWindow::onTrayIconActivated);
    }
    
    // [PHASE 3] Start system monitoring
    if (m_systemMonitor) {
        m_systemMonitor->startVolumeMonitoring();
        // Connect volume changes to sync with server when watched
        connect(m_systemMonitor, &SystemMonitor::volumeChanged, this, [this](int) {
            if (m_webSocketClient && m_webSocketClient->isConnected() && m_isWatched) {
                syncRegistration();
            }
        });
    }

    // [PHASE 7.1] Setup WebSocket message handler connections
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->setupConnections(m_webSocketClient);
    }
    
    // [PHASE 7.2] Setup screen event handler connections
    if (m_screenEventHandler) {
        m_screenEventHandler->setupConnections(m_webSocketClient);
    }
    
    // [PHASE 7.3] Setup client list event handler connections
    if (m_clientListEventHandler) {
        m_clientListEventHandler->setupConnections(m_webSocketClient);
    }
    
    // Connect remaining WebSocketClient signals (non-lifecycle)
    connect(m_webSocketClient, &WebSocketClient::connectionError, this, &MainWindow::onConnectionError);
    // Immediate status reflection without polling
    connect(m_webSocketClient, &WebSocketClient::connectionStatusChanged, this, [this](const QString& s){ setLocalNetworkStatus(s); });
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    connect(m_webSocketClient, &WebSocketClient::watchStatusChanged, this, &MainWindow::onWatchStatusChanged);
    // Unused generic message hook removed; specific handlers are wired explicitly
    // Forward all generic messages to UploadManager so it can handle incoming upload_* and remove_all_files when we are the target
    connect(m_webSocketClient, &WebSocketClient::messageReceived, m_uploadManager, &UploadManager::handleIncomingMessage);
    // Upload progress forwards
    connect(m_webSocketClient, &WebSocketClient::uploadProgressReceived, m_uploadManager, &UploadManager::onUploadProgress);
    connect(m_webSocketClient, &WebSocketClient::uploadFinishedReceived, m_uploadManager, &UploadManager::onUploadFinished);
    // New: per-file completion ids
    connect(m_webSocketClient, &WebSocketClient::uploadCompletedFileIdsReceived, m_uploadManager, &UploadManager::onUploadCompletedFileIds);
    connect(m_webSocketClient, &WebSocketClient::allFilesRemovedReceived, m_uploadManager, &UploadManager::onAllFilesRemovedRemote);

    // Managers wiring
    m_uploadManager->setWebSocketClient(m_webSocketClient);
    m_watchManager->setWebSocketClient(m_webSocketClient);
    
    // PHASE 2: SessionManager signal connections (canvas lifecycle tracking)
    connect(m_sessionManager, &SessionManager::sessionDeleted, this, [this](const QString& persistentClientId) {
        // Notify server when canvas is deleted
        CanvasSession* session = m_sessionManager->findSession(persistentClientId);
        if (session && m_webSocketClient) {
            m_webSocketClient->sendCanvasDeleted(persistentClientId, session->canvasSessionId);
        }
    });
    
    // FileManager: configure callback to send file removal commands to remote clients
    // Phase 3: canvasSessionId is MANDATORY - always present from SessionManager
    FileManager::setFileRemovalNotifier([this](const QString& fileId, const QList<QString>& clientIds, const QList<QString>& canvasSessionIds) {
        qDebug() << "MainWindow: FileManager requested removal of file" << fileId
                 << "from clients:" << clientIds << "ideas:" << canvasSessionIds;
        if (!m_webSocketClient) {
            qWarning() << "MainWindow: No WebSocket client available for file removal";
            return;
        }
        
        for (const QString& canvasSessionId : canvasSessionIds) {
            for (const QString& clientId : clientIds) {
                qDebug() << "MainWindow: Sending remove_file command for" << fileId
                         << "to" << clientId << "canvasSessionId" << canvasSessionId;
                m_webSocketClient->sendRemoveFile(clientId, canvasSessionId, fileId);
            }
            
            if (CanvasSession* session = findCanvasSessionByIdeaId(canvasSessionId)) {
                session->knownRemoteFileIds.remove(fileId);
                session->expectedIdeaFileIds.remove(fileId);
            }
        }
    });
    
    // FileWatcher: remove media items when their source files are deleted
    connect(m_fileWatcher, &FileWatcher::filesDeleted, this, [this](const QList<ResizableMediaBase*>& mediaItems) {
        if (!m_screenCanvas || !m_screenCanvas->scene()) return;
        
        qDebug() << "MainWindow: Removing" << mediaItems.size() << "media items due to deleted source files";
        
        for (ResizableMediaBase* mediaItem : mediaItems) {
            // Safety check: ensure the mediaItem pointer is valid
            if (!mediaItem) {
                qDebug() << "MainWindow: Skipping null mediaItem in filesDeleted callback";
                continue;
            }
            
            // Additional safety: check if mediaId is valid
            QString mediaId = mediaItem->mediaId();
            if (mediaId.isEmpty() || mediaId.contains('\0')) {
                qDebug() << "MainWindow: Skipping mediaItem with invalid mediaId:" << mediaId;
                continue;
            }
            
            // Stop watching this item
            m_fileWatcher->unwatchMediaItem(mediaItem);
            
            // Use the same safe deletion pattern as the delete button
            // Defer deletion to the event loop to avoid re-entrancy issues
            QTimer::singleShot(0, [itemPtr=mediaItem]() {
                if (itemPtr) { // Extra safety check in lambda
                    itemPtr->prepareForDeletion();
                    if (itemPtr->scene()) itemPtr->scene()->removeItem(itemPtr);
                    delete itemPtr;
                }
            });
        }
        
        // Refresh the media list overlay if it's visible (also deferred)
        if (m_screenCanvas) {
            QTimer::singleShot(0, [this]() {
                if (m_screenCanvas) {
                    m_screenCanvas->refreshInfoOverlay();
                }
            });
        }
    });
    
    // Phase 4.3: Inject FileManager into media items (static setter)
    ResizableMediaBase::setFileManager(m_fileManager);
    
    // File error callback: remove media items when playback detects missing/corrupted files
    ResizableMediaBase::setFileErrorNotifier([this](ResizableMediaBase* mediaItem) {
        if (!m_screenCanvas || !m_screenCanvas->scene() || !mediaItem) return;
        
        // Additional safety: check if mediaId is valid
        QString mediaId = mediaItem->mediaId();
        if (mediaId.isEmpty() || mediaId.contains('\0')) {
            qDebug() << "MainWindow: Skipping mediaItem with invalid mediaId in file error callback:" << mediaId;
            return;
        }
        
        qDebug() << "MainWindow: Removing media item due to file error:" << mediaItem->sourcePath();
        
        // Stop watching this item
        m_fileWatcher->unwatchMediaItem(mediaItem);
        
        // Use the same safe deletion pattern as the delete button
        // Defer deletion to the event loop to avoid re-entrancy issues
        QTimer::singleShot(0, [itemPtr=mediaItem]() {
            itemPtr->prepareForDeletion();
            if (itemPtr->scene()) itemPtr->scene()->removeItem(itemPtr);
            delete itemPtr;
        });
        
        // Refresh the media list overlay (also deferred)
        QTimer::singleShot(0, [this]() {
            if (m_screenCanvas) {
                m_screenCanvas->refreshInfoOverlay();
            }
        });
    });

    // UI refresh when upload state changes
    auto applyUploadButtonStyle = [this]() {
        if (!m_uploadButton) return;
        
        // Recalculate stored default font so runtime changes to typography propagate.
        AppColors::applyCanvasButtonFont(m_uploadButtonDefaultFont);
        const QString canvasFontCss = AppColors::canvasButtonFontCss();
        
    // If button is in overlay, use custom overlay styling
        if (m_uploadButtonInOverlay) {
            if (!m_remoteOverlayActionsEnabled) {
                m_uploadButton->setEnabled(false);
                m_uploadButton->setCheckable(false);
                m_uploadButton->setChecked(false);
                m_uploadButton->setStyleSheet(ScreenCanvas::overlayDisabledButtonStyle());
                m_uploadButton->setFont(m_uploadButtonDefaultFont);
                m_uploadButton->setFixedHeight(40);
                m_uploadButton->setMaximumWidth(uploadButtonMaxWidth());
                return;
            }
            const QString overlayIdleStyle = QString(
                "QPushButton { "
                "    padding: 0px 20px; "
                "    %1 "
                "    color: %2; "
                "    background: transparent; "
                "    border: none; "
                "    border-radius: 0px; "
                "    text-align: center; "
                "} "
                "QPushButton:hover { "
                "    color: white; "
                "    background: rgba(255,255,255,0.05); "
                "} "
                "QPushButton:pressed { "
                "    color: white; "
                "    background: rgba(255,255,255,0.1); "
                "}"
            ).arg(canvasFontCss, AppColors::colorToCss(AppColors::gOverlayTextColor));
            const QString overlayUploadingStyle = QString(
                "QPushButton { "
                "    padding: 0px 20px; "
                "    %1 "
                "    color: %2; "
                "    background: %3; "
                "    border: none; "
                "    border-radius: 0px; "
                "    text-align: center; "
                "} "
                "QPushButton:hover { "
                "    color: %2; "
                "    background: %4; "
                "} "
                "QPushButton:pressed { "
                "    color: %2; "
                "    background: %5; "
                "}"
            ).arg(canvasFontCss,
                  AppColors::gBrandBlue.name(),
                  AppColors::colorToCss(AppColors::gButtonPrimaryBg),
                  AppColors::colorToCss(AppColors::gButtonPrimaryHover),
                  AppColors::colorToCss(AppColors::gButtonPrimaryPressed));
            const QString overlayUnloadStyle = QString(
                "QPushButton { "
                "    padding: 0px 20px; "
                "    %1 "
                "    color: %2; "
                "    background: %3; "
                "    border: none; "
                "    border-radius: 0px; "
                "    text-align: center; "
                "} "
                "QPushButton:hover { "
                "    color: %2; "
                "    background: rgba(76, 175, 80, 56); "
                "} "
                "QPushButton:pressed { "
                "    color: %2; "
                "    background: rgba(76, 175, 80, 77); "
                "}"
            ).arg(canvasFontCss,
                  AppColors::colorToCss(AppColors::gMediaUploadedColor),
                  AppColors::colorToCss(AppColors::gStatusConnectedBg));
            
            const QString target = m_uploadManager->targetClientId();
            const CanvasSession* targetSession = findCanvasSessionByServerClientId(target);
            const bool sessionHasRemote = targetSession && targetSession->upload.remoteFilesPresent;
            const bool managerHasActiveForTarget = m_uploadManager->hasActiveUpload() &&
                                                   m_uploadManager->activeUploadTargetClientId() == target;
            const bool remoteActive = sessionHasRemote || managerHasActiveForTarget;
            
            // Check if remote scene is launched on the canvas that owns this upload button
            bool remoteSceneLaunched = false;
            for (CanvasSession* session : m_sessionManager->getAllSessions()) {
                if (session->uploadButton == m_uploadButton && session->canvas) {
                    remoteSceneLaunched = session->canvas->isRemoteSceneLaunched();
                    break;
                }
            }

            if (m_uploadManager->isUploading()) {
                if (m_uploadManager->isCancelling()) {
                    m_uploadButton->setText("Cancelling…");
                    m_uploadButton->setEnabled(false);
                    m_uploadButton->setFont(m_uploadButtonDefaultFont);
                } else {
                    if (m_uploadButton->text() == "Upload") {
                        m_uploadButton->setText("Preparing");
                    }
                    m_uploadButton->setEnabled(true);
                    // Switch to monospace font for stable width while showing progress
#ifdef Q_OS_MACOS
                    QFont mono("Menlo");
#else
                    QFont mono("Courier New");
#endif
                    AppColors::applyCanvasButtonFont(mono);
                    m_uploadButton->setFont(mono);
                }
                m_uploadButton->setStyleSheet(overlayUploadingStyle);
            } else if (m_uploadManager->isFinalizing()) {
                m_uploadButton->setText("Finalizing…");
                m_uploadButton->setEnabled(false);
                m_uploadButton->setStyleSheet(overlayUploadingStyle);
                m_uploadButton->setFont(m_uploadButtonDefaultFont);
            } else if (remoteActive) {
                // If there are newly added items not yet uploaded to the target, switch back to Upload
                const bool hasUnuploaded = hasUnuploadedFilesForTarget(target);
                // If target is unknown for any reason, default to offering Upload rather than Unload
                if (target.isEmpty() || hasUnuploaded) {
                    m_uploadButton->setText("Upload");
                    m_uploadButton->setEnabled(!remoteSceneLaunched); // Disable if remote scene is active
                    m_uploadButton->setStyleSheet(remoteSceneLaunched ? ScreenCanvas::overlayDisabledButtonStyle() : overlayIdleStyle);
                    m_uploadButton->setFont(m_uploadButtonDefaultFont);
                } else {
                    m_uploadButton->setText("Unload");
                    m_uploadButton->setEnabled(!remoteSceneLaunched); // Disable if remote scene is active
                    m_uploadButton->setStyleSheet(remoteSceneLaunched ? ScreenCanvas::overlayDisabledButtonStyle() : overlayUnloadStyle);
                    m_uploadButton->setFont(m_uploadButtonDefaultFont);
                }
            } else {
                m_uploadButton->setText("Upload");
                m_uploadButton->setEnabled(!remoteSceneLaunched); // Disable if remote scene is active
                m_uploadButton->setStyleSheet(remoteSceneLaunched ? ScreenCanvas::overlayDisabledButtonStyle() : overlayIdleStyle);
                m_uploadButton->setFont(m_uploadButtonDefaultFont);
            }
            m_uploadButton->setFixedHeight(40);
            m_uploadButton->setMaximumWidth(uploadButtonMaxWidth());
            return;
        }
        
        if (!m_remoteOverlayActionsEnabled) {
            m_uploadButton->setEnabled(false);
            m_uploadButton->setCheckable(false);
            m_uploadButton->setChecked(false);
            return;
        }

        // Base style strings using gDynamicBox configuration for regular buttons
        const QString greyStyle = QString(
            "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %3px; background-color: %4; color: white; border-radius: %1px; min-height: %2px; max-height: %2px; } "
            "QPushButton:checked { background-color: %5; }"
        ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(gDynamicBoxFontPx).arg(AppColors::colorToCss(AppColors::gButtonGreyBg)).arg(AppColors::colorToCss(AppColors::gButtonGreyPressed));
        const QString blueStyle = QString(
            "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %3px; background-color: %4; color: white; border-radius: %1px; min-height: %2px; max-height: %2px; } "
            "QPushButton:checked { background-color: %5; }"
        ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(gDynamicBoxFontPx).arg(AppColors::colorToCss(AppColors::gButtonBlueBg)).arg(AppColors::colorToCss(AppColors::gButtonBluePressed));
        const QString greenStyle = QString(
            "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %3px; background-color: %4; color: white; border-radius: %1px; min-height: %2px; max-height: %2px; } "
            "QPushButton:checked { background-color: %5; }"
        ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(gDynamicBoxFontPx).arg(AppColors::colorToCss(AppColors::gButtonGreenBg)).arg(AppColors::colorToCss(AppColors::gButtonGreenPressed));

    const QString target = m_uploadManager->targetClientId();
    const CanvasSession* targetSession = findCanvasSessionByServerClientId(target);
    const bool sessionHasRemote = targetSession && targetSession->upload.remoteFilesPresent;
    const bool managerHasActiveForTarget = m_uploadManager->hasActiveUpload() &&
                           m_uploadManager->activeUploadTargetClientId() == target;
    const bool remoteActive = sessionHasRemote || managerHasActiveForTarget;

        if (m_uploadManager->isUploading()) {
            // Upload in progress (preparing or actively streaming): show preparing or cancelling state handled elsewhere
            if (m_uploadManager->isCancelling()) {
                m_uploadButton->setText("Cancelling…");
                m_uploadButton->setEnabled(false);
            } else {
                // Initial immediate state after click before first progress message
                if (m_uploadButton->text() == "Upload to Client") {
                    m_uploadButton->setText("Preparing download");
                }
                m_uploadButton->setEnabled(true);
            }
            m_uploadButton->setCheckable(true);
            m_uploadButton->setChecked(true);
            m_uploadButton->setStyleSheet(blueStyle);
            m_uploadButton->setFixedHeight(gDynamicBoxHeight);
            m_uploadButton->setMaximumWidth(uploadButtonMaxWidth());
            // Monospace font for stability
#ifdef Q_OS_MACOS
            QFont mono("Menlo");
#else
            QFont mono("Courier New");
#endif
            AppColors::applyCanvasButtonFont(mono);
            m_uploadButton->setFont(mono);
        } else if (m_uploadManager->isFinalizing()) {
            // Waiting for server to ack upload_finished
            m_uploadButton->setCheckable(true);
            m_uploadButton->setChecked(true);
            m_uploadButton->setEnabled(false);
            m_uploadButton->setText("Finalizing…");
            m_uploadButton->setStyleSheet(blueStyle);
            m_uploadButton->setFixedHeight(gDynamicBoxHeight);
            m_uploadButton->setMaximumWidth(uploadButtonMaxWidth());
            m_uploadButton->setFont(m_uploadButtonDefaultFont);
        } else if (remoteActive) {
            // If there are new unuploaded files, return to Upload state; otherwise offer unload
            // If target is unknown for any reason, default to offering Upload rather than Unload
            if (target.isEmpty() || hasUnuploadedFilesForTarget(target)) {
                m_uploadButton->setCheckable(false);
                m_uploadButton->setChecked(false);
                m_uploadButton->setEnabled(true);
                m_uploadButton->setText("Upload to Client");
                m_uploadButton->setStyleSheet(greyStyle);
                m_uploadButton->setFixedHeight(gDynamicBoxHeight);
                m_uploadButton->setMaximumWidth(uploadButtonMaxWidth());
                m_uploadButton->setFont(m_uploadButtonDefaultFont);
            } else {
                // Uploaded & resident on target: allow unload
                m_uploadButton->setCheckable(true);
                m_uploadButton->setChecked(true);
                m_uploadButton->setEnabled(true);
                m_uploadButton->setText("Remove all files");
                m_uploadButton->setStyleSheet(greenStyle);
                m_uploadButton->setFixedHeight(gDynamicBoxHeight);
                m_uploadButton->setMaximumWidth(uploadButtonMaxWidth());
                m_uploadButton->setFont(m_uploadButtonDefaultFont);
            }
        } else {
            // Idle state
            m_uploadButton->setCheckable(false);
            m_uploadButton->setChecked(false);
            m_uploadButton->setEnabled(true);
            m_uploadButton->setText("Upload to Client");
            m_uploadButton->setStyleSheet(greyStyle);
            m_uploadButton->setFixedHeight(gDynamicBoxHeight);
            m_uploadButton->setMaximumWidth(uploadButtonMaxWidth());
            m_uploadButton->setFont(m_uploadButtonDefaultFont);
        }
    };
    connect(m_uploadManager, &UploadManager::uiStateChanged, this, applyUploadButtonStyle);
    connect(m_uploadManager, &UploadManager::uploadProgress, this, [this, applyUploadButtonStyle](int percent, int filesCompleted, int totalFiles){
        if (!m_uploadButton) return;
        if ((m_uploadManager->isUploading() || m_uploadManager->isFinalizing()) && !m_uploadManager->isCancelling()) {
            if (m_uploadManager->isFinalizing()) {
                m_uploadButton->setText("Finalizing…");
            } else {
            m_uploadButton->setText(QString("Uploading (%1/%2) %3%")
                                    .arg(filesCompleted)
                                    .arg(totalFiles)
                                    .arg(percent));
            }
        }
        applyUploadButtonStyle();
        
        // Update individual media progress based on server-acknowledged data
        updateIndividualProgressFromServer(percent, filesCompleted, totalFiles);
    });
    connect(m_uploadManager, &UploadManager::uploadFinished, this, [this, applyUploadButtonStyle](){ applyUploadButtonStyle(); });
    connect(m_uploadManager, &UploadManager::allFilesRemoved, this, [this, applyUploadButtonStyle](){ applyUploadButtonStyle(); });

    // Periodic connection status refresh no longer needed (now event-driven); keep timer disabled
    m_statusUpdateTimer->stop();

    // Periodic display sync only when watched
    m_displaySyncTimer->setInterval(3000);
    connect(m_displaySyncTimer, &QTimer::timeout, this, [this]() { if (m_isWatched && m_webSocketClient->isConnected()) syncRegistration(); });
    // Don't start automatically - will be started when watched

    // Smart reconnect timer
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::attemptReconnect);

    // Initialize toast notification system
    m_toastSystem = new ToastNotificationSystem(this, this);
    ToastNotificationSystem::setInstance(m_toastSystem);

    connectToServer();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // Block space bar from triggering button presses when focus is on stack/canvas container
    QStackedWidget* canvasStack = m_canvasViewPage ? m_canvasViewPage->getCanvasStack() : nullptr;
    QWidget* screenViewWidget = m_canvasViewPage;
    
    if ((obj == m_stackedWidget || obj == canvasStack || obj == screenViewWidget) && event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Space) { event->accept(); return true; }
    }

    return QMainWindow::eventFilter(obj, event);
}

// [PHASE 5] Delegate to RemoteClientInfoManager
void MainWindow::createRemoteClientInfoContainer() {
    if (m_remoteClientInfoManager) {
        m_remoteClientInfoManager->createContainer();
    }
}

// [PHASE 5] Delegate to RemoteClientInfoManager
void MainWindow::removeRemoteStatusFromLayout() {
    if (m_remoteClientInfoManager) {
        m_remoteClientInfoManager->removeRemoteStatusFromLayout();
    }
    updateApplicationSuspendedState(false);
}

// [PHASE 5] Delegate to RemoteClientInfoManager
void MainWindow::addRemoteStatusToLayout() {
    if (m_remoteClientInfoManager) {
        m_remoteClientInfoManager->addRemoteStatusToLayout();
    }
}

// [PHASE 5] Delegate to RemoteClientInfoManager
void MainWindow::removeVolumeIndicatorFromLayout() {
    if (m_remoteClientInfoManager) {
        m_remoteClientInfoManager->removeVolumeIndicatorFromLayout();
    }
}

// [PHASE 5] Delegate to RemoteClientInfoManager
void MainWindow::addVolumeIndicatorToLayout() {
    if (m_remoteClientInfoManager) {
        m_remoteClientInfoManager->addVolumeIndicatorToLayout();
    }
}

// [PHASE 7.2] Accessor for ScreenEventHandler
void MainWindow::stopInlineSpinner() {
    if (m_inlineSpinner && m_inlineSpinner->isSpinning()) {
        m_inlineSpinner->stop();
        m_inlineSpinner->hide();
    }
}

// [PHASE 7.3] Accessors for ClientListEventHandler
bool MainWindow::isInlineSpinnerSpinning() const {
    return m_inlineSpinner && m_inlineSpinner->isSpinning();
}

void MainWindow::showInlineSpinner() {
    if (m_inlineSpinner) {
        m_inlineSpinner->show();
    }
}

void MainWindow::startInlineSpinner() {
    if (m_inlineSpinner) {
        m_inlineSpinner->start();
    }
}

// [PHASE 6.1] Delegate to TopBarManager
void MainWindow::createLocalClientInfoContainer() {
    if (m_topBarManager) {
        m_topBarManager->createLocalClientInfoContainer();
    }
}

// [PHASE 6.1] Delegate to TopBarManager
// [PHASE 6.1] Delegate to TopBarManager
void MainWindow::setLocalNetworkStatus(const QString& status) {
    if (m_topBarManager) {
        m_topBarManager->setLocalNetworkStatus(status);
    }
}

// [PHASE 5] Updated to use RemoteClientInfoManager
void MainWindow::initializeRemoteClientInfoInTopBar() {
    // Get the container from the manager
    QWidget* container = m_remoteClientInfoManager ? m_remoteClientInfoManager->getContainer() : nullptr;
    
    // Create the container if it doesn't exist
    if (!container) {
        createRemoteClientInfoContainer();
        container = m_remoteClientInfoManager ? m_remoteClientInfoManager->getContainer() : nullptr;
    }
    
    // Create wrapper widget to hold container + inline spinner side by side (once)
    if (!m_remoteClientInfoWrapper) {
        m_remoteClientInfoWrapper = new QWidget();
        auto* wrapperLayout = new QHBoxLayout(m_remoteClientInfoWrapper);
        wrapperLayout->setContentsMargins(0, 0, 0, 0);
        wrapperLayout->setSpacing(8); // 8px gap between container and spinner
        if (container) {
            wrapperLayout->addWidget(container);
        }

        if (!m_inlineSpinner) {
            m_inlineSpinner = new SpinnerWidget(m_remoteClientInfoWrapper);
            const int spinnerSize = gDynamicBoxHeight;
            m_inlineSpinner->setRadius(qMax(8, spinnerSize / 2 - 2));
            m_inlineSpinner->setLineWidth(qMax(2, spinnerSize / 6));
            m_inlineSpinner->setColor(QColor("#4a90e2"));
            m_inlineSpinner->setFixedSize(spinnerSize, spinnerSize);
            m_inlineSpinner->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            m_inlineSpinner->hide();
        } else {
            m_inlineSpinner->setParent(m_remoteClientInfoWrapper);
            m_inlineSpinner->hide();
        }

        if (m_inlineSpinner) {
            wrapperLayout->addWidget(m_inlineSpinner, 0, Qt::AlignVCenter);
        }
    } else {
        // Ensure container and spinner belong to the wrapper layout
        if (container && container->parent() != m_remoteClientInfoWrapper) {
            container->setParent(m_remoteClientInfoWrapper);
        }
        auto* wrapperLayout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoWrapper->layout());
        if (container && wrapperLayout && wrapperLayout->indexOf(container) == -1) {
            wrapperLayout->insertWidget(0, container);
        }
        if (m_inlineSpinner) {
            if (m_inlineSpinner->parent() != m_remoteClientInfoWrapper) {
                m_inlineSpinner->setParent(m_remoteClientInfoWrapper);
            }
            if (wrapperLayout && wrapperLayout->indexOf(m_inlineSpinner) == -1) {
                wrapperLayout->addWidget(m_inlineSpinner, 0, Qt::AlignVCenter);
            }
            m_inlineSpinner->hide();
        }
    }

    // Initially hide the wrapper (and implicitly the container)
    if (m_remoteClientInfoWrapper) {
        m_remoteClientInfoWrapper->setVisible(false);
    }
    
    // Add wrapper to connection layout after back button permanently
    int backButtonIndex = -1;
    for (int i = 0; i < m_connectionLayout->count(); ++i) {
        if (auto* item = m_connectionLayout->itemAt(i)) {
            if (item->widget() == m_backButton) {
                backButtonIndex = i;
                break;
            }
        }
    }
    
    if (backButtonIndex >= 0 && m_remoteClientInfoWrapper) {
        m_connectionLayout->insertWidget(backButtonIndex + 1, m_remoteClientInfoWrapper);
    }
}

// Phase 4.1: Migrated to use SessionManager
MainWindow::CanvasSession* MainWindow::findCanvasSession(const QString& persistentClientId) {
    return m_sessionManager->findSession(persistentClientId);
}

const MainWindow::CanvasSession* MainWindow::findCanvasSession(const QString& persistentClientId) const {
    return m_sessionManager->findSession(persistentClientId);
}

MainWindow::CanvasSession* MainWindow::findCanvasSessionByServerClientId(const QString& serverClientId) {
    return m_sessionManager->findSessionByServerClientId(serverClientId);
}

const MainWindow::CanvasSession* MainWindow::findCanvasSessionByServerClientId(const QString& serverClientId) const {
    return m_sessionManager->findSessionByServerClientId(serverClientId);
}

MainWindow::CanvasSession* MainWindow::findCanvasSessionByIdeaId(const QString& canvasSessionId) {
    return m_sessionManager->findSessionByIdeaId(canvasSessionId);
}

MainWindow::CanvasSession& MainWindow::ensureCanvasSession(const ClientInfo& client) {
    QString persistentId = client.clientId();
    if (persistentId.isEmpty()) {
        qWarning() << "MainWindow::ensureCanvasSession: client has no persistentClientId, this should not happen";
        persistentId = client.getId();
    }
    
    // Check if session already exists
    bool isNewSession = !m_sessionManager->hasSession(persistentId);
    
    // Phase 4.1: Use SessionManager (creates canvasSessionId automatically)
    CanvasSession& session = m_sessionManager->getOrCreateSession(persistentId, client);
    
    // PHASE 2: Notify server of canvas creation (CRITICAL for canvasSessionId validation)
    if (isNewSession && m_webSocketClient) {
        m_webSocketClient->sendCanvasCreated(persistentId, session.canvasSessionId);
    }
    
    // Initialize canvas if needed (UI-specific responsibility)
    if (!session.canvas) {
        QStackedWidget* canvasHostStack = m_canvasViewPage ? m_canvasViewPage->getCanvasHostStack() : nullptr;
        if (!canvasHostStack) {
            qWarning() << "Cannot create canvas: CanvasViewPage not initialized";
            return session;
        }
        session.canvas = new ScreenCanvas(canvasHostStack);
        session.canvas->setActiveIdeaId(session.canvasSessionId); // Use canvasSessionId from SessionManager
        session.connectionsInitialized = false;
        configureCanvasSession(session);
        canvasHostStack->addWidget(session.canvas);
    }
    
    // Update remote target
    if (!session.persistentClientId.isEmpty()) {
        session.canvas->setRemoteSceneTarget(session.persistentClientId, session.lastClientInfo.getMachineName());
    }
    
    // Update online status
    if (session.lastClientInfo.isOnline()) {
        session.remoteContentClearedOnDisconnect = false;
    }
    
    // Phase 1.1: Refresh ongoing scenes via ClientListPage
    if (m_clientListPage) {
        m_clientListPage->refreshOngoingScenesList();
    }
    return session;
}

void MainWindow::configureCanvasSession(CanvasSession& session) {
    if (!session.canvas) return;

    session.canvas->setActiveIdeaId(session.canvasSessionId);
    session.canvas->setWebSocketClient(m_webSocketClient);
    session.canvas->setUploadManager(m_uploadManager);
    session.canvas->setFileManager(m_fileManager);
    session.canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    session.canvas->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    session.canvas->setFocusPolicy(Qt::StrongFocus);
    session.canvas->installEventFilter(this);

    connect(session.canvas, &ScreenCanvas::remoteSceneLaunchStateChanged, this,
            &MainWindow::onRemoteSceneLaunchStateChanged,
            Qt::UniqueConnection);

    if (session.canvas->viewport()) {
        QWidget* viewport = session.canvas->viewport();
        viewport->setAttribute(Qt::WA_StyledBackground, true);
        viewport->setAutoFillBackground(true);
        viewport->setStyleSheet("background: palette(base); border: none; border-radius: 5px;");
        viewport->installEventFilter(this);
    }

    if (!session.connectionsInitialized) {
        connect(session.canvas, &ScreenCanvas::mediaItemAdded, this,
                [this, persistentId=session.persistentClientId](ResizableMediaBase* mediaItem) {
                    if (m_fileWatcher && mediaItem && !mediaItem->sourcePath().isEmpty()) {
                        m_fileWatcher->watchMediaItem(mediaItem);
                        qDebug() << "MainWindow: Added media item to file watcher:" << mediaItem->sourcePath();
                    }
                    CanvasSession* sess = m_sessionManager->findSession(persistentId);
                    if (sess) {
                        sess->lastClientInfo.setFromMemory(true);
                    }
                    // Update upload button state immediately when media is added
                    if (m_uploadManager) {
                        emit m_uploadManager->uiStateChanged();
                    }
                    if (m_autoUploadImportedMedia && m_uploadManager && !m_uploadManager->isUploading() && !m_uploadManager->isCancelling()) {
                        QTimer::singleShot(0, this, [this]() { onUploadButtonClicked(); });
                    }
                });
        
        connect(session.canvas, &ScreenCanvas::mediaItemRemoved, this,
                [this](ResizableMediaBase*) {
                    // Update upload button state immediately when media is removed
                    if (m_uploadManager) {
                        emit m_uploadManager->uiStateChanged();
                    }
                });
    }

    if (QPushButton* overlayBtn = session.canvas->getUploadButton()) {
        if (session.uploadButton != overlayBtn) {
            connect(overlayBtn, &QPushButton::clicked, this, &MainWindow::onUploadButtonClicked, Qt::UniqueConnection);
        }
        session.uploadButton = overlayBtn;
        session.uploadButtonInOverlay = true;
        session.uploadButtonDefaultFont = overlayBtn->font();
    } else {
        session.uploadButton = nullptr;
        session.uploadButtonInOverlay = false;
        session.uploadButtonDefaultFont = QFont();
    }

    session.connectionsInitialized = true;
}

void MainWindow::switchToCanvasSession(const QString& persistentClientId) {
    // Navigation between clients should NOT trigger unload - uploads persist per session
    // Unload only happens when explicitly requested via button or when remote disconnects
    
    CanvasSession* session = findCanvasSession(persistentClientId);
    if (!session || !session->canvas) return;

    m_activeSessionIdentity = persistentClientId;
    m_screenCanvas = session->canvas;
    if (m_navigationManager) {
        m_navigationManager->setActiveCanvas(m_screenCanvas);
    }

    QStackedWidget* canvasHostStack = m_canvasViewPage ? m_canvasViewPage->getCanvasHostStack() : nullptr;
    if (canvasHostStack) {
        if (canvasHostStack->indexOf(session->canvas) == -1) {
            canvasHostStack->addWidget(session->canvas);
        }
        canvasHostStack->setCurrentWidget(session->canvas);
    }

    session->canvas->setFocus(Qt::OtherFocusReason);
    // Phase 3: Use persistentClientId for server communication
    if (!session->persistentClientId.isEmpty()) {
        session->canvas->setRemoteSceneTarget(session->persistentClientId, session->lastClientInfo.getMachineName());
    }

    // Set upload manager target to restore per-session upload state
    if (m_uploadManager) {
        m_uploadManager->setTargetClientId(session->persistentClientId);
        m_uploadManager->setActiveIdeaId(session->canvasSessionId);
    }
    updateUploadButtonForSession(*session);

    refreshOverlayActionsState(session->lastClientInfo.isOnline());
}

void MainWindow::updateUploadButtonForSession(CanvasSession& session) {
    m_uploadButton = session.uploadButton;
    m_uploadButtonInOverlay = session.uploadButtonInOverlay;
    if (session.uploadButtonDefaultFont != QFont()) {
        m_uploadButtonDefaultFont = session.uploadButtonDefaultFont;
    }
    if (m_uploadManager) {
        emit m_uploadManager->uiStateChanged();
    }
}

void MainWindow::unloadUploadsForSession(CanvasSession& session, bool attemptRemote) {
    if (!m_uploadManager) return;
    // Phase 3: Use persistentClientId for server communication
    const QString targetId = session.persistentClientId;
    if (targetId.isEmpty()) {
        session.remoteContentClearedOnDisconnect = true;
        return;
    }

    m_uploadManager->setTargetClientId(targetId);
    m_uploadManager->setActiveIdeaId(session.canvasSessionId);

    if (attemptRemote && m_webSocketClient && m_webSocketClient->isConnected()) {
        if (m_uploadManager->isUploading() || m_uploadManager->isFinalizing()) {
            m_uploadManager->requestCancel();
        } else if (m_uploadManager->hasActiveUpload()) {
            m_uploadManager->requestUnload();
        } else {
            m_uploadManager->requestRemoval(targetId);
        }
                m_uploadButton->setFont(m_uploadButtonDefaultFont);

        m_webSocketClient->sendRemoteSceneStop(targetId);
    }

    m_fileManager->unmarkAllForClient(targetId);

    if (session.canvas && session.canvas->scene()) {
        const QList<QGraphicsItem*> items = session.canvas->scene()->items();
        for (QGraphicsItem* item : items) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(item)) {
                media->setUploadNotUploaded();
            }
        }
    }

    session.remoteContentClearedOnDisconnect = true;

    if (m_uploadManager) {
        QPushButton* previousButton = m_uploadButton;
        bool previousOverlayFlag = m_uploadButtonInOverlay;
        QFont previousDefaultFont = m_uploadButtonDefaultFont;

        const bool hasSessionButton = (session.uploadButton != nullptr);
        const bool pointerAlreadySession = hasSessionButton && (m_uploadButton == session.uploadButton);

        if (hasSessionButton && !pointerAlreadySession) {
            m_uploadButton = session.uploadButton;
            m_uploadButtonInOverlay = session.uploadButtonInOverlay;
            if (session.uploadButtonDefaultFont != QFont()) {
                m_uploadButtonDefaultFont = session.uploadButtonDefaultFont;
            }
        }

        m_uploadManager->forceResetForClient(targetId);

        if (hasSessionButton && !pointerAlreadySession) {
            m_uploadButton = previousButton;
            m_uploadButtonInOverlay = previousOverlayFlag;
            m_uploadButtonDefaultFont = previousDefaultFont;
            if (m_uploadManager && previousButton) {
                emit m_uploadManager->uiStateChanged();
            }
        }
    }
}

QString MainWindow::createIdeaId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void MainWindow::rotateSessionIdea(CanvasSession& session) {
    const QString oldIdeaId = session.canvasSessionId;
    
    // PHASE 2: Notify server of canvas deletion before rotation (CRITICAL)
    if (m_webSocketClient && !session.persistentClientId.isEmpty()) {
        m_webSocketClient->sendCanvasDeleted(session.persistentClientId, oldIdeaId);
    }
    
    session.canvasSessionId = createIdeaId();
    session.expectedIdeaFileIds.clear();
    session.knownRemoteFileIds.clear();
    if (session.canvas) {
        session.canvas->setActiveIdeaId(session.canvasSessionId);
    }
    m_fileManager->removeIdeaAssociations(oldIdeaId);

    if (m_uploadManager) {
        if (m_activeSessionIdentity == session.persistentClientId) {
            m_uploadManager->setActiveIdeaId(session.canvasSessionId);
        }
    }
    
    // PHASE 2: Notify server of new canvas creation after rotation (CRITICAL)
    if (m_webSocketClient && !session.persistentClientId.isEmpty()) {
        m_webSocketClient->sendCanvasCreated(session.persistentClientId, session.canvasSessionId);
    }
}

void MainWindow::reconcileRemoteFilesForSession(CanvasSession& session, const QSet<QString>& currentFileIds) {
    session.expectedIdeaFileIds = currentFileIds;
    m_fileManager->replaceIdeaFileSet(session.canvasSessionId, currentFileIds);

    // Phase 3: Use persistentClientId for server communication
    if (!session.persistentClientId.isEmpty()) {
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            const QSet<QString> toRemove = session.knownRemoteFileIds - currentFileIds;
            for (const QString& fileId : toRemove) {
                m_webSocketClient->sendRemoveFile(session.persistentClientId, session.canvasSessionId, fileId);
                session.knownRemoteFileIds.remove(fileId);
            }
        }
        session.knownRemoteFileIds.intersect(currentFileIds);
    }
}

// [PHASE 7.4] Upload signal connections
void MainWindow::connectUploadSignals() {
    if (m_uploadSignalsConnected || !m_uploadManager) return;
    
    connect(m_uploadManager, &UploadManager::fileUploadStarted, this, [this](const QString& fileId) {
        if (CanvasSession* session = sessionForActiveUpload()) {
            if (!session->canvas || !session->canvas->scene()) return;
            session->upload.perFileProgress[fileId] = 0;
            const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
            for (ResizableMediaBase* item : items) {
                if (item && item->uploadState() != ResizableMediaBase::UploadState::Uploaded) {
                    item->setUploadUploading(0);
                }
            }
        }
    });
    
    connect(m_uploadManager, &UploadManager::fileUploadProgress, this, [this](const QString& fileId, int percent) {
        if (CanvasSession* session = sessionForActiveUpload()) {
            if (!session->canvas || !session->canvas->scene()) return;
            if (percent >= 100) {
                session->upload.perFileProgress[fileId] = 100;
                const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
                for (ResizableMediaBase* item : items) {
                    if (item) item->setUploadUploaded();
                }
                session->upload.serverCompletedFileIds.insert(fileId);
                return;
            }
            int clamped = std::clamp(percent, 0, 99);
            int previous = session->upload.perFileProgress.value(fileId, -1);
            if (previous >= 100 || clamped <= previous) return;
            session->upload.perFileProgress[fileId] = clamped;
            const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
            for (ResizableMediaBase* item : items) {
                if (item && item->uploadState() != ResizableMediaBase::UploadState::Uploaded) {
                    item->setUploadUploading(clamped);
                }
            }
        }
    });
    
    connect(m_webSocketClient, &WebSocketClient::uploadPerFileProgressReceived, this,
            [this](const QString& uploadId, const QHash<QString, int>& filePercents) {
        CanvasSession* session = sessionForUploadId(uploadId);
        if (!session || !session->canvas || !session->canvas->scene()) return;

        if (!session->upload.receivingFilesToastShown && !filePercents.isEmpty()) {
            const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                ? session->serverAssignedId
                : session->lastClientInfo.getDisplayText();
            TOAST_INFO(QString("Remote client %1 is receiving files...").arg(label));
            session->upload.receivingFilesToastShown = true;
        }

        for (auto it = filePercents.constBegin(); it != filePercents.constEnd(); ++it) {
            const QString& fid = it.key();
            const int p = std::clamp(it.value(), 0, 100);
            int previous = session->upload.perFileProgress.value(fid, -1);
            if (p <= previous && p < 100) continue;
            session->upload.perFileProgress[fid] = std::max(previous, p);
            const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fid);
            for (ResizableMediaBase* item : items) {
                if (!item) continue;
                if (p >= 100) item->setUploadUploaded();
                else item->setUploadUploading(p);
            }
            if (p >= 100) session->upload.serverCompletedFileIds.insert(fid);
        }
    });
    
    connect(m_uploadManager, &UploadManager::uploadFinished, this, [this]() {
        if (CanvasSession* session = sessionForActiveUpload()) {
            const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                ? session->serverAssignedId
                : session->lastClientInfo.getDisplayText();
            TOAST_SUCCESS(QString("Upload completed successfully to %1").arg(label));
            session->upload.remoteFilesPresent = true;
            session->knownRemoteFileIds.unite(session->expectedIdeaFileIds);
            clearUploadTracking(*session);
        } else {
            TOAST_SUCCESS("Upload completed successfully");
        }
    });
    
    connect(m_uploadManager, &UploadManager::uploadCompletedFileIds, this, [this](const QStringList& fileIds) {
        if (CanvasSession* session = sessionForActiveUpload()) {
            if (!session->canvas || !session->canvas->scene()) return;
            for (const QString& fileId : fileIds) {
                if (session->upload.serverCompletedFileIds.contains(fileId)) continue;
                const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
                for (ResizableMediaBase* item : items) {
                    if (item) item->setUploadUploaded();
                }
                session->upload.serverCompletedFileIds.insert(fileId);
            }
        }
    });
    
    connect(m_uploadManager, &UploadManager::allFilesRemoved, this, [this]() {
        CanvasSession* session = sessionForActiveUpload();
        if (!session) {
            const QString activeIdentity = m_uploadManager->activeSessionIdentity();
            if (!activeIdentity.isEmpty()) {
                session = findCanvasSession(activeIdentity);
            }
        }
        if (!session) {
            const QString lastClientId = m_uploadManager->lastRemovalClientId();
            if (!lastClientId.isEmpty()) {
                session = findCanvasSessionByServerClientId(lastClientId);
            }
        }

        if (session) {
            const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                ? session->serverAssignedId
                : session->lastClientInfo.getDisplayText();
            TOAST_INFO(QString("All files removed from %1").arg(label));

            session->upload.remoteFilesPresent = false;
            if (session->canvas && session->canvas->scene()) {
                const QList<QGraphicsItem*> allItems = session->canvas->scene()->items();
                for (QGraphicsItem* it : allItems) {
                    if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                        media->setUploadNotUploaded();
                    }
                }
            }
            clearUploadTracking(*session);
            m_uploadManager->clearLastRemovalClientId();
        } else {
            TOAST_INFO("All files removed from remote client");
            m_uploadManager->clearLastRemovalClientId();
        }
    });
    
    m_uploadSignalsConnected = true;
}

void MainWindow::setUploadSessionByUploadId(const QString& uploadId, const QString& sessionIdentity) {
    m_uploadSessionByUploadId.insert(uploadId, sessionIdentity);
}

// [PHASE 7.1] Simplified - delegates to WebSocketMessageHandler
void MainWindow::handleStateSyncFromServer(const QJsonObject& message) {
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->handleStateSyncMessage(message);
    }
}

void MainWindow::markAllSessionsOffline() {
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        session->lastClientInfo.setClientId(session->persistentClientId);
        session->lastClientInfo.setFromMemory(true);
        session->lastClientInfo.setOnline(false);
    }
}

QList<ClientInfo> MainWindow::buildDisplayClientList(const QList<ClientInfo>& connectedClients) {
    QList<ClientInfo> result;
    markAllSessionsOffline();
    QSet<QString> identitiesSeen;

    for (ClientInfo client : connectedClients) {
        QString persistentId = client.clientId();
        if (persistentId.isEmpty()) {
            qWarning() << "MainWindow::buildDisplayClientList: client has no persistentClientId";
            continue;
        }
        client.setClientId(persistentId);
        client.setOnline(true);

        if (CanvasSession* session = findCanvasSession(persistentId)) {
            session->serverAssignedId = client.getId(); // Keep for local lookup
            session->lastClientInfo = client;
            session->lastClientInfo.setClientId(persistentId);
            session->lastClientInfo.setFromMemory(true);
            session->lastClientInfo.setOnline(true);
            session->remoteContentClearedOnDisconnect = false;
            // Phase 3: Use persistentClientId for server communication
            if (session->canvas && !session->persistentClientId.isEmpty()) {
                session->canvas->setRemoteSceneTarget(session->persistentClientId, session->lastClientInfo.getMachineName());
            }
            client.setFromMemory(true);
            client.setId(session->serverAssignedId);
        } else {
            client.setFromMemory(false);
        }

        identitiesSeen.insert(persistentId);
        result.append(client);
    }

    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        if (identitiesSeen.contains(session->persistentClientId)) continue;
        ClientInfo info = session->lastClientInfo;
        info.setClientId(session->persistentClientId);
        if (!session->serverAssignedId.isEmpty()) {
            info.setId(session->serverAssignedId);
        }
        info.setOnline(false);
        info.setFromMemory(true);
        result.append(info);
    }

    return result;
}

ScreenCanvas* MainWindow::canvasForClientId(const QString& clientId) const {
    if (clientId.isEmpty()) {
        const CanvasSession* active = findCanvasSession(m_activeSessionIdentity);
        return active ? active->canvas : nullptr;
    }
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        if (session->serverAssignedId == clientId) {
            return session->canvas;
        }
    }
    return nullptr;
}


MainWindow::CanvasSession* MainWindow::sessionForActiveUpload() {
    if (!m_activeUploadSessionIdentity.isEmpty()) {
        if (CanvasSession* session = findCanvasSession(m_activeUploadSessionIdentity)) {
            return session;
        }
    }
    if (m_uploadManager) {
        const QString clientId = m_uploadManager->activeUploadTargetClientId();
        if (!clientId.isEmpty()) {
            if (CanvasSession* session = findCanvasSessionByServerClientId(clientId)) {
                return session;
            }
        }
    }
    return nullptr;
}

MainWindow::CanvasSession* MainWindow::sessionForUploadId(const QString& uploadId) {
    if (!uploadId.isEmpty()) {
        const QString identity = m_uploadSessionByUploadId.value(uploadId);
        if (!identity.isEmpty()) {
            if (CanvasSession* session = findCanvasSession(identity)) {
                return session;
            }
        }
    }
    return sessionForActiveUpload();
}

void MainWindow::clearUploadTracking(CanvasSession& session) {
    // Removed mediaIdsBeingUploaded.clear() - no longer exists
    // Removed mediaIdByFileId.clear() - no longer exists
    session.upload.itemsByFileId.clear();
    session.upload.currentUploadFileOrder.clear();
    session.upload.serverCompletedFileIds.clear();
    session.upload.perFileProgress.clear();
    session.upload.receivingFilesToastShown = false;
    if (!session.upload.activeUploadId.isEmpty()) {
        m_uploadSessionByUploadId.remove(session.upload.activeUploadId);
        session.upload.activeUploadId.clear();
    }
    if (m_activeUploadSessionIdentity == session.persistentClientId) {
        m_activeUploadSessionIdentity.clear();
    }
    if (m_uploadManager && m_uploadManager->activeSessionIdentity() == session.persistentClientId) {
        m_uploadManager->setActiveSessionIdentity(QString());
    }
}



void MainWindow::updateApplicationSuspendedState(bool suspended) {
    if (m_applicationSuspended == suspended) {
        return;
    }
    m_applicationSuspended = suspended;
    ScreenCanvas::setAllCanvasesSuspended(suspended);
}


void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        const bool minimized = (windowState() & Qt::WindowMinimized);
        updateApplicationSuspendedState(minimized || isHidden());
    }
}

void MainWindow::handleApplicationStateChanged(Qt::ApplicationState state) {
    const bool suspended = (state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended);
    updateApplicationSuspendedState(suspended);
}

void MainWindow::showScreenView(const ClientInfo& client) {
    if (!m_navigationManager) return;
    CanvasSession& session = ensureCanvasSession(client);
    const bool sessionHasActiveScreens = session.canvas && session.canvas->hasActiveScreens();
    const bool sessionHasStoredScreens = !session.lastClientInfo.getScreens().isEmpty();
    const bool hasCachedContent = sessionHasActiveScreens || sessionHasStoredScreens;
    switchToCanvasSession(session.persistentClientId);
    m_activeRemoteClientId = session.serverAssignedId;
    m_remoteClientConnected = false;
    m_selectedClient = session.lastClientInfo;
    ClientInfo effectiveClient = session.lastClientInfo;
    const bool alreadyOnScreenView = m_navigationManager->isOnScreenView();
    const QString currentId = alreadyOnScreenView ? m_navigationManager->currentClientId() : QString();
    const bool alreadyOnThisClient = alreadyOnScreenView && currentId == effectiveClient.getId() && !effectiveClient.getId().isEmpty();

    if (hasCachedContent) {
        m_canvasContentEverLoaded = true;
    }

    if (!alreadyOnThisClient) {
        // New client selection: reset reveal flag so first incoming screens will fade in once
        m_canvasRevealedForCurrentClient = false;
        m_navigationManager->showScreenView(effectiveClient, hasCachedContent);
        if (session.canvas) {
            session.canvas->resetTransform();
            session.canvas->requestDeferredInitialRecenter(53);
            session.canvas->recenterWithMargin(53);
        }
    } else {
        // Same client: refresh subscriptions without resetting UI state
        m_navigationManager->refreshActiveClientPreservingCanvas(effectiveClient);
        if (hasCachedContent && effectiveClient.isOnline()) {
            m_navigationManager->enterLoadingStateImmediate();
        }
    }

    // Hide top-bar page title and show back button on screen view
    if (m_pageTitleLabel) m_pageTitleLabel->hide();
    if (m_backButton) m_backButton->show();

    // Update upload target
    m_uploadManager->setTargetClientId(session.serverAssignedId);

    // Show remote client info wrapper when viewing a client
    if (m_remoteClientInfoWrapper) {
        m_remoteClientInfoWrapper->setVisible(true);
    }
    // [PHASE 5] Use manager accessor
    QWidget* container = m_remoteClientInfoManager ? m_remoteClientInfoManager->getContainer() : nullptr;
    if (container) {
        container->setVisible(true);
    }
    if (m_inlineSpinner) {
        m_inlineSpinner->hide();
    }
    updateClientNameDisplay(effectiveClient);
    // While refreshing, start from a clean layout then reapply cached state if available
    removeVolumeIndicatorFromLayout();
    removeRemoteStatusFromLayout();

    addRemoteStatusToLayout();
    if (effectiveClient.isOnline()) {
        setRemoteConnectionStatus("CONNECTING...", /*propagateLoss*/ false);
    } else {
        setRemoteConnectionStatus("DISCONNECTED");
    }

    if (hasCachedContent) {
        if (session.canvas) {
            session.canvas->showContentAfterReconnect();
            session.canvas->resetTransform();
            session.canvas->recenterWithMargin(53);
            session.canvas->requestDeferredInitialRecenter(53);
        }
        m_canvasRevealedForCurrentClient = true;
        m_canvasContentEverLoaded = true;

        if (session.lastClientInfo.getVolumePercent() >= 0) {
            addVolumeIndicatorToLayout();
            updateVolumeIndicator();
        }

    }
    
    // Update button visibility for screen view page
    if (m_responsiveLayoutManager) {
        m_responsiveLayoutManager->updateResponsiveButtonVisibility();
    }
}

// [PHASE 5] Delegate to RemoteClientInfoManager
void MainWindow::updateClientNameDisplay(const ClientInfo& client) {
    if (m_remoteClientInfoManager) {
        m_remoteClientInfoManager->updateClientNameDisplay(client);
    }
}

void MainWindow::showClientListView() {
    // Do NOT unload when navigating back to client list - uploads persist per session
    // Each client maintains its own upload state that should survive navigation
    
    if (m_navigationManager) m_navigationManager->showClientList();
    if (m_uploadButton) m_uploadButton->setText("Upload to Client");
    m_uploadManager->setTargetClientId(QString());
    // Clear remote connection status when leaving screen view
    setRemoteConnectionStatus("DISCONNECTED", /*propagateLoss*/ false);
    m_activeRemoteClientId.clear();
    m_remoteClientConnected = false;
    
    // Hide remote client info wrapper when on client list
    if (m_remoteClientInfoWrapper) {
        m_remoteClientInfoWrapper->setVisible(false);
    }
    // [PHASE 5] Use manager accessor
    QWidget* container = m_remoteClientInfoManager ? m_remoteClientInfoManager->getContainer() : nullptr;
    if (container) {
        container->setVisible(false);
    }
    if (m_inlineSpinner) {
        m_inlineSpinner->stop();
        m_inlineSpinner->hide();
    }
    // Ensure volume indicator is removed when leaving screen view
    removeVolumeIndicatorFromLayout();
    // Show top-bar page title and hide back button on client list
    if (m_pageTitleLabel) m_pageTitleLabel->show();
    if (m_backButton) m_backButton->hide();
    
    // Update button visibility for client list page
    if (m_responsiveLayoutManager) {
        m_responsiveLayoutManager->updateResponsiveButtonVisibility();
    }
}

// Removed legacy createScreenWidget(): ScreenCanvas draws screens directly now

// [PHASE 1.2] Delegate to CanvasViewPage
// [PHASE 5] Delegate to both CanvasViewPage and RemoteClientInfoManager
void MainWindow::updateVolumeIndicator() {
    int vol = -1;
    if (!m_selectedClient.getId().isEmpty()) {
        vol = m_selectedClient.getVolumePercent();
    }
    if (m_canvasViewPage) {
        m_canvasViewPage->updateVolumeIndicator(vol);
    }
    if (m_remoteClientInfoManager) {
        m_remoteClientInfoManager->updateVolumeIndicator(vol);
    }
}

void MainWindow::onUploadButtonClicked() {
    // [PHASE 7.4] Delegate to UploadEventHandler
    if (m_uploadEventHandler) {
        m_uploadEventHandler->onUploadButtonClicked();
    }
}


void MainWindow::onBackToClientListClicked() { showClientListView(); }


// Phase 1.1: New slot connected to ClientListPage::clientClicked signal
void MainWindow::onClientSelected(const ClientInfo& client, int clientIndex) {
    Q_UNUSED(clientIndex);
    CanvasSession& session = ensureCanvasSession(client);
    switchToCanvasSession(session.persistentClientId);
    m_selectedClient = session.lastClientInfo;
    showScreenView(session.lastClientInfo);
    // ScreenNavigationManager will request screens; no need to duplicate here
}

// Phase 1.1: New slot connected to ClientListPage::ongoingSceneClicked signal
void MainWindow::onOngoingSceneSelected(const QString& persistentClientId) {
    CanvasSession* session = findCanvasSession(persistentClientId);
    if (!session) return;

    switchToCanvasSession(session->persistentClientId);
    m_selectedClient = session->lastClientInfo;
    showScreenView(session->lastClientInfo);
}

// Note: generic message hook removed; we handle specific message types via dedicated slots


MainWindow::~MainWindow() {
    // Cleanly disconnect
    if (m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
    }
    // Persist current settings on shutdown (safety in case dialog not used)
    QSettings settings("Mouffette", "Client");
    settings.setValue("serverUrl", m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig);
    settings.setValue("autoUploadImportedMedia", m_autoUploadImportedMedia);
}

void MainWindow::updateStylesheetsForTheme() {
    // Re-apply stylesheets that use ColorSource to pick up theme changes
    if (m_centralWidget) {
        m_centralWidget->setStyleSheet(QString(
            "QWidget#CentralRoot { background-color: %1; }"
        ).arg(AppColors::colorSourceToCss(AppColors::gWindowBackgroundColorSource)));
    }
    // Note: Client list styling now handled by ClientListPage
    
    // Ensure the client list page title uses the same text color as other texts
    if (m_pageTitleLabel) {
        m_pageTitleLabel->setStyleSheet(QString(
            "QLabel { "
            "    background: transparent; "
            "    border: none; "
            "    font-size: %1px; "
            "    font-weight: bold; "
            "    color: palette(text); "
            "}").arg(gTitleTextFontSize)
        );
    }

    // Phase 1.2: Update canvas container via CanvasViewPage
    if (m_canvasViewPage) {
        QWidget* canvasContainer = m_canvasViewPage->getCanvasContainer();
        if (canvasContainer) {
            canvasContainer->setStyleSheet(
                QString("QWidget#CanvasContainer { "
                "   background-color: %2; "
                "   border: 1px solid %1; "
                "   border-radius: 5px; "
                "}").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)).arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
            );
        }
    }
    
    // [PHASE 5] Update remote client info container border and separators via manager
    QWidget* remoteContainer = m_remoteClientInfoManager ? m_remoteClientInfoManager->getContainer() : nullptr;
    if (remoteContainer) {
        const QString containerStyle = QString(
            "QWidget { "
            "    background-color: transparent; "
            "    color: palette(button-text); "
            "    border: 1px solid %3; "
            "    border-radius: %1px; "
            "    min-height: %2px; "
            "    max-height: %2px; "
            "}"
        ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource));
        remoteContainer->setStyleSheet(containerStyle);
        
        // Update separators in remote client info
        QList<QFrame*> separators = remoteContainer->findChildren<QFrame*>();
        for (QFrame* separator : separators) {
            if (separator && separator->frameShape() == QFrame::VLine) {
                separator->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
            }
        }
    }
    
    // [PHASE 6.1] Update local client info container border via TopBarManager
    QWidget* localContainer = m_topBarManager ? m_topBarManager->getLocalClientInfoContainer() : nullptr;
    if (localContainer) {
        const QString containerStyle = QString(
            "QWidget { "
            "    background-color: transparent; "
            "    color: palette(button-text); "
            "    border: 1px solid %3; "
            "    border-radius: %1px; "
            "    min-height: %2px; "
            "}"
        ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource));
        localContainer->setStyleSheet(containerStyle);
    }
    
    // Update separators in local client info
    QList<QFrame*> localSeparators = localContainer ? localContainer->findChildren<QFrame*>() : QList<QFrame*>();
    for (QFrame* separator : localSeparators) {
        if (separator && separator->frameShape() == QFrame::VLine) {
            separator->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
        }
    }
    
    // Update all buttons that use gAppBorderColorSource
    QList<QPushButton*> buttons = findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button && button->styleSheet().contains("border:") && 
            !button->styleSheet().contains("border: none") &&
            !button->styleSheet().contains("background: transparent")) {
            // Re-apply button styles - check if it's a primary or normal button
            QString currentStyle = button->styleSheet();
            if (currentStyle.contains(AppColors::gBrandBlue.name())) {
                applyPrimaryBtn(button);
            } else if (currentStyle.contains("QPushButton")) {
                applyPillBtn(button);
            }
        }
    }
    // Note: Client list style updates now handled by ClientListPage
}

void MainWindow::setupUI() {
    // Use a standard central widget (native window frame provides chrome)
    m_centralWidget = new QWidget(this);
    m_centralWidget->setObjectName("CentralRoot");
    setCentralWidget(m_centralWidget);

    
    m_mainLayout = new QVBoxLayout(m_centralWidget);
    // Use explicit spacer to control gap so it's not affected by any nested margins
    m_mainLayout->setSpacing(0);
    // Apply global window content margins; no extra top inset needed with native title bar
    int topMargin = gWindowContentMarginTop;
    m_mainLayout->setContentsMargins(gWindowContentMarginLeft, topMargin, gWindowContentMarginRight, gWindowContentMarginBottom);

    // Match central background to app window background so macOS title bar (transparent) blends in
    m_centralWidget->setStyleSheet(QString(
        "QWidget#CentralRoot { background-color: %1; }"
    ).arg(AppColors::colorSourceToCss(AppColors::gWindowBackgroundColorSource)));
    
    // Top section with margins
    QWidget* topSection = new QWidget();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    // Disable context menus on top section as well
    topSection->setContextMenuPolicy(Qt::NoContextMenu);
#endif
    QVBoxLayout* topLayout = new QVBoxLayout(topSection);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    // Remove all margins around the top section
    topLayout->setContentsMargins(0, 0, 0, 0);
#else
    // Remove all margins on non-mac/win as well
    topLayout->setContentsMargins(0, 0, 0, 0);
#endif
    // No internal vertical spacing inside the top section; vertical gap is controlled by gInnerContentGap
    topLayout->setSpacing(0);
    
    // Connection section (always visible)
    m_connectionBar = new QWidget();
    m_connectionBar->setObjectName("ConnectionBar");
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    m_connectionBar->setContextMenuPolicy(Qt::NoContextMenu);
#endif
    m_connectionLayout = new QHBoxLayout(m_connectionBar);
    m_connectionLayout->setContentsMargins(0, 0, 0, 0);
    // Standard spacing between items in the connection bar
    m_connectionLayout->setSpacing(8);
    
    // Contextual page title
    m_pageTitleLabel = new QLabel("Connected Clients");
    applyTitleText(m_pageTitleLabel);
    // Match hostname styling: same font size, weight, and color
    m_pageTitleLabel->setStyleSheet(QString(
        "QLabel { "
        "    background: transparent; "
        "    border: none; "
        "    font-size: %1px; "
        "    font-weight: bold; "
        "    color: palette(text); "
        "}").arg(gTitleTextFontSize)
    );
    m_connectionLayout->addWidget(m_pageTitleLabel);

    // Note: applyPillBtn and applyPrimaryBtn are now defined globally at the top of the file
    // using gDynamicBox configuration for consistent sizing

    // Back button (left-aligned, initially hidden)
    m_backButton = new QPushButton("← Go Back");
    applyPillBtn(m_backButton);
    // Ensure button is sized properly for its text content
    m_backButton->adjustSize();
    int textWidth = m_backButton->fontMetrics().horizontalAdvance(m_backButton->text()) + 24; // text + padding
    int buttonWidth = qMax(10, textWidth);
    m_backButton->setFixedWidth(buttonWidth); // Use fixed width to prevent any changes
    // Override size policy to prevent shrinking when window reduces
    m_backButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_backButton->hide(); // Initially hidden, shown only on screen view
    connect(m_backButton, &QPushButton::clicked, this, &MainWindow::onBackToClientListClicked);
    
    // Create local client info container ("You" + network status)
    createLocalClientInfoContainer();
    // Initialize with disconnected status
    setLocalNetworkStatus("DISCONNECTED");

    // Enable/Disable toggle button with fixed width (left of Settings)
    m_connectToggleButton = new QPushButton("Disable");
    applyPillBtn(m_connectToggleButton);
    // Ensure button is sized properly for "Disable"/"Enable" text (using longer text)
    int toggleTextWidth = m_connectToggleButton->fontMetrics().horizontalAdvance("Disable") + 24;
    int toggleButtonWidth = qMax(80, toggleTextWidth);
    m_connectToggleButton->setFixedWidth(toggleButtonWidth); // Use fixed width to prevent any changes
    connect(m_connectToggleButton, &QPushButton::clicked, this, &MainWindow::onEnableDisableClicked);

    // Settings button
    m_settingsButton = new QPushButton("Settings");
    applyPillBtn(m_settingsButton);
    // Ensure button is sized properly for its text content
    int settingsTextWidth = m_settingsButton->fontMetrics().horizontalAdvance("Settings") + 24;
    int settingsButtonWidth = qMax(80, settingsTextWidth);
    m_settingsButton->setFixedWidth(settingsButtonWidth); // Use fixed width to prevent any changes
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::showSettingsDialog);

    // [PHASE 6.1] Get local client info container from TopBarManager
    QWidget* localClientContainer = m_topBarManager ? m_topBarManager->getLocalClientInfoContainer() : nullptr;
    
    // Layout: [title][back][stretch][local-client-info][connect][settings]
    m_connectionLayout->addWidget(m_backButton);
    m_connectionLayout->addStretch();
    if (localClientContainer) {
        m_connectionLayout->addWidget(localClientContainer);
    }
    m_connectionLayout->addWidget(m_connectToggleButton);
    m_connectionLayout->addWidget(m_settingsButton);

    topLayout->addWidget(m_connectionBar);
    m_mainLayout->addWidget(topSection);
    // Explicit inner gap between top container and hostname container
    m_mainLayout->addSpacing(gInnerContentGap);
    
    // Bottom section with margins (no separator line)
    QWidget* bottomSection = new QWidget();
    QVBoxLayout* bottomLayout = new QVBoxLayout(bottomSection);
    // Remove all margins for the bottom section; outer spacing is controlled by gInnerContentGap spacer
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(0);
    
    // Create stacked widget for page navigation
    m_stackedWidget = new QStackedWidget();
    // Ensure the stacked widget adds no extra padding; vertical gap is controlled by gInnerContentGap
    m_stackedWidget->setContentsMargins(0, 0, 0, 0);
    // Block stray key events (like space) at the stack level
    m_stackedWidget->installEventFilter(this);
    bottomLayout->addWidget(m_stackedWidget);
    m_mainLayout->addWidget(bottomSection);
    
    // Phase 1.1: Create ClientListPage
    m_clientListPage = new ClientListPage(m_sessionManager, this);
    connect(m_clientListPage, &ClientListPage::clientClicked, this, &MainWindow::onClientSelected);
    connect(m_clientListPage, &ClientListPage::ongoingSceneClicked, this, &MainWindow::onOngoingSceneSelected);
    m_stackedWidget->addWidget(m_clientListPage);
    
    // Show placeholder immediately (before any connection) so page isn't empty during CONNECTING state
    m_clientListPage->ensureClientListPlaceholder();
    m_clientListPage->ensureOngoingScenesPlaceholder();
    
    // Phase 1.2: Create CanvasViewPage
    m_canvasViewPage = new CanvasViewPage(this);
    m_stackedWidget->addWidget(m_canvasViewPage);
    
    // Initialize remote client info in top bar (must be done after CanvasViewPage exists)
    initializeRemoteClientInfoInTopBar();
    
    // Start with client list page
    m_stackedWidget->setCurrentWidget(m_clientListPage);

    // Initialize navigation manager (after widgets exist)
    m_navigationManager = new ScreenNavigationManager(this);
    
    // Initialize responsive layout manager (after UI is created)
    if (m_responsiveLayoutManager) {
        m_responsiveLayoutManager->initialize();
    }
    {
        ScreenNavigationManager::Widgets w;
        w.stack = m_stackedWidget;
        w.clientListPage = m_clientListPage;
        w.screenViewPage = m_canvasViewPage;  // Phase 1.2: Use CanvasViewPage
        w.backButton = m_canvasViewPage ? m_canvasViewPage->getBackButton() : nullptr;
        w.canvasStack = m_canvasViewPage ? m_canvasViewPage->getCanvasStack() : nullptr;
        w.loadingSpinner = m_canvasViewPage ? m_canvasViewPage->getLoadingSpinner() : nullptr;
        w.spinnerOpacity = m_canvasViewPage ? m_canvasViewPage->getSpinnerOpacity() : nullptr;
        w.spinnerFade = m_canvasViewPage ? m_canvasViewPage->getSpinnerFade() : nullptr;
        w.canvasOpacity = m_canvasViewPage ? m_canvasViewPage->getCanvasOpacity() : nullptr;
        w.canvasFade = m_canvasViewPage ? m_canvasViewPage->getCanvasFade() : nullptr;
        w.inlineSpinner = m_inlineSpinner;
        w.canvasContentEverLoaded = &m_canvasContentEverLoaded;
        w.volumeOpacity = m_canvasViewPage ? m_canvasViewPage->getVolumeOpacity() : nullptr;
        w.volumeFade = m_canvasViewPage ? m_canvasViewPage->getVolumeFade() : nullptr;
        w.screenCanvas = nullptr;
        m_navigationManager->setWidgets(w);
        m_navigationManager->setDurations(m_loaderDelayMs, m_loaderFadeDurationMs, m_fadeDurationMs);
        connect(m_navigationManager, &ScreenNavigationManager::requestScreens, this, [this](const QString& id){
            if (m_webSocketClient && m_webSocketClient->isConnected()) m_webSocketClient->requestScreens(id);
        });
        connect(m_navigationManager, &ScreenNavigationManager::watchTargetRequested, this, [this](const QString& id){
            if (m_watchManager && m_webSocketClient && m_webSocketClient->isConnected()) m_watchManager->toggleWatch(id);
        });
    connect(m_navigationManager, &ScreenNavigationManager::clientListEntered, this, [this](){
            // Do NOT unload uploads when navigating back to client list
            // Uploads should persist per session and only be cleared on disconnect or explicit unload
            if (m_watchManager) m_watchManager->unwatchIfAny();
            if (m_screenCanvas) m_screenCanvas->hideRemoteCursor();
        });
    }

    // Receive remote cursor updates when watching
    connect(m_webSocketClient, &WebSocketClient::cursorPositionReceived, this,
            [this](const QString& targetId, int x, int y) {
                if (!m_screenCanvas) return;
                if (m_stackedWidget->currentWidget() != m_canvasViewPage) return;  // Phase 1.2
                bool matchWatch = (m_watchManager && targetId == m_watchManager->watchedClientId());
                bool matchSelected = (!m_selectedClient.getId().isEmpty() && targetId == m_selectedClient.getId());
                if (matchWatch || matchSelected) {
                    m_screenCanvas->updateRemoteCursor(x, y);
                }
            });

    // Initialize responsive layout
    QTimer::singleShot(0, this, [this]() {
        if (m_responsiveLayoutManager) {
            m_responsiveLayoutManager->updateResponsiveLayout();
        }
    });

#ifdef Q_OS_MACOS
    // Set native macOS window level for true always-on-top behavior across Spaces
    QTimer::singleShot(100, this, [this]() {
        MacWindowManager::setWindowAlwaysOnTop(this);
    });
#endif
}

// [PHASE 1.2] Canvas view page creation moved to CanvasViewPage class (~183 lines removed)

// [PHASE 6.3] Delegate to MenuBarManager
void MainWindow::setupMenuBar() {
    if (m_menuBarManager) {
        m_menuBarManager->setup();
    }
}

// [PHASE 6.3] Handle quit request from menu
void MainWindow::onMenuQuitRequested() {
    if (m_webSocketClient && m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
    }
    QApplication::quit();
}

// [PHASE 6.3] Handle about request from menu
void MainWindow::onMenuAboutRequested() {
    qDebug() << "About dialog suppressed (no popup mode).";
}

// [PHASE 6.2] Delegate to SystemTrayManager
void MainWindow::setupSystemTray() {
    if (m_systemTrayManager) {
        m_systemTrayManager->setup();
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // Interpret window close as: stop watching (to stop remote stream) but keep app running in background.
    if (m_watchManager) m_watchManager->unwatchIfAny();
    hide();
    event->ignore(); // do not quit app
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // If user reopens the window and we are on the canvas view with a selected client but not watching anymore, restart watch.
    if (m_navigationManager && m_navigationManager->isOnScreenView() && m_watchManager) {
        const QString selId = m_selectedClient.getId();
        if (!selId.isEmpty() && !m_watchManager->isWatching() && m_webSocketClient && m_webSocketClient->isConnected()) {
            qDebug() << "Reopening window: auto-resuming watch on" << selId;
            m_watchManager->toggleWatch(selId); // since not watching, this starts watch
            // Also request screens to ensure fresh snapshot if server paused sending after unwatch
            m_webSocketClient->requestScreens(selId);
        }
    }
    updateApplicationSuspendedState(windowState() & Qt::WindowMinimized);
}

void MainWindow::hideEvent(QHideEvent* event) {
    QMainWindow::hideEvent(event);
    updateApplicationSuspendedState(true);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    
    // Update responsive layout based on window width
    if (m_responsiveLayoutManager) {
        m_responsiveLayoutManager->updateResponsiveLayout();
    }
    
    // If we're currently showing the screen view and have a canvas with content,
    // recenter the view to maintain good visibility only before first reveal or when no screens are present
    if (m_stackedWidget && m_stackedWidget->currentWidget() == m_canvasViewPage &&  // Phase 1.2
        m_screenCanvas) {
        const bool hasScreens = !m_selectedClient.getScreens().isEmpty();
        if (!m_canvasRevealedForCurrentClient && hasScreens) {
            m_screenCanvas->recenterWithMargin(53);
        }
    }
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    // Show/hide window on any click (left, right, or double-click)
    switch (reason) {
    case QSystemTrayIcon::Trigger:        // Single left-click
    case QSystemTrayIcon::DoubleClick:    // Double left-click  
    case QSystemTrayIcon::Context:        // Right-click
        {
            const bool minimized = (windowState() & Qt::WindowMinimized);
            const bool hidden = isHidden() || !isVisible();
            if (minimized || hidden) {
                // Reveal and focus the window if minimized or hidden
                if (minimized) {
                    setWindowState(windowState() & ~Qt::WindowMinimized);
                    showNormal();
                }
                show();
                raise();
                activateWindow();
            } else {
                // Fully visible: toggle to hide to tray
                hide();
            }
        }
        break;
    default:
        break;
    }
}

void MainWindow::onEnableDisableClicked() {
    if (!m_webSocketClient) return;
    
    if (m_connectToggleButton->text() == "Disable") {
        // Disable client: disconnect and prevent auto-reconnect
        m_userDisconnected = true;
        m_reconnectTimer->stop(); // Stop any pending reconnection
        if (m_webSocketClient->isConnected()) {
            m_webSocketClient->disconnect();
        }
        m_connectToggleButton->setText("Enable");
    } else {
        // Enable client: allow connections and start connecting
        m_userDisconnected = false;
        m_reconnectAttempts = 0; // Reset reconnection attempts
        connectToServer();
        m_connectToggleButton->setText("Disable");
    }
}

// Settings dialog: server URL with Save/Cancel
void MainWindow::showSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("Settings");
    QVBoxLayout* v = new QVBoxLayout(&dialog);
    QLabel* urlLabel = new QLabel("Server URL");
    QLineEdit* urlEdit = new QLineEdit(&dialog);
    if (m_serverUrlConfig.isEmpty()) m_serverUrlConfig = DEFAULT_SERVER_URL;
    urlEdit->setText(m_serverUrlConfig);
    v->addWidget(urlLabel);
    v->addWidget(urlEdit);

    // New: Auto-upload imported media checkbox
    QCheckBox* autoUploadChk = new QCheckBox("Upload imported media automatically", &dialog);
    autoUploadChk->setChecked(m_autoUploadImportedMedia);
    v->addSpacing(8);
    v->addWidget(autoUploadChk);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton* cancelBtn = new QPushButton("Cancel");
    QPushButton* saveBtn = new QPushButton("Save");
    applyPillBtn(cancelBtn);
    applyPrimaryBtn(saveBtn);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    v->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, [this, urlEdit, autoUploadChk, &dialog]() {
        const QString newUrl = urlEdit->text().trimmed();
        if (!newUrl.isEmpty()) {
            bool changed = (newUrl != (m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig));
            m_serverUrlConfig = newUrl;
            if (changed) {
                // Restart connection to apply new server URL
                if (m_webSocketClient->isConnected()) {
                    m_userDisconnected = false; // this is not a manual disconnect, we want reconnect
                    m_webSocketClient->disconnect();
                }
                connectToServer();
            }
        }
        m_autoUploadImportedMedia = autoUploadChk->isChecked();

        // Persist immediately
        {
            QSettings settings("Mouffette", "Client");
            settings.setValue("serverUrl", m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig);
            settings.setValue("autoUploadImportedMedia", m_autoUploadImportedMedia);
            settings.sync();
        }
        dialog.accept();
    });

    dialog.exec();
}

// (Removed stray duplicated code block previously injected)

void MainWindow::scheduleReconnect() {
    if (m_userDisconnected) {
        return; // Don't reconnect if user disabled the client
    }
    
    // Exponential backoff: 2^attempts seconds, capped at maxReconnectDelay
    int delay = qMin(static_cast<int>(qPow(2, m_reconnectAttempts)) * 1000, m_maxReconnectDelay);
    
    // Add some jitter to avoid thundering herd (±25%)
    int jitter = QRandomGenerator::global()->bounded(-delay/4, delay/4);
    delay += jitter;
    
    qDebug() << "Scheduling reconnect attempt" << (m_reconnectAttempts + 1) << "in" << delay << "ms";
    
    m_reconnectTimer->start(delay);
    m_reconnectAttempts++;
}

void MainWindow::attemptReconnect() {
    if (m_userDisconnected) {
        return; // Don't reconnect if user disabled the client
    }
    
    qDebug() << "Attempting reconnection...";
    connectToServer();
}

// [PHASE 7.1] Helper methods for WebSocketMessageHandler
void MainWindow::resetReconnectState() {
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
}

void MainWindow::resetAllSessionUploadStates() {
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        if (session->canvas && session->canvas->scene()) {
            const QList<QGraphicsItem*> allItems = session->canvas->scene()->items();
            for (QGraphicsItem* item : allItems) {
                if (auto* media = dynamic_cast<ResizableMediaBase*>(item)) {
                    if (media->uploadState() == ResizableMediaBase::UploadState::Uploading) {
                        media->setUploadNotUploaded();
                    }
                }
            }
        }
        session->upload.remoteFilesPresent = false;
        clearUploadTracking(*session);
    }
    m_uploadSessionByUploadId.clear();
    m_activeUploadSessionIdentity.clear();
}

void MainWindow::syncCanvasSessionFromServer(const QString& canvasSessionId, const QSet<QString>& fileIds) {
    // Find session with this canvasSessionId and mark files as uploaded
    CanvasSession* session = findCanvasSessionByIdeaId(canvasSessionId);
    if (session && !session->persistentClientId.isEmpty()) {
        session->knownRemoteFileIds = fileIds;
        
        // Mark files as uploaded in FileManager
        for (const QString& fileId : fileIds) {
            m_fileManager->markFileUploadedToClient(fileId, session->persistentClientId);
        }
        
        qDebug() << "MainWindow: Restored upload state for session" << session->persistentClientId
                 << "idea" << canvasSessionId;
        
        // Refresh UI if this is the active session
        if (m_activeSessionIdentity == session->persistentClientId && m_uploadManager) {
            emit m_uploadManager->uiStateChanged();
        }
    } else {
        qDebug() << "MainWindow: No matching session found for idea" << canvasSessionId;
    }
}

// [PHASE 7.1] Simplified - delegates to WebSocketMessageHandler
void MainWindow::onConnected() {
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->onConnected();
    }
}

// [PHASE 7.1] Simplified - delegates to WebSocketMessageHandler  
void MainWindow::onDisconnected() {
    if (m_webSocketMessageHandler) {
        m_webSocketMessageHandler->onDisconnected();
    }
}

// start Watching/stopWatchingCurrentClient removed (handled by WatchManager)

void MainWindow::onConnectionError(const QString& error) {
    qWarning() << "Failed to connect to server:" << error << "(silent mode, aucune popup)";
    setUIEnabled(false);
    setLocalNetworkStatus("Error");
    TOAST_ERROR(QString("Connection failed: %1").arg(error), 4000);
}

void MainWindow::onClientListReceived(const QList<ClientInfo>& clients) {
    // [PHASE 7.3] Delegate to ClientListEventHandler
    if (m_clientListEventHandler) {
        m_clientListEventHandler->onClientListReceived(clients);
    }
}

void MainWindow::onRegistrationConfirmed(const ClientInfo& clientInfo) {
    m_thisClient = clientInfo;
    qDebug() << "Registration confirmed for:" << clientInfo.getMachineName();
}

void MainWindow::syncRegistration() {
    // [PHASE 7.2] Delegate to ScreenEventHandler
    if (m_screenEventHandler) {
        m_screenEventHandler->syncRegistration();
    }
}

void MainWindow::onScreensInfoReceived(const ClientInfo& clientInfo) {
    // [PHASE 7.2] Delegate to ScreenEventHandler
    if (m_screenEventHandler) {
        m_screenEventHandler->onScreensInfoReceived(clientInfo);
    }
}

void MainWindow::onWatchStatusChanged(bool watched) {
    // Store watched state locally (as this client being watched by someone else)
    // We don't need a member; we can gate sending by this flag at runtime.
    // For simplicity, keep a static so our timers can read it.
    m_isWatched = watched;
    
    // Start/stop display sync timer based on watch status to prevent unnecessary canvas reloads
    if (watched) {
        // Immediately push a fresh snapshot so watchers don't wait for the first 3s tick
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            syncRegistration();
        }
        if (!m_displaySyncTimer->isActive()) m_displaySyncTimer->start();
    } else {
        if (m_displaySyncTimer->isActive()) m_displaySyncTimer->stop();
    }
    
    qDebug() << "Watch status changed:" << (watched ? "watched" : "not watched");

    // Begin/stop sending our cursor position to watchers (target side)
    if (watched) {
        if (!m_cursorTimer) {
            m_cursorTimer = new QTimer(this);
            m_cursorTimer->setInterval(m_cursorUpdateIntervalMs); // configurable
            connect(m_cursorTimer, &QTimer::timeout, this, [this]() {
                static int lastX = INT_MIN, lastY = INT_MIN;
#ifdef Q_OS_WIN
                POINT pt{0,0};
                const BOOL ok = GetCursorPos(&pt);
                const int gx = ok ? static_cast<int>(pt.x) : QCursor::pos().x();
                const int gy = ok ? static_cast<int>(pt.y) : QCursor::pos().y();
                if (gx != lastX || gy != lastY) {
                    lastX = gx; lastY = gy;
                    if (m_webSocketClient && m_webSocketClient->isConnected() && m_isWatched) {
                        m_webSocketClient->sendCursorUpdate(gx, gy);
                    }
                }
#else
                const QPoint p = QCursor::pos();
                if (p.x() != lastX || p.y() != lastY) {
                    lastX = p.x();
                    lastY = p.y();
                    if (m_webSocketClient && m_webSocketClient->isConnected() && m_isWatched) {
                        m_webSocketClient->sendCursorUpdate(p.x(), p.y());
                    }
                }
#endif
            });
        }
        // Apply any updated interval before starting
        m_cursorTimer->setInterval(m_cursorUpdateIntervalMs);
        if (!m_cursorTimer->isActive()) m_cursorTimer->start();
    } else {
        if (m_cursorTimer) m_cursorTimer->stop();
    }
}

void MainWindow::onDataRequestReceived() {
    // [PHASE 7.2] Delegate to ScreenEventHandler
    if (m_screenEventHandler) {
        m_screenEventHandler->onDataRequestReceived();
    }
}

void MainWindow::onRemoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName) {
    Q_UNUSED(active);
    Q_UNUSED(targetClientId);
    Q_UNUSED(targetMachineName);
    // Phase 1.1: Refresh ongoing scenes via ClientListPage
    if (m_clientListPage) {
        m_clientListPage->refreshOngoingScenesList();
    }
}

// [PHASE 3] Delegate to SystemMonitor
QList<ScreenInfo> MainWindow::getLocalScreenInfo() {
    return m_systemMonitor ? m_systemMonitor->getLocalScreenInfo() : QList<ScreenInfo>();
}


void MainWindow::connectToServer() {
    if (!m_webSocketClient) return;
    const QString url = m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig;
    qDebug() << "Connecting to server:" << url;
    m_webSocketClient->connectToServer(url);
}

// [PHASE 3] Delegate to SystemMonitor
QString MainWindow::getMachineName() {
    return m_systemMonitor ? m_systemMonitor->getMachineName() : "Unknown Machine";
}

// [PHASE 3] Delegate to SystemMonitor
QString MainWindow::getPlatformName() {
    return m_systemMonitor ? m_systemMonitor->getPlatformName() : "Unknown";
}

// [PHASE 3] Delegate to SystemMonitor
// [PHASE 3] Delegate to SystemMonitor
int MainWindow::getSystemVolumePercent() {
    return m_systemMonitor ? m_systemMonitor->getSystemVolumePercent() : -1;
}

// [PHASE 3] Delegate to SystemMonitor
// [PHASE 3] Delegate to SystemMonitor - now handled in constructor
void MainWindow::setupVolumeMonitoring() {
    if (m_systemMonitor) {
        m_systemMonitor->startVolumeMonitoring();
    }
}

void MainWindow::setUIEnabled(bool enabled) {
    // Phase 1.1: Client list enabled state managed by ClientListPage
    if (m_clientListPage) {
        m_clientListPage->setEnabled(enabled);
    }
}

void MainWindow::updateConnectionStatus() {
    QString status = m_webSocketClient->getConnectionStatus();
    // Update the local network status in the new container
    setLocalNetworkStatus(status);
}

void MainWindow::updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles) {
    Q_UNUSED(globalPercent);
    Q_UNUSED(totalFiles);
    if (totalFiles == 0) return;

    CanvasSession* session = sessionForActiveUpload();
    if (!session || !session->canvas || !session->canvas->scene()) return;

    const int desired = qMax(0, filesCompleted);
    if (desired <= 0) return;

    int have = session->upload.serverCompletedFileIds.size();
    if (have >= desired) return;

    for (const QString& fileId : session->upload.currentUploadFileOrder) {
        if (session->upload.serverCompletedFileIds.contains(fileId)) continue;
        const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
        for (ResizableMediaBase* item : items) {
            if (item) item->setUploadUploaded();
        }
        session->upload.serverCompletedFileIds.insert(fileId);
        have++;
        if (have >= desired) break;
    }
}

int MainWindow::getInnerContentGap() const
{
    return gInnerContentGap;
}

// Phase 5: Delegate to RemoteClientInfoManager
QWidget* MainWindow::getRemoteClientInfoContainer() const {
    return m_remoteClientInfoManager ? m_remoteClientInfoManager->getContainer() : nullptr;
}

// Phase 6.1: Delegate to TopBarManager
QWidget* MainWindow::getLocalClientInfoContainer() const {
    return m_topBarManager ? m_topBarManager->getLocalClientInfoContainer() : nullptr;
}

QPushButton* MainWindow::getBackButton() const {
    return m_canvasViewPage ? m_canvasViewPage->getBackButton() : nullptr;
}

bool MainWindow::hasUnuploadedFilesForTarget(const QString& targetClientId) const {
    ScreenCanvas* canvas = canvasForClientId(targetClientId);
    if (!canvas || !canvas->scene()) return false;
    const QList<QGraphicsItem*> allItems = canvas->scene()->items();
    for (QGraphicsItem* it : allItems) {
        if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
            const QString fileId = media->fileId();
            if (fileId.isEmpty()) continue;
            if (!m_fileManager->isFileUploadedToClient(fileId, targetClientId)) return true;
        }
    }
    return false;
}


