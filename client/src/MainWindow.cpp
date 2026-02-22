#include "MainWindow.h"
#include "frontend/managers/ui/RemoteClientState.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/models/ClientInfo.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WatchManager.h"
#include "backend/files/FileWatcher.h"
#include "frontend/ui/widgets/SpinnerWidget.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "shared/rendering/ICanvasHost.h"
#include "backend/domain/media/MediaItems.h"
#include "frontend/rendering/canvas/OverlayPanels.h"
#include "backend/files/Theme.h"
#include "frontend/ui/theme/AppColors.h"
#include "backend/files/FileManager.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include "backend/domain/session/SessionManager.h"
#include "frontend/ui/widgets/RoundedContainer.h"
#include "frontend/ui/widgets/ClippedContainer.h"
#include "frontend/ui/widgets/ClientListDelegate.h"
#include "frontend/ui/pages/ClientListPage.h"
#include "frontend/ui/pages/CanvasViewPage.h"
#include "frontend/ui/theme/ThemeManager.h"
#include "frontend/managers/ui/RemoteClientInfoManager.h"
#include "backend/managers/system/SystemMonitor.h"
#include "frontend/managers/ui/TopBarManager.h"
#include "backend/managers/app/SystemTrayManager.h"
#include "backend/managers/app/MenuBarManager.h"
#include "backend/handlers/WebSocketMessageHandler.h"
#include "backend/handlers/ScreenEventHandler.h"
#include "backend/handlers/ClientListEventHandler.h"
#include "backend/handlers/UploadEventHandler.h"
#include "backend/controllers/CanvasSessionController.h"
#include "frontend/handlers/WindowEventHandler.h"
#include "backend/controllers/TimerController.h"
#include "frontend/managers/ui/UploadButtonStyleManager.h"
#include "backend/managers/app/SettingsManager.h"
#include "backend/managers/app/MigrationTelemetryManager.h"
#include "backend/managers/network/ClientListBuilder.h"
#include "frontend/handlers/UploadSignalConnector.h"
#include <QMenuBar>
#include <QHostInfo>

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

namespace {
bool cursorDebugEnabled() {
    static const bool enabled = qEnvironmentVariableIsSet("MOUFFETTE_CURSOR_DEBUG");
    return enabled;
}
}

#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacVideoThumbnailer.h"
#include "backend/platform/macos/MacWindowManager.h"
#endif
#include "frontend/ui/layout/ResponsiveLayoutManager.h"
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

// [Phase 17] Global style configuration - migrated to ThemeManager
// These macros provide backward compatibility while using ThemeManager
#define gWindowContentMarginTop (ThemeManager::instance()->getWindowContentMarginTop())
#define gWindowContentMarginRight (ThemeManager::instance()->getWindowContentMarginRight())
#define gWindowContentMarginBottom (ThemeManager::instance()->getWindowContentMarginBottom())
#define gWindowContentMarginLeft (ThemeManager::instance()->getWindowContentMarginLeft())
#define gWindowBorderRadiusPx (ThemeManager::instance()->getWindowBorderRadiusPx())
#define gInnerContentGap (ThemeManager::instance()->getInnerContentGap())
#define gDynamicBoxMinWidth (ThemeManager::instance()->getDynamicBoxMinWidth())
#define gDynamicBoxHeight (ThemeManager::instance()->getDynamicBoxHeight())
#define gDynamicBoxBorderRadius (ThemeManager::instance()->getDynamicBoxBorderRadius())
#define gDynamicBoxFontPx (ThemeManager::instance()->getDynamicBoxFontPx())
#define gRemoteClientContainerPadding (ThemeManager::instance()->getRemoteClientContainerPadding())
#define gTitleTextFontSize (ThemeManager::instance()->getTitleTextFontSize())
#define gTitleTextHeight (ThemeManager::instance()->getTitleTextHeight())

// Z-ordering constants used throughout the scene
namespace {
constexpr qreal Z_SCREENS = -1000.0;
constexpr qreal Z_MEDIA_BASE = 1.0;
constexpr qreal Z_REMOTE_CURSOR = 10000.0;
constexpr qreal Z_SCENE_OVERLAY = 12000.0; // above all scene content
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

