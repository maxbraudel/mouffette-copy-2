#include "MainWindow.h"
#include "frontend/managers/ui/RemoteClientState.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/models/ClientInfo.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "backend/network/UploadManager.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/RemoteCacheStore.h"
#include "backend/files/FileWatcher.h"
#include "frontend/ui/widgets/SpinnerWidget.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "shared/rendering/ICanvasHost.h"
#include "backend/domain/media/MediaItems.h"
#include "backend/domain/media/MediaRuntimeHooks.h"
#include "frontend/rendering/canvas/OverlayPanels.h"
#include "backend/files/Theme.h"
#include "frontend/ui/theme/AppColors.h"
#include "backend/files/FileManager.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include "backend/domain/session/SessionManager.h"
#include "backend/domain/project/ProjectManager.h"
#include "backend/domain/project/ProjectModel.h"
#include "backend/domain/scene/SceneActivityModel.h"
#include "backend/network/SceneRunCoordinator.h"
#include "frontend/ui/widgets/RoundedContainer.h"
#include "frontend/ui/widgets/ClippedContainer.h"
#include "frontend/ui/widgets/ClientListDelegate.h"
#include "frontend/ui/pages/ClientListPage.h"
#include "frontend/ui/pages/CanvasViewPage.h"
#include "frontend/ui/pages/HistoryPage.h"
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
#include "backend/config/AppConfig.h"
#include "frontend/managers/ui/UploadButtonStyleManager.h"
#include "backend/managers/app/SettingsManager.h"
#include "backend/managers/app/MigrationTelemetryManager.h"
#include "backend/managers/network/ClientListBuilder.h"
#include "backend/managers/network/ConnectionManager.h"
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
#include <QStatusBar>
#include <QGraphicsPixmapItem>
#include <QGraphicsSceneMouseEvent>
#include <QPen>
#include <QBrush>
#include <QUrl>
#include <QImage>
#include <QAbstractItemView>
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
#include <QMessageBox>
#include <climits>
#include <memory>

namespace {
constexpr double kMaxSafeJsonInteger = 9007199254740991.0;

bool readSafePositiveJsonInteger(const QJsonValue& value, quint64* output)
{
    if (!output || !value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 1.0
        || number > kMaxSafeJsonInteger || std::floor(number) != number) {
        return false;
    }
    *output = static_cast<quint64>(number);
    return true;
}

ProjectManager::TimingPolicy projectTimingPolicyFromConfig() {
    ProjectManager::TimingPolicy timing;
    timing.remoteSessionHiddenTimeoutMs =
        AppConfig::instance().remoteSessionHiddenTimeoutMs();
    timing.projectHiddenRetentionMs =
        AppConfig::instance().projectHiddenRetentionMs();
    return timing;
}

QString canonicalExistingPath(const QString& sourcePath) {
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        return {};
    }
    return info.canonicalFilePath();
}

QString sourceIdentityForPath(const QString& canonicalPath) {
    const QFileInfo info(canonicalPath);
    if (!info.exists() || !info.isFile()) {
        return {};
    }
    return QStringLiteral("%1:%2")
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

QString sha256ForPath(const QString& canonicalPath) {
    QFile file(canonicalPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(4 * 1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            return {};
        }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
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
      m_projectManager(new ProjectManager(projectTimingPolicyFromConfig(), this)),
      m_sceneActivityModel(new SceneActivityModel(this)),
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
      m_menuBarManager(new MenuBarManager(this, this)), // Phase 6.3
      m_systemTrayManager(new SystemTrayManager(this)), // Phase 6.2
      m_webSocketClient(new WebSocketClient(this)),
      m_connectionManager(new ConnectionManager(m_webSocketClient, this)),
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
      m_uploadManager(new UploadManager(m_fileManager, this)),
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
    if (m_canvasViewPage) {
        connect(m_canvasViewPage, &CanvasViewPage::disconnectRequested,
                this, &MainWindow::onDisconnectProjectRequested);
        connect(m_canvasViewPage, &CanvasViewPage::deleteProjectRequested,
                this, &MainWindow::onDeleteProjectRequested);
    }
    if (m_projectManager) {
        connect(m_projectManager, &ProjectManager::remoteSessionCloseDue,
                this, [this](const QString&, const QString& targetDeviceId) {
            terminateProjectRemoteSession(targetDeviceId, true);
        });
        connect(m_projectManager,
                &ProjectManager::projectRestoredAfterSessionDeadline,
                this, [this](const QString&, const QString& targetDeviceId) {
            // Every path which makes a hidden canvas visible (window restore,
            // tray, wake, Settings return, or explicit navigation) receives
            // the same single post-60-second reconnect allowance.
            m_reopenAfterSessionCloseTargets.insert(targetDeviceId);
            m_remoteSessionOpenSuppressedTargets.remove(targetDeviceId);
            QTimer::singleShot(0, this, [this, targetDeviceId]() {
                if (!m_navigationManager
                    || !m_navigationManager->isOnScreenView()
                    || m_activeSessionIdentity != targetDeviceId) {
                    return;
                }
                CanvasSession* session = findCanvasSession(targetDeviceId);
                if (!session) return;
                ensureRemoteSessionForClient(session->lastClientInfo);
            });
        });
        connect(m_projectManager, &ProjectManager::projectAboutToDelete,
                this, [this](const ProjectRecord& project) {
            terminateProjectRemoteSession(project.targetDeviceId, true);
        });
        connect(m_projectManager, &ProjectManager::projectCheckpointDue,
                this, &MainWindow::persistProjectCanvas);
        connect(m_projectManager, &ProjectManager::projectDeleted,
                this, [this](const QString&, const QString& targetDeviceId) {
            removeRuntimeCanvasSession(targetDeviceId);
        });
        connect(m_projectManager, &ProjectManager::projectsChanged,
                this, &MainWindow::refreshProjectClientList);
        connect(m_projectManager, &ProjectManager::persistenceError,
                this, [](const QString& message) {
            qWarning().noquote() << "Project persistence error:" << message;
        });
    }
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
    connect(g_remoteSceneController, &RemoteSceneController::teardownSettled,
            this, &MainWindow::handleRemoteRendererTeardownSettled,
            Qt::UniqueConnection);
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
        // Volume is part of every protocol-v2 device snapshot; it is not tied
        // to any remote controller's Project or session.
        connect(m_systemMonitor, &SystemMonitor::volumeChanged, this, [this](int) {
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                syncRegistration();
            }
        });
        connect(m_systemMonitor, &SystemMonitor::screenConfigurationChanged,
                this, [this](const QList<ScreenInfo>&) {
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
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
    // The handler above applies discovery/snapshot updates first. This second
    // observer only reconciles the active Project with its v2 RemoteSession;
    // it never owns retry policy or rebuilds the list a second time.
    connect(m_webSocketClient, &WebSocketClient::clientListReceived,
            this, &MainWindow::onClientListReceived);

    // SceneActivityModel is the sole source for the Ongoing Scenes UI. A run
    // is inserted only after the protocol-v2 coordinator reaches Live and is
    // removed as soon as it leaves Live.
    if (m_sceneActivityModel && m_webSocketClient) {
        m_sceneActivityModel->setLocalDeviceId(m_webSocketClient->deviceId());
        if (SceneRunCoordinator* sceneRuns = m_webSocketClient->sceneRunCoordinator()) {
            connect(sceneRuns, &SceneRunCoordinator::runChanged, this,
                    [this, sceneRuns](const QString& sceneRunId,
                                      SceneRunCoordinator::Phase phase) {
                if (!m_sceneActivityModel) return;
                if (phase != SceneRunCoordinator::Phase::Live) {
                    m_sceneActivityModel->remove(sceneRunId);
                    return;
                }
                const SceneRunCoordinator::Run run = sceneRuns->run(sceneRunId);
                if (run.sceneRunId.isEmpty()) return;
                qint64 startedAt = run.startEpochMs;
                if (startedAt <= 0) {
                    const SceneActivityModel::Activity existing =
                        m_sceneActivityModel->activity(sceneRunId);
                    startedAt = existing.startedAtEpochMs > 0
                        ? existing.startedAtEpochMs
                        : QDateTime::currentMSecsSinceEpoch();
                }
                m_sceneActivityModel->upsertLive(
                    run.sceneRunId, run.remoteSessionId, run.ownerDeviceId,
                    run.targetDeviceId, startedAt);
            });
        }
        connect(m_webSocketClient, &WebSocketClient::transportHealthChanged,
                m_sceneActivityModel, &SceneActivityModel::setAllDegraded);
        connect(m_webSocketClient, &WebSocketClient::remoteSessionLeaseStateChanged,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionLeaseState(envelope);
            if (!m_sceneActivityModel) return;
            const QString sessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
            const QString phase = envelope.value(QStringLiteral("phase"))
                                      .toString(envelope.value(QStringLiteral("state")).toString());
            if (phase.compare(QStringLiteral("Grace"), Qt::CaseInsensitive) == 0) {
                m_sceneActivityModel->setSessionDegraded(sessionId, true);
            } else if (phase.compare(QStringLiteral("Active"), Qt::CaseInsensitive) == 0) {
                m_sceneActivityModel->setSessionDegraded(sessionId, false);
            } else if (phase.compare(QStringLiteral("Terminating"), Qt::CaseInsensitive) == 0
                       || phase.compare(QStringLiteral("CleanupPending"), Qt::CaseInsensitive) == 0
                       || phase.compare(QStringLiteral("Closed"), Qt::CaseInsensitive) == 0) {
                m_sceneActivityModel->removeForSession(sessionId);
            }
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionTerminating,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionTerminating(envelope);
            if (m_sceneActivityModel) {
                m_sceneActivityModel->removeForSession(
                    envelope.value(QStringLiteral("remoteSessionId")).toString());
            }
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionClosed,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionClosed(envelope);
            if (m_sceneActivityModel) {
                m_sceneActivityModel->removeForSession(
                    envelope.value(QStringLiteral("remoteSessionId")).toString());
            }
        });
        connect(m_webSocketClient, &WebSocketClient::serverRestarted,
                m_sceneActivityModel, [this](const QString&, const QString&) {
            if (m_sceneActivityModel) m_sceneActivityModel->clear();
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionOpened,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionReady(envelope, false);
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionResumed,
                this, [this](const QJsonObject& envelope) {
            handleRemoteSessionReady(envelope, true);
        });
        connect(m_webSocketClient, &WebSocketClient::remoteSessionError,
                this, &MainWindow::handleRemoteSessionError);
    }
    
    // ConnectionManager is the sole owner of retries and transport status.
    connect(m_connectionManager, &ConnectionManager::connectionError,
            this, &MainWindow::onConnectionError);
    connect(m_webSocketClient, &WebSocketClient::connected,
            this, &MainWindow::retryPendingTeardownAcks);
    connect(m_webSocketClient, &WebSocketClient::disconnected,
            this, [this]() {
        if (m_cleanShutdownPrepared && !m_cleanShutdownFinished) {
            finishCleanShutdown();
        }
    });
    connect(m_connectionManager, &ConnectionManager::statusChanged,
            this, [this](const QString& status) {
        setLocalNetworkStatus(status);
        if (status == QLatin1String("Reconnecting")) {
            for (ClientInfo& client : m_discoveredClients) {
                client.setStatus(QStringLiteral("Reconnecting"));
                client.setAvailabilityStatus(QStringLiteral("Reconnecting"));
            }
            refreshProjectClientList();
            if (m_screenCanvas) {
                // Grace disables new remote commands but does not tear down the
                // running graph or discard resumable upload state.
                m_screenCanvas->setOverlayActionsEnabled(false);
            }
        }
    });
    connect(m_connectionManager, &ConnectionManager::leaseExpired,
            this, [this](const QString& serverBootId, quint64 generation) {
        if (m_cleanShutdownPrepared) {
            // WebSocketClient::disconnect() deliberately expires its in-memory
            // lease. During a requested quit this is cleanup, not an outage,
            // and must not create a misleading history entry/toast.
            if (m_sceneActivityModel) m_sceneActivityModel->clear();
            return;
        }
        // This signal is delivered synchronously from WebSocketClient before
        // SceneRunCoordinator::clearSessions(). Capture every incoming binding
        // now; a queued cleanup would lose the immutable cache correlation.
        beginTerminalIncomingCacheCleanup(QStringLiteral("lease_expired"));
        if (m_toastSystem) {
            NotificationRequest notification;
            notification.severity = NotificationSeverity::Warning;
            notification.category = QStringLiteral("Remote session");
            notification.message = QStringLiteral(
                "Connection lost for more than 3 seconds. Remote scenes were stopped and session uploads were invalidated.");
            notification.correlationId = QStringLiteral("lease-expired:%1:%2")
                                             .arg(serverBootId)
                                             .arg(generation);
            notification.terminal = true;
            m_toastSystem->publishNotification(notification);
        }
        if (m_sceneActivityModel) m_sceneActivityModel->clear();
        if (m_sessionManager) {
            const QList<QString> targets =
                m_sessionManager->getAllPersistentClientIds();
            for (const QString& target : targets) {
                m_remoteSessionOpenSuppressedTargets.insert(target);
                if (CanvasSession* session =
                        m_sessionManager->findSession(target)) {
                    terminateProjectRemoteSession(target, false);
                }
            }
            markAllSessionsOffline();
        }
        m_remoteSessionOpenPendingTargets.clear();
        m_remoteSessionOpenTargetByRequestId.clear();
        m_locallyTerminatingRemoteSessions.clear();
        m_pendingTeardownAcks.clear();
        m_discoveredClients.clear();
        refreshProjectClientList();
    });
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    // UploadManager subscribes to the canonical protocol-v2 upload envelope.
    m_uploadManager->setWebSocketClient(m_webSocketClient);
    connect(m_uploadManager, &UploadManager::terminalIncomingCleanupRequired,
            this, &MainWindow::beginTerminalIncomingCacheCleanup);
    connect(m_uploadManager,
            &UploadManager::receiverAdvertisementReadinessChanged,
            this, [this](bool ready, const QString&) {
        // A welcome may have completed while renderer destruction was still
        // pending. Publish the device only after logical quarantine commits.
        if (ready && m_webSocketClient && m_webSocketClient->isConnected()) {
            syncRegistration();
        }
    });

    connect(m_uploadManager, &UploadManager::assetRemovalCommitted,
            this, [this](const QString& targetDeviceId,
                         const QStringList& localFileIds) {
        CanvasSession* session = m_sessionManager
            ? m_sessionManager->findSession(targetDeviceId) : nullptr;
        if (!session) return;
        for (const QString& fileId : localFileIds) {
            session->knownRemoteFileIds.remove(fileId);
            session->expectedIdeaFileIds.remove(fileId);
        }
        session->upload.remoteFilesPresent =
            !session->knownRemoteFileIds.isEmpty();
    });
    connect(m_uploadManager, &UploadManager::assetRemovalFailed,
            this, [this](const QString& targetDeviceId,
                         const QString& remoteSessionId,
                         const QStringList& localFileIds,
                         const QString& reason) {
        Q_UNUSED(localFileIds);
        TOAST_ERROR(QStringLiteral(
            "Remote asset cleanup failed; the session is being closed: %1")
                        .arg(reason), 5000);
        RemoteSessionCoordinator* coordinator = m_webSocketClient
            ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
        const RemoteSessionCoordinator::Binding binding = coordinator
            ? coordinator->byId(remoteSessionId)
            : RemoteSessionCoordinator::Binding();
        if (!binding.remoteSessionId.isEmpty()
            && binding.ownerDeviceId == m_webSocketClient->deviceId()) {
            terminateProjectRemoteSession(targetDeviceId, true);
        } else {
            clearRemoteSessionRuntimeState(targetDeviceId, false);
        }
    });
    
    // A file is invalidated only after its last local media reference is gone.
    // Remote copies are addressed through the exact authenticated session
    // inventory; no target name or historical canvas identifier is accepted.
    FileManager::setFileRemovalNotifier([this](const QString& fileId, const QList<QString>& clientIds, const QList<QString>& canvasSessionIds) {
        Q_UNUSED(canvasSessionIds);
        if (!m_uploadManager) return;
        QSet<QString> uniqueTargets(clientIds.cbegin(), clientIds.cend());
        for (const QString& targetDeviceId : uniqueTargets) {
            m_uploadManager->requestAssetRemoval(
                targetDeviceId, fileId, QStringLiteral("source_removed"));
        }
    });
    
    // FileWatcher: remove media items when their source files are deleted
    connect(m_fileWatcher, &FileWatcher::filesDeleted, this,
            [this](const QList<ResizableMediaBase*>& mediaItems) {
        removeInvalidMediaItems(mediaItems);
    });
    
    // Phase 4.3: Inject FileManager into media runtime hooks
    MediaRuntimeHooks::setFileManager(m_fileManager);
    
    // File error callback: remove media items when playback detects missing/corrupted files
    MediaRuntimeHooks::setFileErrorNotifier([this](ResizableMediaBase* mediaItem) {
        removeInvalidMediaItems({mediaItem});
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
    connect(m_uploadManager, &UploadManager::uploadFinished, this,
            [this](const QString&) {
        if (m_uploadButtonStyleManager && m_uploadButton) {
            m_uploadButtonStyleManager->applyUploadButtonStyle(m_uploadButton);
        }
    });
    // [Phase 10] Setup timers via TimerController
    m_timerController->setupTimers();

    // Initialize toast notification system
    m_toastSystem = new ToastNotificationSystem(this, this);
    ToastNotificationSystem::setInstance(m_toastSystem);
    if (m_historyPage && m_toastSystem->notificationCenter()) {
        NotificationCenter* center = m_toastSystem->notificationCenter();
        m_historyPage->setNotificationCenter(center);
        connect(center, &NotificationCenter::unreadCountChanged,
                this, &MainWindow::updateHistoryUnreadBadge);
        updateHistoryVisibilityState();
        updateHistoryUnreadBadge(center->unreadCount());
    }

    if (m_projectManager && !m_projectManager->load()) {
        TOAST_ERROR(QStringLiteral("Projects could not be loaded: %1")
                        .arg(m_projectManager->lastError()), 5000);
    } else {
        validateAllProjectSources();
        refreshProjectClientList();
    }

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

// [Phase 12] Delegate to SettingsManager
bool MainWindow::getAutoUploadImportedMedia() const {
    return m_settingsManager ? m_settingsManager->getAutoUploadImportedMedia() : false;
}

bool MainWindow::useQuickCanvasRenderer() const {
    // Qt Quick is the single visible rendering contract. The old flag remains
    // readable for migration telemetry/settings compatibility, but selecting a
    // second renderer would reintroduce local/remote visual divergence.
    return true;
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

    if (!session.persistentClientId.isEmpty()) {
        const QSet<QString> toRemove = session.knownRemoteFileIds - currentFileIds;
        for (const QString& fileId : toRemove) {
            if (m_uploadManager) {
                m_uploadManager->requestAssetRemoval(
                    session.persistentClientId, fileId,
                    QStringLiteral("no_longer_referenced"));
            }
        }
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

void MainWindow::markAllSessionsOffline() {
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        session->lastClientInfo.setClientId(session->persistentClientId);
        session->lastClientInfo.setFromMemory(true);
        session->lastClientInfo.setOnline(false);
    }
}

QList<ClientInfo> MainWindow::buildDisplayClientList(const QList<ClientInfo>& connectedClients) {
    m_discoveredClients = connectedClients;
    if (!m_projectManager) {
        return ClientListBuilder::buildDisplayClientList(this, connectedClients);
    }

    const QList<ProjectClientEntry> entries =
        m_projectManager->mergeDiscoveredClients(connectedClients);
    QList<ClientInfo> result;
    result.reserve(entries.size());
    for (const ProjectClientEntry& entry : entries) {
        ClientInfo client = entry.client;
        client.setClientId(entry.deviceId);
        client.setOnline(entry.online);
        client.setFromMemory(entry.hasProject);
        client.setProjectId(entry.projectId);
        client.setHasProject(entry.hasProject);
        client.setRemoteSessionCloseAtMs(entry.remoteSessionCloseAtMs);
        client.setProjectDeleteAtMs(entry.projectDeleteAtMs);
        if (!entry.online) {
            client.setStatus(QStringLiteral("Offline"));
            client.setAvailabilityStatus(QStringLiteral("Offline"));
        } else {
            static const QSet<QString> allowedStatuses = {
                QStringLiteral("Available"),
                QStringLiteral("Connecting"),
                QStringLiteral("Connected"),
                QStringLiteral("Reconnecting"),
                QStringLiteral("Disconnecting"),
                QStringLiteral("In use"),
                QStringLiteral("Unavailable")
            };
            QString status = client.availabilityStatus().trimmed();
            if (status.compare(QStringLiteral("active"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Connected");
            } else if (status.compare(QStringLiteral("in_use"), Qt::CaseInsensitive) == 0
                       || status.compare(QStringLiteral("busy"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("In use");
            } else if (status.compare(QStringLiteral("opening"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Connecting");
            } else if (status.compare(QStringLiteral("grace"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Reconnecting");
            } else if (status.compare(QStringLiteral("terminating"), Qt::CaseInsensitive) == 0
                       || status.compare(QStringLiteral("cleanup_pending"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Disconnecting");
            } else if (status.compare(QStringLiteral("cleanup_error"), Qt::CaseInsensitive) == 0) {
                status = QStringLiteral("Unavailable");
            }
            if (!allowedStatuses.contains(status)) {
                status = QStringLiteral("Available");
            }
            client.setStatus(status);
            client.setAvailabilityStatus(status);
        }

        if (CanvasSession* session = m_sessionManager->findSession(entry.deviceId)) {
            if (entry.online) {
                m_sessionManager->updateSessionServerId(entry.deviceId, client.getId());
                session = m_sessionManager->findSession(entry.deviceId);
            }
            if (session) {
                session->lastClientInfo = client;
                session->remoteContentClearedOnDisconnect = !entry.online
                    ? session->remoteContentClearedOnDisconnect : false;
                if (session->canvas) {
                    session->canvas->setRemoteSceneTarget(
                        entry.deviceId, client.getMachineName());
                }
            }
        }
        result.append(client);
    }
    return result;
}

void MainWindow::refreshProjectClientList() {
    if (!m_clientListPage) {
        return;
    }
    m_clientListPage->updateClientList(buildDisplayClientList(m_discoveredClients));
}

QList<ProjectMediaReference> MainWindow::collectProjectMediaReferences(
    const QString& targetDeviceId, ICanvasHost* canvas) const {
    QList<ProjectMediaReference> references;
    if (!canvas) {
        return references;
    }

    QHash<QString, ProjectMediaReference> previousByMediaId;
    if (m_projectManager) {
        if (const ProjectRecord* project =
                m_projectManager->projectForTarget(targetDeviceId)) {
            for (const ProjectMediaReference& previous : project->mediaReferences) {
                previousByMediaId.insert(previous.mediaId, previous);
            }
        }
    }

    for (ResizableMediaBase* media : canvas->enumerateMediaItems()) {
        if (!media || media->isTextMedia() || media->sourcePath().isEmpty()) {
            continue;
        }
        const QString canonicalPath = canonicalExistingPath(media->sourcePath());
        if (canonicalPath.isEmpty()) {
            continue;
        }
        const QString identity = sourceIdentityForPath(canonicalPath);
        ProjectMediaReference reference = previousByMediaId.value(media->mediaId());
        reference.mediaId = media->mediaId();
        if (reference.canonicalSourcePath != canonicalPath
            || reference.sourceIdentity != identity
            || reference.sha256.isEmpty()) {
            reference.canonicalSourcePath = canonicalPath;
            reference.sourceIdentity = identity;
            reference.sha256 = sha256ForPath(canonicalPath);
            reference.assetId = reference.sha256;
        }
        reference.mediaType = media->isVideoMedia()
            ? QStringLiteral("video") : QStringLiteral("image");
        if (!reference.sha256.isEmpty()) {
            references.append(reference);
        }
    }
    return references;
}

void MainWindow::persistProjectCanvas(const QString& targetDeviceId) {
    if (!m_projectManager || targetDeviceId.isEmpty()) {
        return;
    }
    CanvasSession* session = m_sessionManager->findSession(targetDeviceId);
    if (!session || !session->canvas
        || !m_projectManager->hasProjectForTarget(targetDeviceId)) {
        return;
    }
    m_projectManager->updateCanvasState(
        targetDeviceId,
        session->canvas->serializeProjectState(),
        collectProjectMediaReferences(targetDeviceId, session->canvas));
}

void MainWindow::restoreProjectCanvas(CanvasSession& session) {
    if (!m_projectManager || !session.canvas
        || m_restoredProjectIds.contains(session.persistentClientId)) {
        return;
    }
    const ProjectRecord* stored =
        m_projectManager->projectForTarget(session.persistentClientId);
    if (!stored) {
        return;
    }

    QHash<QString, QString> validSources;
    QList<ProjectMediaReference> validReferences;
    QSet<QString> invalidMediaIds;
    for (const ProjectMediaReference& reference : stored->mediaReferences) {
        const QString canonicalPath = canonicalExistingPath(reference.canonicalSourcePath);
        const QString identity = sourceIdentityForPath(canonicalPath);
        if (canonicalPath.isEmpty()
            || canonicalPath != reference.canonicalSourcePath
            || identity.isEmpty()
            || identity != reference.sourceIdentity
            || sha256ForPath(canonicalPath) != reference.sha256) {
            invalidMediaIds.insert(reference.mediaId);
            continue;
        }
        validSources.insert(reference.mediaId, canonicalPath);
        validReferences.append(reference);
    }

    QJsonObject durableState = stored->canvasStateForRestore();
    QJsonArray filteredMedia;
    for (const QJsonValue& value : durableState.value(QStringLiteral("media")).toArray()) {
        const QJsonObject media = value.toObject();
        const QString type = media.value(QStringLiteral("type")).toString().toLower();
        const QString mediaId = media.value(QStringLiteral("mediaId")).toString();
        if (type == QLatin1String("text") || validSources.contains(mediaId)) {
            filteredMedia.append(media);
        } else {
            invalidMediaIds.insert(mediaId);
        }
    }
    durableState.insert(QStringLiteral("media"), filteredMedia);

    QStringList skipped;
    if (!session.canvas->restoreProjectState(durableState, validSources, &skipped)) {
        qWarning() << "Project canvas restoration failed for device"
                   << session.persistentClientId.left(12);
        return;
    }
    for (const QString& mediaId : skipped) {
        invalidMediaIds.insert(mediaId);
    }
    m_restoredProjectIds.insert(session.persistentClientId);

    if (!invalidMediaIds.isEmpty() || !skipped.isEmpty()) {
        QJsonArray finalMedia;
        for (const QJsonValue& value : filteredMedia) {
            const QString mediaId = value.toObject()
                                        .value(QStringLiteral("mediaId")).toString();
            if (!invalidMediaIds.contains(mediaId)) {
                finalMedia.append(value);
            }
        }
        durableState.insert(QStringLiteral("media"), finalMedia);
        m_projectManager->updateCanvasState(
            session.persistentClientId, durableState, validReferences);
        TOAST_WARNING(QStringLiteral("%1 missing or modified media item%2 removed from the project")
                          .arg(invalidMediaIds.size())
                          .arg(invalidMediaIds.size() == 1 ? QString() : QStringLiteral("s")),
                      5000);
    }
}

void MainWindow::removeInvalidMediaItems(
    const QList<ResizableMediaBase*>& mediaItems) {
    if (!m_sessionManager || mediaItems.isEmpty()) {
        return;
    }

    QSet<ResizableMediaBase*> requested;
    QSet<QString> affectedCanonicalPaths;
    QSet<QString> affectedMediaIds;
    for (ResizableMediaBase* media : mediaItems) {
        if (!media || media->mediaId().isEmpty()
            || media->mediaId().contains(QChar::Null)) {
            continue;
        }
        requested.insert(media);
        affectedMediaIds.insert(media->mediaId());
        QString sourcePath = m_fileWatcher
            ? m_fileWatcher->watchedFilePath(media) : QString();
        if (sourcePath.isEmpty()) {
            const QFileInfo info(media->sourcePath());
            sourcePath = info.canonicalFilePath();
            if (sourcePath.isEmpty()) {
                sourcePath = QDir::cleanPath(info.absoluteFilePath());
            }
        }
        if (!sourcePath.isEmpty()) {
            affectedCanonicalPaths.insert(sourcePath);
        }
    }
    if (requested.isEmpty()) {
        return;
    }

    QHash<QString, QList<ResizableMediaBase*>> itemsByProject;
    for (CanvasSession* session : m_sessionManager->getAllSessions()) {
        if (!session || !session->canvas) {
            continue;
        }
        for (ResizableMediaBase* candidate : session->canvas->enumerateMediaItems()) {
            if (!candidate || candidate->isTextMedia()) {
                continue;
            }
            QString candidatePath = m_fileWatcher
                ? m_fileWatcher->watchedFilePath(candidate) : QString();
            if (candidatePath.isEmpty()) {
                const QFileInfo candidateInfo(candidate->sourcePath());
                candidatePath = candidateInfo.canonicalFilePath();
                if (candidatePath.isEmpty()) {
                    candidatePath = QDir::cleanPath(candidateInfo.absoluteFilePath());
                }
            }
            if (requested.contains(candidate)
                || affectedCanonicalPaths.contains(candidatePath)) {
                itemsByProject[session->persistentClientId].append(candidate);
            }
        }
    }

    // Stop every affected graph before deleting the first item. The canvas
    // method also sends the remote stop when a remote run is active.
    for (auto it = itemsByProject.cbegin(); it != itemsByProject.cend(); ++it) {
        if (CanvasSession* session = m_sessionManager->findSession(it.key());
            session && session->canvas) {
            session->canvas->stopScenesForSourceInvalidation();
        }
    }

    int removedCount = 0;
    for (auto it = itemsByProject.cbegin(); it != itemsByProject.cend(); ++it) {
        CanvasSession* session = m_sessionManager->findSession(it.key());
        if (!session || !session->canvas) {
            continue;
        }
        for (ResizableMediaBase* media : it.value()) {
            if (m_fileWatcher) {
                m_fileWatcher->unwatchMediaItem(media);
            }
            session->canvas->deleteMediaItemCanonical(media);
            ++removedCount;
        }
        persistProjectCanvas(it.key());
    }

    // A source can occur in projects whose canvas has not been instantiated
    // in this process. Remove those durable occurrences as part of the same
    // canonical operation; no placeholder survives.
    if (m_projectManager) {
        const QList<ProjectRecord> projects = m_projectManager->projects();
        for (const ProjectRecord& project : projects) {
            QSet<QString> removeIds;
            QList<ProjectMediaReference> retainedReferences;
            for (const ProjectMediaReference& reference : project.mediaReferences) {
                if (affectedMediaIds.contains(reference.mediaId)
                    || affectedCanonicalPaths.contains(reference.canonicalSourcePath)) {
                    removeIds.insert(reference.mediaId);
                } else {
                    retainedReferences.append(reference);
                }
            }
            if (removeIds.isEmpty()) {
                continue;
            }
            QJsonObject state = project.canvasState;
            QJsonArray retainedMedia;
            for (const QJsonValue& value : state.value(QStringLiteral("media")).toArray()) {
                if (!removeIds.contains(
                        value.toObject().value(QStringLiteral("mediaId")).toString())) {
                    retainedMedia.append(value);
                }
            }
            state.insert(QStringLiteral("media"), retainedMedia);
            m_projectManager->updateCanvasState(
                project.targetDeviceId, state, retainedReferences);
        }
        m_projectManager->flush();
    }

    if (removedCount > 0) {
        TOAST_WARNING(
            QStringLiteral("%1 unavailable media item%2 removed from all projects")
                .arg(removedCount)
                .arg(removedCount == 1 ? QString() : QStringLiteral("s")),
            5000);
    }
}

void MainWindow::validateAllProjectSources() {
    if (!m_projectManager) {
        return;
    }

    int removedCount = 0;
    bool changed = false;
    const QList<ProjectRecord> projects = m_projectManager->projects();
    for (const ProjectRecord& project : projects) {
        QSet<QString> validMediaIds;
        QSet<QString> invalidMediaIds;
        QList<ProjectMediaReference> retainedReferences;
        for (const ProjectMediaReference& reference : project.mediaReferences) {
            const QString canonicalPath =
                canonicalExistingPath(reference.canonicalSourcePath);
            const bool valid = !canonicalPath.isEmpty()
                && canonicalPath == reference.canonicalSourcePath
                && sourceIdentityForPath(canonicalPath) == reference.sourceIdentity
                && sha256ForPath(canonicalPath) == reference.sha256;
            if (valid) {
                validMediaIds.insert(reference.mediaId);
                retainedReferences.append(reference);
            } else {
                invalidMediaIds.insert(reference.mediaId);
            }
        }

        QJsonObject state = project.canvasState;
        QJsonArray retainedMedia;
        for (const QJsonValue& value : state.value(QStringLiteral("media")).toArray()) {
            const QJsonObject media = value.toObject();
            const QString type = media.value(QStringLiteral("type")).toString().toLower();
            const QString mediaId = media.value(QStringLiteral("mediaId")).toString();
            if (type == QLatin1String("text") || validMediaIds.contains(mediaId)) {
                retainedMedia.append(media);
            } else {
                invalidMediaIds.insert(mediaId);
            }
        }

        if (invalidMediaIds.isEmpty()) {
            continue;
        }
        removedCount += invalidMediaIds.size();
        state.insert(QStringLiteral("media"), retainedMedia);
        m_projectManager->updateCanvasState(
            project.targetDeviceId, state, retainedReferences);
        changed = true;
    }

    if (changed) {
        m_projectManager->flush();
        TOAST_WARNING(
            QStringLiteral("%1 missing or modified media item%2 removed while restoring projects")
                .arg(removedCount)
                .arg(removedCount == 1 ? QString() : QStringLiteral("s")),
            5000);
    }
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
    updateHistoryVisibilityState();
}

void MainWindow::handleApplicationStateChanged(Qt::ApplicationState state) {
    if (m_windowEventHandler) {
        m_windowEventHandler->handleApplicationStateChanged(state);
    }
}

void MainWindow::handleNativeSystemSuspendedChanged(bool suspended) {
    if (m_windowEventHandler) {
        m_windowEventHandler->updateNativeSystemSuspendedState(suspended);
    }
}

void MainWindow::showScreenView(const ClientInfo& client) {
    if (!m_navigationManager) return;
    const QString targetDeviceId = client.clientId().trimmed();
    if (targetDeviceId.isEmpty()) {
        TOAST_ERROR(QStringLiteral("This client has no authenticated device identity"), 4000);
        return;
    }
    const bool explicitProjectEntry = !m_navigationManager->isOnScreenView()
        || m_activeSessionIdentity != targetDeviceId;
    if (explicitProjectEntry) {
        // A fresh user navigation is the only event that clears a prior
        // command-level rejection. Discovery changes never auto-queue access.
        m_remoteSessionOpenSuppressedTargets.remove(targetDeviceId);
    }
    const bool hadRuntimeCanvas =
        m_sessionManager && m_sessionManager->hasSession(targetDeviceId);
    bool hasDurableViewport = false;
    if (m_projectManager) {
        if (const ProjectRecord* existing =
                m_projectManager->projectForTarget(targetDeviceId)) {
            hasDurableViewport =
                existing->canvasState.value(QStringLiteral("viewport")).isObject();
        }
    }
    const bool preserveProjectViewport = hadRuntimeCanvas || hasDurableViewport;
    if (!m_activeSessionIdentity.isEmpty()
        && m_activeSessionIdentity != targetDeviceId
        && m_projectManager) {
        persistProjectCanvas(m_activeSessionIdentity);
        m_projectManager->setHidden(m_activeSessionIdentity);
    }

    if (m_projectManager) {
        if (!m_projectManager->hasProjectForTarget(targetDeviceId)) {
            if (m_projectManager->ensureProject(
                    ClientSnapshot::fromClientInfo(client, QDateTime::currentMSecsSinceEpoch()),
                    ProjectLifecycleState::Visible).isEmpty()) {
                TOAST_ERROR(QStringLiteral("The project could not be created"), 4000);
                return;
            }
        } else {
            if (client.isOnline()) {
                m_projectManager->updateClientSnapshot(
                    ClientSnapshot::fromClientInfo(client, QDateTime::currentMSecsSinceEpoch()));
            }
            if (!m_projectManager->setVisible(targetDeviceId)) {
                TOAST_WARNING(QStringLiteral("This project has expired"), 3500);
                refreshProjectClientList();
                return;
            }
        }
    }

    CanvasSession& session = ensureCanvasSession(client);
    restoreProjectCanvas(session);
    if (session.canvas) {
        // The authenticated device snapshot is authoritative for topology.
        // setScreens updates only screen backdrops; persisted media keep their
        // absolute scene coordinates even when monitors changed.
        session.canvas->setScreens(session.lastClientInfo.getScreens());
    }
    markCanvasLoadRequest(session.persistentClientId);
    const bool sessionHasActiveScreens = session.canvas && session.canvas->hasActiveScreens();
    const bool sessionHasStoredScreens = !session.lastClientInfo.getScreens().isEmpty();
    const bool hasRenderableCachedContent = sessionHasActiveScreens;
    const bool hasCachedContent = hasRenderableCachedContent || sessionHasStoredScreens;
    switchToCanvasSession(session.persistentClientId);
    m_activeRemoteClientId = session.persistentClientId;
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
        m_preserveViewportOnReconnect = preserveProjectViewport;
        m_navigationManager->showScreenView(effectiveClient, hasCachedContent);
        if (session.canvas && !preserveProjectViewport) {
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
    m_uploadManager->setTargetClientId(
        effectiveClient.isOnline() ? session.persistentClientId : QString());

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
            if (!preserveProjectViewport) {
                session.canvas->resetTransform();
                session.canvas->recenterWithMargin(53);
                session.canvas->requestDeferredInitialRecenter(53);
            }
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
    if (m_canvasViewPage) {
        m_canvasViewPage->setDisconnecting(false);
        m_canvasViewPage->setProjectActionsEnabled(effectiveClient.isOnline(), true);
    }
    setActiveProjectVisibleIfAppropriate();
    ensureRemoteSessionForClient(effectiveClient);
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
    
    const QString leavingTarget = m_activeSessionIdentity;
    if (!leavingTarget.isEmpty() && m_projectManager
        && m_navigationManager && m_navigationManager->isOnScreenView()) {
        persistProjectCanvas(leavingTarget);
        m_projectManager->setHidden(leavingTarget);
    }
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
    if (m_pageTitleLabel) {
        const int authenticatedCount = m_clientListPage
            ? m_clientListPage->authenticatedDeviceCount() : 0;
        const int liveCount = m_clientListPage
            ? m_clientListPage->liveSceneCount() : 0;
        m_pageTitleLabel->setText(
            QStringLiteral("Clients · %1 authenticated · %2 live")
                .arg(authenticatedCount)
                .arg(liveCount));
        m_pageTitleLabel->show();
    }
    if (m_backButton) m_backButton->hide();
    
    // Update button visibility for client list page
    if (m_responsiveLayoutManager) {
        m_responsiveLayoutManager->updateResponsiveButtonVisibility();
    }
}

void MainWindow::updateRemoteClientAvailability(const QString& targetDeviceId,
                                                const QString& status) {
    if (targetDeviceId.isEmpty() || status.isEmpty()) {
        return;
    }
    for (ClientInfo& client : m_discoveredClients) {
        if (client.clientId() != targetDeviceId) {
            continue;
        }
        client.setStatus(status);
        client.setAvailabilityStatus(status);
        if (status == QLatin1String("Offline")) {
            client.setOnline(false);
        }
    }
    if (m_sessionManager) {
        if (CanvasSession* session = m_sessionManager->findSession(targetDeviceId)) {
            session->lastClientInfo.setStatus(status);
            session->lastClientInfo.setAvailabilityStatus(status);
            if (status == QLatin1String("Offline")) {
                session->lastClientInfo.setOnline(false);
            }
        }
    }
    refreshProjectClientList();
}

void MainWindow::ensureRemoteSessionForClient(const ClientInfo& client) {
    if (m_cleanShutdownPrepared || !m_webSocketClient || !m_canvasViewPage) {
        return;
    }
    const QString targetDeviceId = client.clientId().trimmed();
    if (targetDeviceId.isEmpty() || targetDeviceId == m_webSocketClient->deviceId()) {
        return;
    }

    RemoteSessionCoordinator* coordinator =
        m_webSocketClient->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = coordinator
        ? coordinator->outgoingForPeer(targetDeviceId)
        : RemoteSessionCoordinator::Binding();
    if (!binding.remoteSessionId.isEmpty()) {
        QString status = QStringLiteral("Disconnecting");
        if (binding.phase == QLatin1String("Active")) {
            status = QStringLiteral("Connected");
        } else if (binding.phase == QLatin1String("Opening")) {
            status = QStringLiteral("Connecting");
        } else if (binding.phase == QLatin1String("Grace")) {
            status = QStringLiteral("Reconnecting");
        } else if (binding.phase == QLatin1String("Closed")) {
            status = client.isOnline() ? QStringLiteral("Available")
                                       : QStringLiteral("Offline");
        }
        updateRemoteClientAvailability(targetDeviceId, status);
        const bool commandReady = binding.phase == QLatin1String("Active")
            && !m_locallyTerminatingRemoteSessions.contains(binding.remoteSessionId);
        if (m_activeSessionIdentity == targetDeviceId) {
            m_remoteClientConnected = binding.phase == QLatin1String("Active")
                || binding.phase == QLatin1String("Grace");
            setRemoteConnectionStatus(commandReady ? QStringLiteral("CONNECTED")
                                                   : status.toUpper(), false);
            m_canvasViewPage->setProjectActionsEnabled(commandReady, true);
            if (m_screenCanvas) m_screenCanvas->setOverlayActionsEnabled(commandReady);
            if (m_uploadManager) {
                m_uploadManager->setTargetClientId(
                    commandReady ? targetDeviceId : QString());
            }
        }
        return;
    }

    // If setVisible() just expired an old hidden session, this is the one
    // permitted replacement attempt. Once consumed it is never auto-retried.
    m_reopenAfterSessionCloseTargets.remove(targetDeviceId);
    if (m_remoteSessionOpenPendingTargets.contains(targetDeviceId)) {
        updateRemoteClientAvailability(targetDeviceId, QStringLiteral("Connecting"));
        if (m_activeSessionIdentity == targetDeviceId && m_uploadManager) {
            m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    if (m_remoteSessionOpenSuppressedTargets.contains(targetDeviceId)) {
        const QString status = client.isOnline()
            ? client.availabilityBadgeText() : QStringLiteral("Offline");
        updateRemoteClientAvailability(targetDeviceId, status);
        if (m_activeSessionIdentity == targetDeviceId) {
            setRemoteConnectionStatus(status.toUpper(), false);
            m_canvasViewPage->setProjectActionsEnabled(false, true);
            if (m_screenCanvas) m_screenCanvas->setOverlayActionsEnabled(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }

    const QString badge = client.availabilityBadgeText();
    const bool canOpen = client.isOnline()
        && badge != QLatin1String("In use")
        && badge != QLatin1String("Unavailable")
        && badge != QLatin1String("Disconnecting")
        && m_webSocketClient->isConnected();
    if (!canOpen) {
        const QString status = client.isOnline() ? badge : QStringLiteral("Offline");
        updateRemoteClientAvailability(targetDeviceId, status);
        m_remoteSessionOpenSuppressedTargets.insert(targetDeviceId);
        if (m_activeSessionIdentity == targetDeviceId) {
            setRemoteConnectionStatus(status.toUpper(), false);
            m_canvasViewPage->setProjectActionsEnabled(false, true);
            if (m_screenCanvas) m_screenCanvas->setOverlayActionsEnabled(false);
            if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
        }
        return;
    }

    QString requestId;
    if (!m_webSocketClient->openRemoteSession(targetDeviceId, &requestId)) {
        updateRemoteClientAvailability(targetDeviceId, QStringLiteral("Unavailable"));
        m_canvasViewPage->setProjectActionsEnabled(false, true);
        if (m_activeSessionIdentity == targetDeviceId && m_uploadManager) {
            m_uploadManager->setTargetClientId(QString());
        }
        return;
    }
    m_remoteSessionOpenPendingTargets.insert(targetDeviceId);
    m_remoteSessionOpenTargetByRequestId.insert(requestId, targetDeviceId);
    updateRemoteClientAvailability(targetDeviceId, QStringLiteral("Connecting"));
    if (m_activeSessionIdentity == targetDeviceId) {
        m_remoteClientConnected = false;
        setRemoteConnectionStatus(QStringLiteral("CONNECTING..."), false);
        m_canvasViewPage->setProjectActionsEnabled(false, true);
        if (m_screenCanvas) m_screenCanvas->setOverlayActionsEnabled(false);
        if (m_uploadManager) m_uploadManager->setTargetClientId(QString());
    }
}

void MainWindow::handleRemoteSessionReady(const QJsonObject& envelope,
                                          bool resumed) {
    Q_UNUSED(resumed);
    if (!m_webSocketClient) return;
    const QString ownerDeviceId = envelope.value(QStringLiteral("ownerDeviceId")).toString();
    const QString targetDeviceId = envelope.value(QStringLiteral("targetDeviceId")).toString();
    const QString localDeviceId = m_webSocketClient->deviceId();
    const QString peerDeviceId = ownerDeviceId == localDeviceId
        ? targetDeviceId
        : (targetDeviceId == localDeviceId ? ownerDeviceId : QString());
    if (peerDeviceId.isEmpty()) return;

    // A response to an in-flight open (or a new incoming session) may cross
    // the quit request. Never let it resurrect command-capable state while the
    // transport is being drained: include it in the same idempotent close set.
    if (m_cleanShutdownPrepared) {
        const QString remoteSessionId =
            envelope.value(QStringLiteral("remoteSessionId")).toString();
        if (!remoteSessionId.isEmpty()) {
            m_cleanShutdownPendingSessionIds.insert(remoteSessionId);
            m_locallyTerminatingRemoteSessions.insert(remoteSessionId);
            m_webSocketClient->closeRemoteSession(
                remoteSessionId, nullptr, QStringLiteral("clean_shutdown"));
            if (targetDeviceId == localDeviceId && g_remoteSceneController) {
                g_remoteSceneController->teardownRemoteSession(remoteSessionId);
            }
        }
        return;
    }

    const QString requestId = envelope.value(QStringLiteral("requestId")).toString();
    if (!requestId.isEmpty()) m_remoteSessionOpenTargetByRequestId.remove(requestId);
    m_remoteSessionOpenPendingTargets.remove(peerDeviceId);
    m_remoteSessionOpenSuppressedTargets.remove(peerDeviceId);
    m_locallyTerminatingRemoteSessions.remove(
        envelope.value(QStringLiteral("remoteSessionId")).toString());
    if (ownerDeviceId != localDeviceId) {
        return; // Incoming sessions do not create a local Project.
    }

    updateRemoteClientAvailability(peerDeviceId, QStringLiteral("Connected"));
    if (m_activeSessionIdentity == peerDeviceId) {
        m_remoteClientConnected = true;
        setRemoteConnectionStatus(QStringLiteral("CONNECTED"), false);
        if (m_canvasViewPage) m_canvasViewPage->setProjectActionsEnabled(true, true);
        if (m_screenCanvas) m_screenCanvas->setOverlayActionsEnabled(true);
        if (m_uploadManager) m_uploadManager->setTargetClientId(peerDeviceId);
    }
}

void MainWindow::handleRemoteSessionLeaseState(const QJsonObject& envelope) {
    if (!m_webSocketClient) return;
    const QString ownerDeviceId = envelope.value(QStringLiteral("ownerDeviceId")).toString();
    if (ownerDeviceId != m_webSocketClient->deviceId()) return;
    const QString targetDeviceId = envelope.value(QStringLiteral("targetDeviceId")).toString();
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString phase = envelope.value(QStringLiteral("phase")).toString();
    const QString state = envelope.value(QStringLiteral("state")).toString(phase);
    QString status;
    if (phase == QLatin1String("Grace") || state == QLatin1String("Grace")
        || state == QLatin1String("Degraded")
        || envelope.value(QStringLiteral("degraded")).toBool()) {
        status = QStringLiteral("Reconnecting");
    } else if (phase == QLatin1String("Active") && state == QLatin1String("Active")) {
        status = QStringLiteral("Connected");
    } else if (phase == QLatin1String("Terminating")
               || phase == QLatin1String("CleanupPending")) {
        status = QStringLiteral("Disconnecting");
    }
    if (status.isEmpty()) return;
    updateRemoteClientAvailability(targetDeviceId, status);
    const bool commandReady = status == QLatin1String("Connected")
        && !m_locallyTerminatingRemoteSessions.contains(remoteSessionId);
    if (m_activeSessionIdentity == targetDeviceId) {
        m_remoteClientConnected = phase == QLatin1String("Active")
            || phase == QLatin1String("Grace");
        setRemoteConnectionStatus(status.toUpper(), false);
        if (m_canvasViewPage) m_canvasViewPage->setProjectActionsEnabled(commandReady, true);
        if (m_screenCanvas) m_screenCanvas->setOverlayActionsEnabled(commandReady);
    }
}

void MainWindow::handleRemoteSessionTerminating(const QJsonObject& envelope) {
    if (!m_webSocketClient) return;
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString teardownId = envelope.value(QStringLiteral("teardownId")).toString();
    const QString ownerDeviceId = envelope.value(QStringLiteral("ownerDeviceId")).toString();
    const QString targetDeviceId = envelope.value(QStringLiteral("targetDeviceId")).toString();
    quint64 generation = 0;
    if (remoteSessionId.isEmpty() || teardownId.isEmpty()
        || !readSafePositiveJsonInteger(
            envelope.value(QStringLiteral("generation")), &generation)) {
        return;
    }
    m_locallyTerminatingRemoteSessions.insert(remoteSessionId);

    if (ownerDeviceId == m_webSocketClient->deviceId()) {
        if (m_activeSessionIdentity == targetDeviceId) m_remoteClientConnected = false;
        updateRemoteClientAvailability(targetDeviceId, QStringLiteral("Disconnecting"));
        clearRemoteSessionRuntimeState(targetDeviceId, false);
        return;
    }
    if (targetDeviceId != m_webSocketClient->deviceId()) return;

    // Duplicate terminal envelopes replay the result already being built or
    // acknowledged. They must never enter renderer destruction or quarantine
    // a second time.
    const auto acknowledged = m_pendingTeardownAcks.constFind(remoteSessionId);
    if (acknowledged != m_pendingTeardownAcks.cend()) {
        if (acknowledged->teardownId == teardownId) retryPendingTeardownAcks();
        return;
    }
    const auto pending = m_pendingRendererTeardowns.constFind(remoteSessionId);
    if (pending != m_pendingRendererTeardowns.cend()) return;

    // Target-side commit order is enforced asynchronously: renderer objects
    // are stopped/detached now, but cache quarantine is forbidden until their
    // actual QObject/native-window destruction completes.
    PendingRendererTeardown rendererTeardown;
    rendererTeardown.ownerDeviceId = ownerDeviceId;
    rendererTeardown.teardownId = teardownId;
    rendererTeardown.generation = generation;
    m_pendingRendererTeardowns.insert(remoteSessionId, rendererTeardown);
    const bool accepted = g_remoteSceneController
        && g_remoteSceneController->teardownRemoteSession(remoteSessionId);
    if (!accepted) {
        handleRemoteRendererTeardownSettled(remoteSessionId, false);
    }
}

void MainWindow::beginTerminalIncomingCacheCleanup(const QString& reasonCode)
{
    if (m_cleanShutdownPrepared || !m_uploadManager || !m_webSocketClient) {
        return;
    }

    // Stop the receiver writer/timer and fail-close advertisement before any
    // asynchronous renderer destruction starts. This call does not touch the
    // cache namespace or FileManager mappings.
    m_uploadManager->beginTerminalIncomingCleanup(reasonCode);

    if (!m_terminalIncomingCleanupActive) {
        m_terminalIncomingCleanupActive = true;
        m_terminalIncomingCacheTeardownStarted = false;
    }
    if (!reasonCode.trimmed().isEmpty()) {
        m_terminalIncomingCleanupReason = reasonCode.trimmed();
    }

    const QString localDeviceId = m_webSocketClient->deviceId();
    RemoteSessionCoordinator* coordinator =
        m_webSocketClient->remoteSessionCoordinator();
    const QList<RemoteSessionCoordinator::Binding> bindings = coordinator
        ? coordinator->all() : QList<RemoteSessionCoordinator::Binding>();

    // This slot is reached synchronously from leaseExpired/serverRestarted.
    // WebSocketClient deliberately clears the coordinator only after signal
    // delivery, so capture every incoming id now; a queued scan would be too
    // late and could quarantine media still held by the renderer.
    for (const RemoteSessionCoordinator::Binding& binding : bindings) {
        if (binding.targetDeviceId != localDeviceId
            || binding.remoteSessionId.isEmpty()
            || m_terminalRendererPendingSessionIds.contains(
                binding.remoteSessionId)) {
            continue;
        }
        m_terminalRendererPendingSessionIds.insert(binding.remoteSessionId);
        if (!g_remoteSceneController
            || !g_remoteSceneController->teardownRemoteSession(
                binding.remoteSessionId)) {
            // Keep the id pending and the receiver unadvertised. Clearing it
            // here would permit cache quarantine without renderer settlement.
            qCritical() << "Terminal renderer teardown could not start for"
                        << binding.remoteSessionId;
        }
    }
    finishTerminalIncomingCacheCleanupIfReady();
}

void MainWindow::finishTerminalIncomingCacheCleanupIfReady()
{
    if (!m_terminalIncomingCleanupActive
        || m_terminalIncomingCacheTeardownStarted
        || !m_terminalRendererPendingSessionIds.isEmpty()
        || !m_uploadManager) {
        return;
    }

    m_terminalIncomingCacheTeardownStarted = true;
    const UploadManager::BulkTeardownResult cleanup =
        m_uploadManager->completeTerminalIncomingCleanup(
            m_terminalIncomingCleanupReason);
    if (!cleanup.allLogicallyCommitted()) {
        qCritical() << "Terminal incoming cache cleanup remains pending:"
                    << cleanup.errorCode
                    << "failed scopes" << cleanup.cleanupErrorScopes;
    }
    m_terminalIncomingCleanupActive = false;
}

void MainWindow::handleRemoteRendererTeardownSettled(
    const QString& remoteSessionId,
    bool sceneStopped)
{
    if (remoteSessionId.isEmpty()) return;
    const bool terminalTransportBarrier =
        m_terminalRendererPendingSessionIds.remove(remoteSessionId) > 0;
    const bool cleanShutdownBarrier =
        m_cleanShutdownRendererPendingSessionIds.remove(remoteSessionId) > 0;
    const auto pending = m_pendingRendererTeardowns.find(remoteSessionId);
    if (pending == m_pendingRendererTeardowns.end()) {
        if (terminalTransportBarrier) {
            finishTerminalIncomingCacheCleanupIfReady();
        }
        if (cleanShutdownBarrier) {
            finishCleanShutdownIncomingCacheTeardownIfReady();
        }
        return;
    }
    const PendingRendererTeardown teardown = pending.value();
    m_pendingRendererTeardowns.erase(pending);

    RemoteCacheStore::CommitResult cacheResult;
    bool uploadsAborted = false;
    int removedFileCount = 0;
    if (sceneStopped && m_uploadManager) {
        cacheResult = m_uploadManager->teardownRemoteSession(
            teardown.ownerDeviceId, remoteSessionId, teardown.generation,
            teardown.teardownId);
        uploadsAborted = true;
        removedFileCount = m_uploadManager->lastTeardownRemovedFileCount();
    } else {
        cacheResult.outcome = RemoteCacheStore::CommitOutcome::CleanupError;
        cacheResult.teardownId = teardown.teardownId;
        cacheResult.errorCode = sceneStopped
            ? QStringLiteral("upload_manager_unavailable")
            : QStringLiteral("renderer_teardown_failed");
    }

    PendingTeardownAck ack;
    ack.teardownId = teardown.teardownId;
    ack.sceneStopped = sceneStopped;
    ack.uploadsAborted = uploadsAborted;
    ack.cacheQuarantined = cacheResult.acknowledgementSafe();
    ack.removedFileCount = removedFileCount;
    ack.errorCode = cacheResult.errorCode;
    ack.quarantinedBytes = cacheResult.quarantinedBytes;
    m_pendingTeardownAcks.insert(remoteSessionId, ack);
    retryPendingTeardownAcks();

    if (!ack.sceneStopped || !ack.uploadsAborted || !ack.cacheQuarantined) {
        if (m_toastSystem) {
            NotificationRequest notification;
            notification.severity = NotificationSeverity::Error;
            notification.category = QStringLiteral("Remote session cleanup");
            notification.message = QStringLiteral(
                "Remote session cleanup could not be committed; this device remains unavailable until cleanup succeeds.");
            notification.remoteSessionId = remoteSessionId;
            notification.correlationId =
                NotificationCorrelation::teardown(teardown.teardownId);
            notification.terminal = true;
            m_toastSystem->publishNotification(notification);
        }
    }
    if (cleanShutdownBarrier) {
        finishCleanShutdownIncomingCacheTeardownIfReady();
    }
    if (terminalTransportBarrier) {
        finishTerminalIncomingCacheCleanupIfReady();
    }
}

void MainWindow::retryPendingTeardownAcks() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) return;
    for (auto it = m_pendingTeardownAcks.cbegin();
         it != m_pendingTeardownAcks.cend(); ++it) {
        const PendingTeardownAck& ack = it.value();
        m_webSocketClient->acknowledgeRemoteSessionTeardown(
            it.key(), ack.teardownId, ack.sceneStopped, ack.uploadsAborted,
            ack.cacheQuarantined, ack.removedFileCount, ack.errorCode,
            ack.quarantinedBytes);
    }
}

void MainWindow::clearRemoteSessionRuntimeState(const QString& targetDeviceId,
                                                bool connectionLost) {
    CanvasSession* session = m_sessionManager
        ? m_sessionManager->findSession(targetDeviceId) : nullptr;
    if (!session) return;
    if (session->canvas) {
        if (connectionLost) session->canvas->handleRemoteConnectionLost();
        else session->canvas->stopScenesForSourceInvalidation();
    }
    session->knownRemoteFileIds.clear();
    session->expectedIdeaFileIds.clear();
    session->remoteContentClearedOnDisconnect = true;
    if (session->canvas) {
        for (ResizableMediaBase* media : session->canvas->enumerateMediaItems()) {
            if (media) media->setUploadNotUploaded();
        }
    }
    clearUploadTracking(*session);
    if (m_activeSessionIdentity == targetDeviceId && m_uploadManager) {
        // RemoteSession terminal envelopes invalidate only their own upload.
        // onConnectionLost() is transport-wide and would suspend unrelated
        // concurrent uploads to other devices indefinitely.
        m_uploadManager->setTargetClientId(QString());
    }
}

void MainWindow::handleRemoteSessionClosed(const QJsonObject& envelope) {
    if (!m_webSocketClient) return;
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    if (m_cleanShutdownPrepared) {
        m_cleanShutdownPendingSessionIds.remove(remoteSessionId);
        // Defer final disconnect until every slot handling this envelope has
        // returned, in particular the target-side teardown/ACK handlers.
        QTimer::singleShot(0, this, &MainWindow::maybeFinishCleanShutdown);
    }
    const QString ownerDeviceId = envelope.value(QStringLiteral("ownerDeviceId")).toString();
    const QString targetDeviceId = envelope.value(QStringLiteral("targetDeviceId")).toString();
    m_pendingRendererTeardowns.remove(remoteSessionId);
    m_cleanShutdownRendererPendingSessionIds.remove(remoteSessionId);
    m_pendingTeardownAcks.remove(remoteSessionId);
    m_locallyTerminatingRemoteSessions.remove(remoteSessionId);
    if (ownerDeviceId != m_webSocketClient->deviceId()) return;

    const bool explicitlyReopening =
        m_reopenAfterSessionCloseTargets.contains(targetDeviceId);
    if (!explicitlyReopening) {
        // A terminal lease/peer close is never converted into an implicit new
        // session when discovery later reports the device again.
        m_remoteSessionOpenSuppressedTargets.insert(targetDeviceId);
    }
    clearRemoteSessionRuntimeState(targetDeviceId, false);
    if (m_activeSessionIdentity == targetDeviceId) m_remoteClientConnected = false;
    bool online = false;
    ClientInfo current;
    for (const ClientInfo& client : m_discoveredClients) {
        if (client.clientId() == targetDeviceId) {
            current = client;
            online = client.isOnline();
            break;
        }
    }
    if (online) {
        current.setStatus(QStringLiteral("Available"));
        current.setAvailabilityStatus(QStringLiteral("Available"));
    }
    updateRemoteClientAvailability(
        targetDeviceId, online ? QStringLiteral("Available")
                               : QStringLiteral("Offline"));

    if (m_disconnectPendingTargets.remove(targetDeviceId)) {
        if (m_activeSessionIdentity == targetDeviceId
            && m_navigationManager && m_navigationManager->isOnScreenView()) {
            showClientListView();
        }
        if (m_canvasViewPage) m_canvasViewPage->setDisconnecting(false);
    }
    if (m_deleteAfterSessionCloseTargets.contains(targetDeviceId)) {
        finishDeferredProjectDeletion(targetDeviceId);
        return;
    }
    if (explicitlyReopening
        && online && m_activeSessionIdentity == targetDeviceId
        && m_navigationManager && m_navigationManager->isOnScreenView()) {
        ensureRemoteSessionForClient(current);
    }
}

void MainWindow::handleRemoteSessionError(const QJsonObject& envelope) {
    if (m_cleanShutdownPrepared) {
        // A duplicate/late close rejection cannot change a terminal local
        // shutdown. Keep draining any other correlated sessions until timeout.
        return;
    }
    const QString code = envelope.value(QStringLiteral("code")).toString();
    const QString requestId = envelope.value(QStringLiteral("requestId")).toString();
    QString targetDeviceId =
        envelope.value(QStringLiteral("targetDeviceId")).toString();
    if (targetDeviceId.isEmpty() && !requestId.isEmpty()) {
        targetDeviceId = m_remoteSessionOpenTargetByRequestId.value(requestId);
    }
    if (targetDeviceId.isEmpty()
        && m_remoteSessionOpenPendingTargets.size() == 1) {
        targetDeviceId = *m_remoteSessionOpenPendingTargets.cbegin();
    }
    if (!requestId.isEmpty()) m_remoteSessionOpenTargetByRequestId.remove(requestId);
    if (!targetDeviceId.isEmpty()) {
        m_remoteSessionOpenPendingTargets.remove(targetDeviceId);
        m_remoteSessionOpenSuppressedTargets.insert(targetDeviceId);
        m_reopenAfterSessionCloseTargets.remove(targetDeviceId);
    }

    QString status = QStringLiteral("Unavailable");
    if (code == QLatin1String("target_in_use")) status = QStringLiteral("In use");
    else if (code == QLatin1String("target_offline")) status = QStringLiteral("Offline");
    else if (code == QLatin1String("cleanup_not_committed")) status = QStringLiteral("Unavailable");
    else if (!targetDeviceId.isEmpty()) status = QStringLiteral("Available");
    if (!targetDeviceId.isEmpty()) {
        updateRemoteClientAvailability(targetDeviceId, status);
        if (m_activeSessionIdentity == targetDeviceId && m_canvasViewPage) {
            setRemoteConnectionStatus(status.toUpper(), false);
            m_canvasViewPage->setProjectActionsEnabled(false, true);
            if (m_screenCanvas) m_screenCanvas->setOverlayActionsEnabled(false);
        }
    }
    if (m_toastSystem) {
        NotificationRequest notification;
        notification.severity = NotificationSeverity::Warning;
        notification.category = QStringLiteral("Remote session");
        notification.message = code == QLatin1String("target_in_use")
            ? QStringLiteral("This client is already controlled by another device.")
            : envelope.value(QStringLiteral("message"))
                  .toString(QStringLiteral("The remote session command was rejected."));
        notification.correlationId =
            envelope.value(QStringLiteral("messageId")).toString();
        m_toastSystem->publishNotification(notification);
    }
}

void MainWindow::terminateProjectRemoteSession(const QString& targetDeviceId,
                                               bool attemptRemote) {
    CanvasSession* session = m_sessionManager
        ? m_sessionManager->findSession(targetDeviceId) : nullptr;
    if (session && session->canvas) {
        if (attemptRemote) session->canvas->stopScenesForSourceInvalidation();
        else session->canvas->handleRemoteConnectionLost();
    }

    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const RemoteSessionCoordinator::Binding binding = coordinator
        ? coordinator->outgoingForPeer(targetDeviceId)
        : RemoteSessionCoordinator::Binding();
    if (attemptRemote && m_webSocketClient
        && !binding.remoteSessionId.isEmpty()
        && binding.ownerDeviceId == m_webSocketClient->deviceId()
        && binding.phase != QLatin1String("Terminating")
        && binding.phase != QLatin1String("CleanupPending")) {
        m_locallyTerminatingRemoteSessions.insert(binding.remoteSessionId);
        updateRemoteClientAvailability(targetDeviceId, QStringLiteral("Disconnecting"));
        if (m_uploadManager && m_uploadManager->canRequestCancel()) {
            m_uploadManager->requestCancel();
        }
        if (m_webSocketClient->closeRemoteSession(binding.remoteSessionId)) {
            return;
        }
    }

    // There is no remote transaction to wait for (or the lease is terminal).
    // Only transient remote state is discarded; Project and canvas content stay.
    clearRemoteSessionRuntimeState(targetDeviceId, !attemptRemote);
}

void MainWindow::removeRuntimeCanvasSession(const QString& targetDeviceId) {
    if (!m_sessionManager) {
        return;
    }
    CanvasSession* session = m_sessionManager->findSession(targetDeviceId);
    if (!session) {
        m_restoredProjectIds.remove(targetDeviceId);
        return;
    }

    ICanvasHost* canvas = session->canvas;
    QWidget* canvasWidget = canvas ? canvas->asWidget() : nullptr;
    const QString ideaId = session->canvasSessionId;
    if (canvas) {
        for (ResizableMediaBase* media : canvas->enumerateMediaItems()) {
            if (m_fileWatcher && media) {
                m_fileWatcher->unwatchMediaItem(media);
            }
        }
    }
    clearUploadTracking(*session);
    if (m_fileManager && !ideaId.isEmpty()) {
        m_fileManager->removeIdeaAssociations(ideaId);
    }
    if (m_canvasViewPage && m_canvasViewPage->getCanvasHostStack() && canvasWidget) {
        m_canvasViewPage->getCanvasHostStack()->removeWidget(canvasWidget);
    }
    if (m_activeSessionIdentity == targetDeviceId) {
        m_activeSessionIdentity.clear();
        m_activeRemoteClientId.clear();
        m_screenCanvas = nullptr;
        m_uploadButton = nullptr;
    }
    m_sessionManager->deleteSession(targetDeviceId);
    if (canvasWidget) {
        canvasWidget->deleteLater();
    }
    m_restoredProjectIds.remove(targetDeviceId);
}

void MainWindow::onDisconnectProjectRequested() {
    const QString targetDeviceId = m_activeSessionIdentity;
    if (targetDeviceId.isEmpty()) {
        return;
    }
    persistProjectCanvas(targetDeviceId);
    if (m_canvasViewPage) {
        m_canvasViewPage->setDisconnecting(true);
        m_canvasViewPage->setProjectActionsEnabled(false, false);
    }
    m_disconnectPendingTargets.insert(targetDeviceId);
    terminateProjectRemoteSession(targetDeviceId, true);

    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    if (!coordinator
        || coordinator->outgoingForPeer(targetDeviceId).remoteSessionId.isEmpty()) {
        m_disconnectPendingTargets.remove(targetDeviceId);
        showClientListView();
        if (m_canvasViewPage) m_canvasViewPage->setDisconnecting(false);
        return;
    }

    // The v2 close transaction can finish earlier through its ACK. Its UI
    // deadline comes from the server policy announced at authentication, so
    // the client never carries a second protocol timeout constant.
    const int disconnectDeadlineMs = m_webSocketClient->serverPolicy()
        .value(QStringLiteral("leaseTimeoutMs")).toInt();
    QTimer::singleShot(std::max(0, disconnectDeadlineMs), this,
                       [this, targetDeviceId]() {
        if (!m_disconnectPendingTargets.remove(targetDeviceId)) return;
        clearRemoteSessionRuntimeState(targetDeviceId, false);
        if (m_activeSessionIdentity == targetDeviceId
            && m_navigationManager && m_navigationManager->isOnScreenView()) {
            showClientListView();
        }
        if (m_canvasViewPage) {
            m_canvasViewPage->setDisconnecting(false);
        }
    });
}

void MainWindow::onDeleteProjectRequested() {
    const QString targetDeviceId = m_activeSessionIdentity;
    if (!m_projectManager || targetDeviceId.isEmpty()) {
        return;
    }
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this,
        QStringLiteral("Delete project?"),
        QStringLiteral("Delete this local project and its canvas?\n\n"
                       "Source files will never be deleted."),
        QMessageBox::Cancel | QMessageBox::Yes,
        QMessageBox::Cancel);
    if (choice != QMessageBox::Yes) {
        return;
    }
    persistProjectCanvas(targetDeviceId);
    showClientListView();
    m_deleteAfterSessionCloseTargets.insert(targetDeviceId);
    terminateProjectRemoteSession(targetDeviceId, true);

    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    if (!coordinator
        || coordinator->outgoingForPeer(targetDeviceId).remoteSessionId.isEmpty()) {
        finishDeferredProjectDeletion(targetDeviceId);
        return;
    }
    const int deleteDeadlineMs = m_webSocketClient->serverPolicy()
        .value(QStringLiteral("leaseTimeoutMs")).toInt();
    QTimer::singleShot(std::max(0, deleteDeadlineMs), this,
                       [this, targetDeviceId]() {
        finishDeferredProjectDeletion(targetDeviceId);
    });
}

void MainWindow::finishDeferredProjectDeletion(const QString& targetDeviceId) {
    if (!m_deleteAfterSessionCloseTargets.remove(targetDeviceId)
        || !m_projectManager
        || !m_projectManager->hasProjectForTarget(targetDeviceId)) {
        return;
    }
    if (!m_projectManager->deleteProject(targetDeviceId)) {
        TOAST_ERROR(QStringLiteral("The project could not be deleted"), 4000);
    }
}

void MainWindow::setActiveProjectVisibleIfAppropriate() {
    if (!m_projectManager || m_activeSessionIdentity.isEmpty()) {
        return;
    }
    const bool canvasShown = m_navigationManager
        && m_navigationManager->isOnScreenView();
    const bool visible = canvasShown && isVisible() && !isMinimized()
        && !m_applicationSuspended;
    if (visible) {
        m_projectManager->setVisible(m_activeSessionIdentity);
    } else {
        persistProjectCanvas(m_activeSessionIdentity);
        m_projectManager->setHidden(m_activeSessionIdentity);
    }
}

void MainWindow::setApplicationSuspended(bool suspended) {
    if (m_applicationSuspended == suspended) {
        updateHistoryVisibilityState();
        return;
    }
    m_applicationSuspended = suspended;
    setActiveProjectVisibleIfAppropriate();
    updateHistoryVisibilityState();
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

void MainWindow::showHistoryPage() {
    if (!m_stackedWidget || !m_historyPage) return;

    // Leaving a canvas uses the normal navigation path so watches and transient
    // canvas UI are released consistently before the global page is shown.
    if (m_navigationManager && m_navigationManager->isOnScreenView()) {
        showClientListView();
    }

    m_stackedWidget->setCurrentWidget(m_historyPage);
    if (m_pageTitleLabel) {
        m_pageTitleLabel->setText(QStringLiteral("Notification History"));
        m_pageTitleLabel->show();
    }
    if (m_backButton) m_backButton->show();
    if (m_remoteClientInfoWrapper) m_remoteClientInfoWrapper->hide();
    if (QWidget* container = m_remoteClientInfoManager
            ? m_remoteClientInfoManager->getContainer() : nullptr) {
        container->hide();
    }
    removeVolumeIndicatorFromLayout();
    if (m_responsiveLayoutManager) {
        m_responsiveLayoutManager->updateResponsiveButtonVisibility();
    }
}

void MainWindow::updateHistoryUnreadBadge(int unreadCount) {
    if (!m_historyUnreadBadge || !m_historyButton) return;

    const int normalizedCount = qMax(0, unreadCount);
    m_historyUnreadBadge->setText(normalizedCount > 99
                                      ? QStringLiteral("99+")
                                      : QString::number(normalizedCount));
    m_historyUnreadBadge->setVisible(normalizedCount > 0);
    m_historyButton->setAccessibleDescription(
        normalizedCount == 0
            ? QStringLiteral("No unread notifications")
            : QStringLiteral("%1 unread notification%2")
                  .arg(normalizedCount)
                  .arg(normalizedCount == 1 ? QString() : QStringLiteral("s")));
    m_historyButton->setToolTip(
        normalizedCount == 0
            ? QStringLiteral("Open notification history")
            : QStringLiteral("Open notification history (%1 unread)").arg(normalizedCount));
}

void MainWindow::updateHistoryVisibilityState() {
    if (!m_toastSystem || !m_toastSystem->notificationCenter()) return;

    // Selecting History is insufficient while the window is not actually
    // observable. New events stay unread during minimize, tray hide, lock and
    // sleep even if History remains the current stacked page.
    const bool historyActuallyVisible = m_stackedWidget && m_historyPage
        && m_stackedWidget->currentWidget() == m_historyPage
        && isVisible() && !isMinimized() && !m_applicationSuspended;
    m_toastSystem->notificationCenter()->setHistoryVisible(
        historyActuallyVisible);
}


// Phase 1.1: New slot connected to ClientListPage::clientClicked signal
void MainWindow::onClientSelected(const ClientInfo& client, int clientIndex) {
    Q_UNUSED(clientIndex);
    showScreenView(client);
    // ScreenNavigationManager will request screens; no need to duplicate here
}

// Phase 1.1: New slot connected to ClientListPage::ongoingSceneClicked signal
void MainWindow::onOngoingSceneSelected(const QString& sceneRunId) {
    if (!m_sceneActivityModel) return;
    const SceneActivityModel::Activity activity =
        m_sceneActivityModel->activity(sceneRunId);
    if (activity.sceneRunId.isEmpty()) return;

    if (activity.direction == SceneActivityModel::Direction::Outgoing) {
        if (CanvasSession* session = findCanvasSession(activity.peerDeviceId)) {
            showScreenView(session->lastClientInfo);
            return;
        }

        // A live outgoing run normally has an instantiated canvas. Preserve a
        // safe recovery route if UI state was rebuilt while the protocol run
        // remained live.
        for (const ClientInfo& client : m_discoveredClients) {
            if (client.clientId() == activity.peerDeviceId) {
                showScreenView(client);
                return;
            }
        }
        if (m_projectManager) {
            if (const ProjectRecord* project =
                    m_projectManager->projectForTarget(activity.peerDeviceId)) {
                showScreenView(project->clientSnapshot.toClientInfo(false));
            }
        }
        return;
    }

    QString peerName;
    for (const ClientInfo& client : m_discoveredClients) {
        if (client.clientId() == activity.peerDeviceId) {
            peerName = client.getMachineName().trimmed();
            break;
        }
    }
    if (peerName.isEmpty()) {
        peerName = activity.peerDeviceId.isEmpty()
            ? QStringLiteral("Unknown device")
            : QStringLiteral("Device %1").arg(activity.peerDeviceId.left(8));
    }
    const qint64 elapsedSeconds = qMax<qint64>(
        0, QDateTime::currentMSecsSinceEpoch() - activity.startedAtEpochMs) / 1000;
    const QString duration = elapsedSeconds >= 3600
        ? QStringLiteral("%1:%2:%3")
              .arg(elapsedSeconds / 3600)
              .arg((elapsedSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
              .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2")
              .arg(elapsedSeconds / 60)
              .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0'));
    const QString started = QDateTime::fromMSecsSinceEpoch(activity.startedAtEpochMs)
                                .toString(QStringLiteral("HH:mm:ss"));
    QMessageBox::information(
        this, QStringLiteral("Ongoing Scene"),
        QStringLiteral("Received from %1\n\nStarted: %2\nDuration: %3\nNetwork: %4\n\n"
                       "This incoming scene is read-only.")
            .arg(peerName, started, duration,
                 activity.degraded ? QStringLiteral("Degraded")
                                   : QStringLiteral("Healthy")));
}

// Note: generic message hook removed; we handle specific message types via dedicated slots


MainWindow::~MainWindow() {
    prepareCleanShutdown();
    finishCleanShutdown();
}

void MainWindow::prepareCleanShutdown() {
    if (m_cleanShutdownPrepared) {
        return;
    }
    m_cleanShutdownPrepared = true;

    // Suppress the normal reconnect UI/toast path while retaining the
    // protocol-specific RemoteSession handlers needed for the ACK drain.
    if (m_webSocketClient && m_webSocketMessageHandler) {
        QObject::disconnect(m_webSocketClient, nullptr,
                            m_webSocketMessageHandler, nullptr);
    }

    // Persist every instantiated canvas before changing lifecycle state. This
    // keeps local Projects and source references intact while guaranteeing that
    // no transient RemoteSession/upload/run state reaches disk.
    if (m_sessionManager && m_projectManager) {
        for (CanvasSession* session : m_sessionManager->getAllSessions()) {
            if (session && m_projectManager->hasProjectForTarget(
                               session->persistentClientId)) {
                persistProjectCanvas(session->persistentClientId);
            }
        }
    }
    if (m_projectManager) {
        const qint64 hiddenAt = QDateTime::currentMSecsSinceEpoch();
        m_projectManager->markAllHidden(hiddenAt);
        m_projectManager->flush();
    }

    m_remoteSessionOpenPendingTargets.clear();
    m_remoteSessionOpenTargetByRequestId.clear();
    m_reopenAfterSessionCloseTargets.clear();

    const QString localDeviceId = m_webSocketClient
        ? m_webSocketClient->deviceId() : QString();
    RemoteSessionCoordinator* coordinator = m_webSocketClient
        ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
    const QList<RemoteSessionCoordinator::Binding> bindings = coordinator
        ? coordinator->all() : QList<RemoteSessionCoordinator::Binding>();

    // The close command is the authoritative terminal barrier. Both owners and
    // targets may issue it in protocol v2, so no incoming binding is omitted.
    for (const RemoteSessionCoordinator::Binding& binding : bindings) {
        if (binding.remoteSessionId.isEmpty()) continue;
        m_cleanShutdownPendingSessionIds.insert(binding.remoteSessionId);
        m_locallyTerminatingRemoteSessions.insert(binding.remoteSessionId);
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            m_webSocketClient->closeRemoteSession(
                binding.remoteSessionId, nullptr,
                QStringLiteral("clean_shutdown"));
        }
    }

    // Quiesce every outgoing canvas graph synchronously. This does not remove
    // canvas items or source files; it only destroys active render playback.
    if (m_sessionManager) {
        for (CanvasSession* session : m_sessionManager->getAllSessions()) {
            if (session && session->canvas) {
                session->canvas->stopScenesForSourceInvalidation();
            }
        }
    }

    // Retire every incoming renderer graph first. The bulk cache transaction
    // is deliberately deferred until all correlated teardownSettled barriers
    // have observed actual QObject/native-window destruction.
    for (const RemoteSessionCoordinator::Binding& binding : bindings) {
        if (binding.targetDeviceId == localDeviceId
            && !binding.remoteSessionId.isEmpty()) {
            m_cleanShutdownRendererPendingSessionIds.insert(
                binding.remoteSessionId);
            if (!g_remoteSceneController
                || !g_remoteSceneController->teardownRemoteSession(
                    binding.remoteSessionId)) {
                qCritical() << "Clean shutdown could not start renderer teardown for"
                            << binding.remoteSessionId;
            }
        }
    }
    finishCleanShutdownIncomingCacheTeardownIfReady();
    retryPendingTeardownAcks();
    if (m_sceneActivityModel) m_sceneActivityModel->clear();

    const bool canDrain = m_webSocketClient && m_webSocketClient->isConnected()
        && !m_cleanShutdownPendingSessionIds.isEmpty();
    if (!canDrain) {
        QTimer::singleShot(0, this, &MainWindow::finishCleanShutdown);
        return;
    }

    // Do not spin a nested event loop: normal Qt delivery remains available for
    // remote_session_terminating, renderer settlement, teardown ACK and close.
    const int drainDeadlineMs = m_webSocketClient->serverPolicy()
                                    .value(QStringLiteral("leaseTimeoutMs"))
                                    .toInt();
    QTimer::singleShot(std::max(0, drainDeadlineMs), this,
                       &MainWindow::finishCleanShutdown);
}

void MainWindow::finishCleanShutdownIncomingCacheTeardownIfReady()
{
    if (!m_cleanShutdownPrepared
        || m_cleanShutdownIncomingCacheTeardownStarted
        || !m_cleanShutdownRendererPendingSessionIds.isEmpty()) {
        return;
    }
    m_cleanShutdownIncomingCacheTeardownStarted = true;
    if (m_uploadManager) {
        const UploadManager::BulkTeardownResult cleanup =
            m_uploadManager->teardownAllIncomingRemoteSessions(
                QStringLiteral("clean_shutdown"));
        if (!cleanup.allLogicallyCommitted()) {
            qCritical() << "Clean shutdown left incoming cache cleanup pending:"
                        << cleanup.errorCode
                        << "failed scopes" << cleanup.cleanupErrorScopes;
        }
    }
}

void MainWindow::maybeFinishCleanShutdown() {
    if (!m_cleanShutdownPrepared || m_cleanShutdownFinished
        || !m_cleanShutdownPendingSessionIds.isEmpty()) {
        return;
    }
    finishCleanShutdown();
}

void MainWindow::finishCleanShutdown() {
    if (m_cleanShutdownFinished) return;
    m_cleanShutdownFinished = true;
    m_cleanShutdownPendingSessionIds.clear();

    if (m_connectionManager) {
        m_connectionManager->disconnect();
    } else if (m_webSocketClient && m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
    }

    if (m_cleanShutdownQuitRequested && qApp) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
}

void MainWindow::handleApplicationAboutToQuit() {
    // aboutToQuit cannot be postponed. It is therefore only the safety net for
    // OS/external exits which bypass onMenuQuitRequested; local teardown still
    // completes synchronously even though a network ACK window is unavailable.
    prepareCleanShutdown();
    finishCleanShutdown();
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
    m_pageTitleLabel = new QLabel(
        QStringLiteral("Clients · 0 authenticated · 0 live"));
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

    // History button and unread badge. The separate compact badge keeps the
    // button label stable as the count changes.
    m_historyControl = new QWidget(m_connectionBar);
    m_historyControl->setObjectName(QStringLiteral("historyControl"));
    m_historyControl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* historyControlLayout = new QHBoxLayout(m_historyControl);
    historyControlLayout->setContentsMargins(0, 0, 0, 0);
    historyControlLayout->setSpacing(4);

    m_historyButton = ThemeManager::createPillButton(
        QStringLiteral("History"), m_historyControl);
    m_historyButton->setObjectName(QStringLiteral("historyButton"));
    m_historyButton->setAccessibleName(QStringLiteral("Notification history"));
    const int historyTextWidth =
        m_historyButton->fontMetrics().horizontalAdvance(m_historyButton->text()) + 24;
    m_historyButton->setFixedWidth(qMax(80, historyTextWidth));
    connect(m_historyButton, &QPushButton::clicked,
            this, &MainWindow::showHistoryPage);
    historyControlLayout->addWidget(m_historyButton);

    m_historyUnreadBadge = new QLabel(QStringLiteral("0"), m_historyControl);
    m_historyUnreadBadge->setObjectName(QStringLiteral("historyUnreadBadge"));
    m_historyUnreadBadge->setAlignment(Qt::AlignCenter);
    m_historyUnreadBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_historyUnreadBadge->setMinimumWidth(18);
    m_historyUnreadBadge->setFixedHeight(18);
    m_historyUnreadBadge->setStyleSheet(QString(
        "QLabel { color: white; background-color: %1; border: none; "
        "border-radius: 9px; padding: 0px 4px; font-size: 10px; font-weight: bold; }")
        .arg(AppColors::colorToCss(AppColors::gStatusErrorText)));
    m_historyUnreadBadge->hide();
    historyControlLayout->addWidget(m_historyUnreadBadge);

    // [PHASE 6.1] Get local client info container from TopBarManager
    QWidget* localClientContainer = m_topBarManager ? m_topBarManager->getLocalClientInfoContainer() : nullptr;
    
    // Layout: [title][back][stretch][local-client-info][connect][history][settings]
    m_connectionLayout->addWidget(m_backButton);
    m_connectionLayout->addStretch();
    if (localClientContainer) {
        m_connectionLayout->addWidget(localClientContainer);
    }
    m_connectionLayout->addWidget(m_connectToggleButton);
    m_connectionLayout->addWidget(m_historyControl);
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
    m_clientListPage = new ClientListPage(m_sceneActivityModel, this);
    connect(m_clientListPage, &ClientListPage::clientClicked, this, &MainWindow::onClientSelected);
    connect(m_clientListPage, &ClientListPage::ongoingSceneClicked, this, &MainWindow::onOngoingSceneSelected);
    connect(m_clientListPage, &ClientListPage::summaryCountsChanged,
            this, [this](int authenticatedCount, int liveCount) {
        if (!m_pageTitleLabel || !m_stackedWidget
            || m_stackedWidget->currentWidget() != m_clientListPage) return;
        m_pageTitleLabel->setText(
            QStringLiteral("Clients · %1 authenticated · %2 live")
                .arg(authenticatedCount)
                .arg(liveCount));
    });
    m_stackedWidget->addWidget(m_clientListPage);
    
    // Show placeholder immediately (before any connection) so page isn't empty during CONNECTING state
    m_clientListPage->ensureClientListPlaceholder();
    m_clientListPage->ensureOngoingScenesPlaceholder();
    
    // Phase 1.2: Create CanvasViewPage
    m_canvasViewPage = new CanvasViewPage(this);
    m_stackedWidget->addWidget(m_canvasViewPage);

    // Notification history binds to the NotificationCenter after the toast
    // system is constructed later in MainWindow's initialization.
    m_historyPage = new HistoryPage(nullptr, this);
    m_stackedWidget->addWidget(m_historyPage);
    connect(m_stackedWidget, &QStackedWidget::currentChanged, this, [this](int) {
        updateHistoryVisibilityState();
    });
    
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
        connect(m_navigationManager, &ScreenNavigationManager::clientListEntered, this, [this](){
            // Do NOT unload uploads when navigating back to client list
            // Uploads should persist per session and only be cleared on disconnect or explicit unload
            if (m_screenCanvas) m_screenCanvas->hideRemoteCursor();
        });
    }

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
    if (m_cleanShutdownQuitRequested) return;
    m_cleanShutdownQuitRequested = true;
    setEnabled(false);
    prepareCleanShutdown();
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
    updateHistoryVisibilityState();
}

void MainWindow::hideEvent(QHideEvent* event) {
    QMainWindow::hideEvent(event);
    if (m_windowEventHandler) {
        m_windowEventHandler->handleHideEvent(event);
    }
    updateHistoryVisibilityState();
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
    if (!m_connectionManager) return;
    
    if (m_connectToggleButton->text() == "Disable") {
        // Disable client: disconnect and prevent auto-reconnect
        m_userDisconnected = true;
        m_connectionManager->disconnect();
        m_connectToggleButton->setText("Enable");
    } else {
        // Enable client: allow connections and start connecting
        m_userDisconnected = false;
        connectToServer();
        m_connectToggleButton->setText("Disable");
    }
}

// Settings dialog: server URL with Save/Cancel
void MainWindow::showSettingsDialog() {
    const QString targetDeviceId = m_activeSessionIdentity;
    const bool wasCanvasVisible = m_navigationManager
        && m_navigationManager->isOnScreenView();
    if (wasCanvasVisible && m_projectManager && !targetDeviceId.isEmpty()) {
        persistProjectCanvas(targetDeviceId);
        m_projectManager->setHidden(targetDeviceId);
    }
    if (m_settingsManager) {
        m_settingsManager->showSettingsDialog();
    }
    if (wasCanvasVisible && targetDeviceId == m_activeSessionIdentity) {
        setActiveProjectVisibleIfAppropriate();
    }
}

// (Removed stray duplicated code block previously injected)

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
    if (m_cleanShutdownPrepared && !m_cleanShutdownFinished) {
        finishCleanShutdown();
    }
}

// Legacy screen watching/cursor streaming was removed by protocol v2.

void MainWindow::onConnectionError(const QString& error) {
    if (m_cleanShutdownPrepared) return;
    qWarning() << "Failed to connect to server:" << error << "(silent mode, aucune popup)";
    TOAST_ERROR(QString("Connection failed: %1").arg(error), 4000);
}

void MainWindow::onClientListReceived(const QList<ClientInfo>& clients) {
    if (!m_activeSessionIdentity.isEmpty()
        && m_navigationManager && m_navigationManager->isOnScreenView()) {
        for (const ClientInfo& client : clients) {
            if (client.clientId() == m_activeSessionIdentity) {
                ensureRemoteSessionForClient(client);
                break;
            }
        }
    }
}

void MainWindow::onRegistrationConfirmed(const ClientInfo& clientInfo) {
    m_thisClient = clientInfo;
    if (m_sceneActivityModel) {
        const QString deviceId = clientInfo.clientId().isEmpty()
            ? (m_webSocketClient ? m_webSocketClient->deviceId() : QString())
            : clientInfo.clientId();
        m_sceneActivityModel->setLocalDeviceId(deviceId);
    }
    qDebug() << "Registration confirmed for:" << clientInfo.getMachineName();
}

void MainWindow::syncRegistration() {
    // Every device_snapshot path (initial connection, periodic refresh and
    // topology/volume changes) converges here.  Keep the receiver absent from
    // discovery while a previous process/server-boot cache has not reached a
    // durable logical quarantine commit.  The connection handler owns retries;
    // periodic refreshes must never bypass that fail-closed decision.
    if (m_uploadManager
        && !m_uploadManager->receiverReadyForAdvertisement()) {
        qWarning() << "Device snapshot suppressed until remote cache cleanup commits:"
                   << m_uploadManager->receiverCleanupError();
        return;
    }
    // [PHASE 7.2] Delegate to ScreenEventHandler
    if (m_screenEventHandler) {
        m_screenEventHandler->syncRegistration();
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
    if (!m_connectionManager) return;
    // [Phase 12] Get server URL from SettingsManager
    const QString url = m_settingsManager
        ? m_settingsManager->getServerUrl()
        : AppConfig::instance().serverUrl();
    m_connectionManager->connectToServer(url);
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