    // Mirror the status in the top-bar remote client container
    if (m_remoteClientInfoManager) {
        if (QLabel* statusLabel = m_remoteClientInfoManager->getRemoteConnectionStatusLabel()) {
            const QString upStatus = status.toUpper();
            statusLabel->setText(upStatus);

            QString textColor;
            QString bgColor;
            if (upStatus == "CONNECTED") {
                textColor = AppColors::colorToCss(AppColors::gStatusConnectedText);
                bgColor = AppColors::colorToCss(AppColors::gStatusConnectedBg);
            } else if (upStatus == "ERROR" || upStatus.startsWith("CONNECTING") || upStatus.startsWith("RECONNECTING")) {
                textColor = AppColors::colorToCss(AppColors::gStatusWarningText);
                bgColor = AppColors::colorToCss(AppColors::gStatusWarningBg);
            } else {
                textColor = AppColors::colorToCss(AppColors::gStatusErrorText);
                bgColor = AppColors::colorToCss(AppColors::gStatusErrorBg);
            }

            statusLabel->setStyleSheet(
                QString("QLabel { "
                        "    color: %1; "
                        "    background-color: %2; "
                        "    border: none; "
                        "    border-radius: 0px; "
                        "    padding: 0px %4px; "
                        "    font-size: %3px; "
                        "    font-weight: bold; "
                        "}")
                    .arg(textColor)
                    .arg(bgColor)
                    .arg(gDynamicBoxFontPx)
                    .arg(gRemoteClientContainerPadding));
        }
    }
    
    // Update MainWindow state
    const QString up = status.toUpper();
    if (up == "CONNECTED") {
        m_remoteClientConnected = true;
    } else if (up == "DISCONNECTED" || up.startsWith("CONNECTING") || up == "ERROR") {
        m_remoteClientConnected = false;
    }
    
    // Manage inline spinner based on connection state
    // Spinner should only be visible during CONNECTING/RECONNECTING states
    if (up == "CONNECTED" || up == "DISCONNECTED" || up == "ERROR") {
        // Stop and hide spinner for stable states
        if (m_inlineSpinner && m_inlineSpinner->isSpinning()) {
            m_inlineSpinner->stop();
            m_inlineSpinner->hide();
        } else if (m_inlineSpinner) {
            m_inlineSpinner->hide();
        }
    } else if (up.startsWith("CONNECTING") || up.startsWith("RECONNECTING")) {
        // Show and start spinner for connecting states
        if (m_inlineSpinner && !m_inlineSpinner->isSpinning()) {
            m_inlineSpinner->show();
            m_inlineSpinner->start();
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

    ICanvasHost* screenCanvas = getScreenCanvas();
    if (screenCanvas) {
        if (!remoteConnected && propagateLoss) {
            screenCanvas->handleRemoteConnectionLost();
        }
        screenCanvas->setOverlayActionsEnabled(remoteConnected);
    }

    QPushButton* uploadButton = getUploadButton();
    if (!uploadButton) {
        return;
    }

    if (getUploadButtonInOverlay()) {
        if (!remoteConnected) {
            uploadButton->setEnabled(false);
            uploadButton->setCheckable(false);
            uploadButton->setChecked(false);
            uploadButton->setStyleSheet(screenCanvas ? screenCanvas->overlayDisabledButtonStyle() : QString());
            QFont defaultFont = getUploadButtonDefaultFont();
            AppColors::applyCanvasButtonFont(defaultFont);
            uploadButton->setFont(defaultFont);
            uploadButton->setFixedHeight(40);
            uploadButton->setMaximumWidth(ThemeManager::instance()->getUploadButtonMaxWidth());
        } else {
            UploadManager* uploadManager = getUploadManager();
            if (uploadManager) {
                QTimer::singleShot(0, this, [uploadManager]() {
                    if (uploadManager) {
                        emit uploadManager->uiStateChanged();
                    }
                });
            }
        }
    } else {
        uploadButton->setEnabled(remoteConnected);
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
      m_settingsManager(new SettingsManager(this, m_webSocketClient, this)), // Phase 12
      m_webSocketMessageHandler(new WebSocketMessageHandler(this, this)), // Phase 7.1
      m_screenEventHandler(new ScreenEventHandler(this, this)), // Phase 7.2
      m_uploadEventHandler(new UploadEventHandler(this, this)), // Phase 7.4
      m_clientListEventHandler(new ClientListEventHandler(this, m_webSocketClient, this)), // Phase 7.3
      m_canvasSessionController(new CanvasSessionController(this, this)), // Phase 8
      m_windowEventHandler(new WindowEventHandler(this, this)), // Phase 9
      m_timerController(new TimerController(this, this)), // Phase 10
      m_uploadButtonStyleManager(new UploadButtonStyleManager(this, this)), // Phase 11
      m_uploadSignalConnector(new UploadSignalConnector(this)), // Phase 15
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
    // [Phase 12] Load persisted settings (server URL, auto-upload, persistent client ID)
    m_settingsManager->loadSettings();
    MigrationTelemetryManager::logStartupFlag(
        useQuickCanvasRenderer(),
        m_settingsManager ? m_settingsManager->getQuickCanvasFlagSource() : QStringLiteral("unknown"));
    
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
    if (m_canvasSessionController && useQuickCanvasRenderer()) {
        QTimer::singleShot(0, this, [this]() {
            if (m_canvasSessionController) {
                m_canvasSessionController->prewarmQuickCanvasHost();
            }
        });
    }
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

    // [PHASE 11] UI refresh when upload state changes - delegate to UploadButtonStyleManager
    connect(m_uploadManager, &UploadManager::uiStateChanged, this, [this]() {
        if (m_uploadButtonStyleManager && m_uploadButton) {
            m_uploadButtonStyleManager->applyUploadButtonStyle(m_uploadButton);
        }
    });
    connect(m_uploadManager, &UploadManager::uploadProgress, this, [this](int percent, int filesCompleted, int totalFiles){
        if (!m_uploadButton) return;
        if (m_uploadButtonStyleManager) {
            m_uploadButtonStyleManager->updateUploadButtonProgress(m_uploadButton, percent, filesCompleted, totalFiles);
        }
        
        // Update individual media progress based on server-acknowledged data
        updateIndividualProgressFromServer(percent, filesCompleted, totalFiles);
    });
    connect(m_uploadManager, &UploadManager::uploadFinished, this, [this]() {
        if (m_uploadButtonStyleManager && m_uploadButton) {
            m_uploadButtonStyleManager->applyUploadButtonStyle(m_uploadButton);
        }
    });
    connect(m_uploadManager, &UploadManager::allFilesRemoved, this, [this]() {
        if (m_uploadButtonStyleManager && m_uploadButton) {
            m_uploadButtonStyleManager->applyUploadButtonStyle(m_uploadButton);
        }
    });

    // [Phase 10] Setup timers via TimerController
    m_timerController->setupTimers();

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
// [PHASE 8] Session lookup - delegate to CanvasSessionController
MainWindow::CanvasSession* MainWindow::findCanvasSession(const QString& persistentClientId) {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->findCanvasSession(persistentClientId)) : nullptr;
}

const MainWindow::CanvasSession* MainWindow::findCanvasSession(const QString& persistentClientId) const {
    return m_canvasSessionController ? static_cast<const CanvasSession*>(m_canvasSessionController->findCanvasSession(persistentClientId)) : nullptr;
}

MainWindow::CanvasSession* MainWindow::findCanvasSessionByServerClientId(const QString& serverClientId) {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->findCanvasSessionByServerClientId(serverClientId)) : nullptr;
}

const MainWindow::CanvasSession* MainWindow::findCanvasSessionByServerClientId(const QString& serverClientId) const {
    return m_canvasSessionController ? static_cast<const CanvasSession*>(m_canvasSessionController->findCanvasSessionByServerClientId(serverClientId)) : nullptr;
}

MainWindow::CanvasSession* MainWindow::findCanvasSessionByIdeaId(const QString& canvasSessionId) {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->findCanvasSessionByIdeaId(canvasSessionId)) : nullptr;
}

// [PHASE 8] Session lifecycle - delegate to CanvasSessionController
MainWindow::CanvasSession& MainWindow::ensureCanvasSession(const ClientInfo& client) {
    void* sessionPtr = m_canvasSessionController->ensureCanvasSession(client);
    return *static_cast<CanvasSession*>(sessionPtr);
}

// [PHASE 8] Session configuration - delegate to CanvasSessionController
void MainWindow::configureCanvasSession(CanvasSession& session) {
    if (m_canvasSessionController) {
        m_canvasSessionController->configureCanvasSession(&session);
    }
}

// [PHASE 8] Switch canvas session - delegate to CanvasSessionController
void MainWindow::switchToCanvasSession(const QString& persistentClientId) {
    if (m_canvasSessionController) {
        m_canvasSessionController->switchToCanvasSession(persistentClientId);
    }
}

// [PHASE 8] Update upload button - delegate to CanvasSessionController
void MainWindow::updateUploadButtonForSession(CanvasSession& session) {
    if (m_canvasSessionController) {
        m_canvasSessionController->updateUploadButtonForSession(&session);
    }
}

// [PHASE 8] Unload uploads - delegate to CanvasSessionController
void MainWindow::unloadUploadsForSession(CanvasSession& session, bool attemptRemote) {
    if (m_canvasSessionController) {
        m_canvasSessionController->unloadUploadsForSession(&session, attemptRemote);
    }
}

// [Phase 12] Delegate to SettingsManager
bool MainWindow::getAutoUploadImportedMedia() const {
    return m_settingsManager ? m_settingsManager->getAutoUploadImportedMedia() : false;
}

bool MainWindow::useQuickCanvasRenderer() const {
    return m_settingsManager ? m_settingsManager->getUseQuickCanvasRenderer() : false;
}

void MainWindow::markCanvasLoadRequest(const QString& persistentClientId) {
    if (persistentClientId.isEmpty()) {
        return;
    }
    m_canvasLoadRequestMsBySession.insert(persistentClientId, QDateTime::currentMSecsSinceEpoch());
    MigrationTelemetryManager::logCanvasLoadRequest(persistentClientId);
}

void MainWindow::recordCanvasLoadReady(const QString& persistentClientId, int screenCount) {
    if (persistentClientId.isEmpty()) {
        return;
    }
    const qint64 startedAt = m_canvasLoadRequestMsBySession.take(persistentClientId);
    qint64 latencyMs = -1;
    if (startedAt > 0) {
        latencyMs = qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - startedAt);
    }
    MigrationTelemetryManager::logCanvasLoadReady(persistentClientId, screenCount, latencyMs);
}

QString MainWindow::createIdeaId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// [PHASE 8] Rotate session idea - delegate to CanvasSessionController
void MainWindow::rotateSessionIdea(CanvasSession& session) {
    if (m_canvasSessionController) {
        m_canvasSessionController->rotateSessionIdea(&session);
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

// [Phase 15] Delegate upload signal connections to UploadSignalConnector
void MainWindow::connectUploadSignals() {
    if (m_uploadSignalConnector) {
        m_uploadSignalConnector->connectAllSignals(this, m_uploadManager, m_webSocketClient, m_uploadSignalsConnected);
    }
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

// [Phase 16] Delegate to ClientListBuilder
QList<ClientInfo> MainWindow::buildDisplayClientList(const QList<ClientInfo>& connectedClients) {
    return ClientListBuilder::buildDisplayClientList(this, connectedClients);
}

ICanvasHost* MainWindow::canvasForClientId(const QString& clientId) const {
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


// [PHASE 8] Session for active upload - delegate to CanvasSessionController
MainWindow::CanvasSession* MainWindow::sessionForActiveUpload() {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->sessionForActiveUpload()) : nullptr;
}

// [PHASE 8] Session for upload ID - delegate to CanvasSessionController
MainWindow::CanvasSession* MainWindow::sessionForUploadId(const QString& uploadId) {
    return m_canvasSessionController ? static_cast<CanvasSession*>(m_canvasSessionController->sessionForUploadId(uploadId)) : nullptr;
}

// [PHASE 8] Clear upload tracking - delegate to CanvasSessionController
void MainWindow::clearUploadTracking(CanvasSession& session) {
    if (m_canvasSessionController) {
        m_canvasSessionController->clearUploadTracking(&session);
    }
}


void MainWindow::updateApplicationSuspendedState(bool suspended) {
    if (m_windowEventHandler) {
        m_windowEventHandler->updateApplicationSuspendedState(suspended);
    }
}


void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (m_windowEventHandler) {
        m_windowEventHandler->handleChangeEvent(event);
    }
}

void MainWindow::handleApplicationStateChanged(Qt::ApplicationState state) {
    if (m_windowEventHandler) {
        m_windowEventHandler->handleApplicationStateChanged(state);
    }
}

void MainWindow::showScreenView(const ClientInfo& client) {
    if (!m_navigationManager) return;
    CanvasSession& session = ensureCanvasSession(client);
    markCanvasLoadRequest(session.persistentClientId);
    const bool sessionHasActiveScreens = session.canvas && session.canvas->hasActiveScreens();
    const bool sessionHasStoredScreens = !session.lastClientInfo.getScreens().isEmpty();
    const bool hasRenderableCachedContent = sessionHasActiveScreens;
    const bool hasCachedContent = hasRenderableCachedContent || sessionHasStoredScreens;
    switchToCanvasSession(session.persistentClientId);
    m_activeRemoteClientId = session.serverAssignedId;
    m_remoteClientConnected = false;
    m_selectedClient = session.lastClientInfo;
    ClientInfo effectiveClient = session.lastClientInfo;
    const bool alreadyOnScreenView = m_navigationManager->isOnScreenView();
    const QString currentId = alreadyOnScreenView ? m_navigationManager->currentClientId() : QString();
    const bool alreadyOnThisClient = alreadyOnScreenView && currentId == effectiveClient.getId() && !effectiveClient.getId().isEmpty();

    if (hasRenderableCachedContent) {
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
        if (hasRenderableCachedContent && effectiveClient.isOnline()) {
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

    if (hasRenderableCachedContent) {
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

void MainWindow::setRemoteClientState(const RemoteClientState& state, bool propagateLoss) {
    // Update internal connection state
    m_remoteClientConnected = (state.connectionStatus == RemoteClientState::Connected);
    
    // Manage spinner based on state
    if (state.spinnerActive) {
        if (m_inlineSpinner && !m_inlineSpinner->isSpinning()) {
            m_inlineSpinner->show();
            m_inlineSpinner->start();
        }
    } else {
        if (m_inlineSpinner && m_inlineSpinner->isSpinning()) {
            m_inlineSpinner->stop();
            m_inlineSpinner->hide();
        } else if (m_inlineSpinner) {
            m_inlineSpinner->hide();
        }
    }
    
    // Delegate to CanvasViewPage
    if (m_canvasViewPage) {
        m_canvasViewPage->setRemoteConnectionStatus(state.statusText(), propagateLoss);
    }
    
    // Apply state to RemoteClientInfoManager (atomically, no flicker)
    if (m_remoteClientInfoManager) {
        m_remoteClientInfoManager->applyState(state);
    }
    
    // Show client list placeholder when connecting
    if (state.connectionStatus == RemoteClientState::Connecting || 
        state.connectionStatus == RemoteClientState::Reconnecting) {
        if (m_clientListPage) {
            m_clientListPage->ensureClientListPlaceholder();
        }
    }

    refreshOverlayActionsState(m_remoteClientConnected, propagateLoss);
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
    showScreenView(client);
    // ScreenNavigationManager will request screens; no need to duplicate here
}

// Phase 1.1: New slot connected to ClientListPage::ongoingSceneClicked signal
void MainWindow::onOngoingSceneSelected(const QString& persistentClientId) {
    CanvasSession* session = findCanvasSession(persistentClientId);
    if (!session) return;

    showScreenView(session->lastClientInfo);
}

// Note: generic message hook removed; we handle specific message types via dedicated slots


MainWindow::~MainWindow() {
    // Cleanly disconnect
    if (m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
    }
    // [Phase 12] Persist current settings on shutdown (safety in case dialog not used)
    if (m_settingsManager) {
        m_settingsManager->saveSettings();
    }
}

void MainWindow::updateStylesheetsForTheme() {
    // [Phase 14.2] Delegate to ThemeManager
    if (ThemeManager::instance()) {
        ThemeManager::instance()->updateAllWidgetStyles(this);
    }
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
    ThemeManager::instance()->applyTitleText(m_pageTitleLabel);
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
    m_backButton = ThemeManager::createPillButton("← Go Back");
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
    m_connectToggleButton = ThemeManager::createPillButton("Disable");
    // Ensure button is sized properly for "Disable"/"Enable" text (using longer text)
    int toggleTextWidth = m_connectToggleButton->fontMetrics().horizontalAdvance("Disable") + 24;
    int toggleButtonWidth = qMax(80, toggleTextWidth);
    m_connectToggleButton->setFixedWidth(toggleButtonWidth); // Use fixed width to prevent any changes
    connect(m_connectToggleButton, &QPushButton::clicked, this, &MainWindow::onEnableDisableClicked);

    // Settings button
    m_settingsButton = ThemeManager::createPillButton("Settings");
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
        w.backButton = m_backButton;
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
            [this](const QString& targetId, int x, int y, int screenId, qreal normalizedX, qreal normalizedY) {
                if (!m_screenCanvas) return;
                if (m_stackedWidget->currentWidget() != m_canvasViewPage) return;  // Phase 1.2
                bool matchWatch = (m_watchManager && targetId == m_watchManager->watchedClientId());
                bool matchSelected = (!m_selectedClient.getId().isEmpty() && targetId == m_selectedClient.getId());
                if (matchWatch || matchSelected) {
                    const QPoint rawPoint(x, y);
                    bool mapped = false;
                    if (screenId >= 0 && normalizedX >= 0.0 && normalizedY >= 0.0) {
                        CanvasSession* session = findCanvasSessionByServerClientId(targetId);
                        QString resolutionPath = QStringLiteral("by_server_id");
                        if (!session) {
                            session = findCanvasSession(targetId);
                            resolutionPath = QStringLiteral("by_persistent_id");
                        }
                        if (!session && m_watchManager && !m_watchManager->watchedClientId().isEmpty()) {
                            session = findCanvasSessionByServerClientId(m_watchManager->watchedClientId());
                            resolutionPath = QStringLiteral("by_watch_server_id");
                            if (!session) {
                                session = findCanvasSession(m_watchManager->watchedClientId());
                                resolutionPath = QStringLiteral("by_watch_persistent_id");
                            }
                        }
                        if (!session && !m_selectedClient.clientId().isEmpty()) {
                            session = findCanvasSession(m_selectedClient.clientId());
                            resolutionPath = QStringLiteral("by_selected_client_id");
                        }
                        if (!session && !m_activeSessionIdentity.isEmpty()) {
                            session = findCanvasSession(m_activeSessionIdentity);
                            resolutionPath = QStringLiteral("by_active_session");
                        }

                        if (cursorDebugEnabled()) {
                            qDebug() << "[CursorDebug][Viewer][Lookup]"
                                     << "targetId=" << targetId
                                     << "watchId=" << (m_watchManager ? m_watchManager->watchedClientId() : QString())
                                     << "selectedId=" << m_selectedClient.getId()
                                     << "sessionResolved=" << (session != nullptr)
                                     << "path=" << resolutionPath
                                     << "rawGlobal=" << rawPoint
                                     << "screenId=" << screenId
                                     << "norm=" << normalizedX << normalizedY;
                        }

                        if (session) {
                            const QList<ScreenInfo> screens = session->lastClientInfo.getScreens();
                            for (const ScreenInfo& screen : screens) {
                                if (screen.id != screenId || screen.width <= 0 || screen.height <= 0) {
                                    continue;
                                }

                                const qreal clampedX = std::clamp(normalizedX, 0.0, 1.0);
                                const qreal clampedY = std::clamp(normalizedY, 0.0, 1.0);
                                const int maxDx = std::max(0, screen.width - 1);
                                const int maxDy = std::max(0, screen.height - 1);
                                x = screen.x + static_cast<int>(std::lround(clampedX * static_cast<qreal>(maxDx)));
                                y = screen.y + static_cast<int>(std::lround(clampedY * static_cast<qreal>(maxDy)));
                                mapped = true;
                                if (cursorDebugEnabled()) {
                                    qDebug() << "[CursorDebug][Viewer][Mapped]"
                                             << "screenRect=" << QRect(screen.x, screen.y, screen.width, screen.height)
                                             << "rawGlobal=" << rawPoint
                                             << "mappedGlobal=" << QPoint(x, y)
                                             << "screenId=" << screenId
                                             << "norm=" << normalizedX << normalizedY;
                                }
                                break;
                            }
                        }
                    }

                    if (cursorDebugEnabled() && !mapped) {
                        qDebug() << "[CursorDebug][Viewer][FallbackRaw]"
                                 << "rawGlobal=" << rawPoint
                                 << "screenId=" << screenId
                                 << "norm=" << normalizedX << normalizedY;
                    }

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

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_windowEventHandler) {
        m_windowEventHandler->handleCloseEvent(event);
    } else {
        QMainWindow::closeEvent(event);
    }
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (m_windowEventHandler) {
        m_windowEventHandler->handleShowEvent(event);
    }
}

void MainWindow::hideEvent(QHideEvent* event) {
    QMainWindow::hideEvent(event);
    if (m_windowEventHandler) {
        m_windowEventHandler->handleHideEvent(event);
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if (m_windowEventHandler) {
        m_windowEventHandler->handleResizeEvent(event);
    }
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    if (m_windowEventHandler) {
        m_windowEventHandler->onTrayIconActivated(reason);
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
    // [Phase 12] Delegate to SettingsManager
    if (m_settingsManager) {
        m_settingsManager->showSettingsDialog();
    }
}

// (Removed stray duplicated code block previously injected)

void MainWindow::scheduleReconnect() {
    if (m_timerController) {
        m_timerController->scheduleReconnect();
    }
}

void MainWindow::attemptReconnect() {
    if (m_timerController) {
        m_timerController->attemptReconnect();
    }
}

// [PHASE 7.1] Helper methods for WebSocketMessageHandler
void MainWindow::resetReconnectState() {
    if (m_timerController) {
        m_timerController->resetReconnectState();
    }
}

void MainWindow::resetAllSessionUploadStates() {
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        if (session->canvas) {
            for (ResizableMediaBase* media : session->canvas->enumerateMediaItems()) {
                if (!media) {
                    continue;
                }
                if (media->uploadState() == ResizableMediaBase::UploadState::Uploading) {
                    media->setUploadNotUploaded();
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
    // [PHASE 10] Delegate to TimerController
    if (m_timerController) {
        m_timerController->setWatchedState(watched);
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
    // [Phase 12] Get server URL from SettingsManager
    const QString url = m_settingsManager ? m_settingsManager->getServerUrl() : DEFAULT_SERVER_URL;
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
    // [Phase 14.3] Delegate to UploadEventHandler
    if (m_uploadEventHandler) {
        m_uploadEventHandler->updateIndividualProgressFromServer(globalPercent, filesCompleted, totalFiles);
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
    return m_backButton;
}

bool MainWindow::hasUnuploadedFilesForTarget(const QString& targetClientId) const {
    ICanvasHost* canvas = canvasForClientId(targetClientId);
    if (!canvas) {
        return false;
    }

    for (ResizableMediaBase* media : canvas->enumerateMediaItems()) {
        if (!media) {
            continue;
        }
        const QString fileId = media->fileId();
        if (fileId.isEmpty()) {
            continue;
        }
        if (!m_fileManager->isFileUploadedToClient(fileId, targetClientId)) {
            return true;
        }
    }
    return false;
}


