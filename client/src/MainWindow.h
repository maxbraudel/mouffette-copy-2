#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSpacerItem>
#include <QStatusBar>
#include <QTimer>
#include <QScreen>
#include <QApplication>
#include <QSystemTrayIcon>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QHideEvent>
#include <QScrollArea>
#include <QWidget>
#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QFont>
#include "backend/network/WebSocketClient.h"
#include "backend/domain/models/ClientInfo.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include "backend/domain/session/SessionManager.h"

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
QT_END_NAMESPACE
class QMediaPlayer;
class QListWidgetItem;
class OverlayPanel;
class ResizableMediaBase;

class SpinnerWidget; // forward declaration for custom loading spinner
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QProcess; // fwd decl to avoid including in header
class UploadManager; // new component for upload/unload feature
class ScreenNavigationManager; // manages page switching & loader UX
class ResponsiveLayoutManager; // manages responsive layout behavior
class FileWatcher; // monitors source files and removes media when files are deleted
class SessionManager; // Phase 4.1: manages canvas session lifecycle
class ProjectManager;
class SceneActivityModel;
struct ProjectMediaReference;
class FileManager; // Phase 4.3: forward declaration for dependency injection
class ClientListPage; // Phase 1.1: extracted client list page
class CanvasViewPage; // Phase 1.2: extracted canvas view page
class HistoryPage;
class RemoteClientInfoManager; // Phase 5: manages remote client info container
class SystemMonitor; // Phase 3: system monitoring (volume, screens, platform)
class TopBarManager; // Phase 6.1: manages top bar UI (local client info)
class SystemTrayManager; // Phase 6.2: manages system tray icon
class MenuBarManager; // Phase 6.3: manages menu bar (File, Help menus)
class WebSocketMessageHandler; // Phase 7.1: manages WebSocket message routing
class ScreenEventHandler; // Phase 7.2: manages screen events and registration
class ClientListEventHandler; // Phase 7.3: manages client list events and connection state
class UploadEventHandler; // Phase 7.4: manages upload events and file transfers
class CanvasSessionController; // Phase 8: manages canvas session lifecycle
class WindowEventHandler; // Phase 9: manages window lifecycle events
class TimerController; // Phase 10: manages timer setup and callbacks
class ConnectionManager;
class UploadButtonStyleManager; // Phase 11: manages upload button styling
class SettingsManager; // Phase 12: manages application settings and persistence
class UploadSignalConnector; // Phase 15: manages upload signal connections
class ClientListBuilder; // Phase 16: builds display client list from connected + offline clients
// using QStackedWidget for canvas container switching
class QFrame; // forward declare for separators in remote info container

// Forward declaration of extracted ScreenCanvas
class ScreenCanvas;
class ICanvasHost;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class CanvasSessionController;
    friend class TimerController;

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    // Fallback used by QCoreApplication::aboutToQuit. Normal UI quits use the
    // asynchronous drain below; an externally initiated exit still performs
    // the same synchronous local teardown before Qt stops its event loop.
    void handleApplicationAboutToQuit();
    
    // Accessor methods for ResponsiveLayoutManager
    QWidget* getRemoteClientInfoContainer() const;
    QWidget* getLocalClientInfoContainer() const; // Phase 6.1: Delegate to TopBarManager
    QHBoxLayout* getConnectionLayout() const { return m_connectionLayout; }
    QVBoxLayout* getMainLayout() const { return m_mainLayout; }
    QStackedWidget* getStackedWidget() const { return m_stackedWidget; }
    CanvasViewPage* getCanvasViewPage() const { return m_canvasViewPage; }
    ClientListPage* getClientListPage() const { return m_clientListPage; }
    HistoryPage* getHistoryPage() const { return m_historyPage; }
    QPushButton* getBackButton() const;
    QLabel* getConnectionStatusLabel() const { return m_connectionStatusLabel; }
    QPushButton* getConnectToggleButton() const { return m_connectToggleButton; }
    QPushButton* getSettingsButton() const { return m_settingsButton; }
    QWidget* getHistoryControl() const { return m_historyControl; }
    int getInnerContentGap() const;
    
    // [Phase 7.1] Accessor methods for WebSocketMessageHandler
    ScreenNavigationManager* getNavigationManager() const { return m_navigationManager; }
    WebSocketClient* getWebSocketClient() const { return m_webSocketClient; }
    UploadManager* getUploadManager() const { return m_uploadManager; }
    const ClientInfo& getSelectedClient() const { return m_selectedClient; }
    bool isUserDisconnected() const { return m_userDisconnected; }
    
    // [Phase 7.1] State management for WebSocketMessageHandler
    void setCanvasRevealedForCurrentClient(bool revealed) { m_canvasRevealedForCurrentClient = revealed; }
    void setPreserveViewportOnReconnect(bool preserve) { m_preserveViewportOnReconnect = preserve; }
    void resetAllSessionUploadStates();
    
    // [Phase 7.1] UI methods for WebSocketMessageHandler (made public)
    void setUIEnabled(bool enabled);
    void setLocalNetworkStatus(const QString& status);
    void syncRegistration();
    void removeVolumeIndicatorFromLayout();
    void addRemoteStatusToLayout();
    void setRemoteConnectionStatus(const QString& status, bool propagateLoss = true);
    void connectToServer(); // [Phase 12] Made public for SettingsManager
    
    // [Phase 7.2] Accessor methods for ScreenEventHandler
    FileManager* getFileManager() const { return m_fileManager; }
    RemoteClientInfoManager* getRemoteClientInfoManager() const { return m_remoteClientInfoManager; }
    QString getActiveSessionIdentity() const { return m_activeSessionIdentity; }
    bool isCanvasRevealedForCurrentClient() const { return m_canvasRevealedForCurrentClient; }
    bool shouldPreserveViewportOnReconnect() const { return m_preserveViewportOnReconnect; }
    QList<ScreenInfo> getLocalScreenInfo();
    int getSystemVolumePercent();
    QString getMachineName();
    QString getPlatformName();
    
    // [Phase 7.2] State management for ScreenEventHandler
    void setActiveCanvas(ICanvasHost* canvas) { m_screenCanvas = canvas; }
    void setSelectedClient(const ClientInfo& client) { m_selectedClient = client; }
    void setCanvasContentEverLoaded(bool loaded) { m_canvasContentEverLoaded = loaded; }
    void stopInlineSpinner();
    void addVolumeIndicatorToLayout();
    void updateVolumeIndicator();
    void updateClientNameDisplay(const ClientInfo& clientInfo);
    
    /**
     * @brief Set the complete remote client state (single source of truth)
     * 
     * This is THE method to update remote client info. It applies the state
     * atomically without flickering and manages all related UI (spinner, etc).
     * 
     * @param state The complete state to apply
     * @param propagateLoss Whether to propagate connection loss
     */
    void setRemoteClientState(const struct RemoteClientState& state, bool propagateLoss = true);
    
    // [Phase 7.2] Session management for ScreenEventHandler
    using CanvasSession = SessionManager::CanvasSession;
    CanvasSession& ensureCanvasSession(const ClientInfo& client);
    CanvasSession* findCanvasSession(const QString& persistentClientId);
    CanvasSession* findCanvasSessionByServerClientId(const QString& serverClientId);
    void configureCanvasSession(CanvasSession& session);
    
    // [Phase 7.3] Accessor methods for ClientListEventHandler
    ICanvasHost* getScreenCanvas() const { return m_screenCanvas; }
    QList<ClientInfo> buildDisplayClientList(const QList<ClientInfo>& connectedClients);
    void markAllSessionsOffline(); // [Phase 16] Made public for ClientListBuilder
    int getLastConnectedClientCount() const { return m_lastConnectedClientCount; }
    void setLastConnectedClientCount(int count) { m_lastConnectedClientCount = count; }
    QString getActiveRemoteClientId() const { return m_activeRemoteClientId; }
    void setActiveRemoteClientId(const QString& id) { m_activeRemoteClientId = id; }
    bool isRemoteClientConnected() const { return m_remoteClientConnected; }
    void setRemoteClientConnected(bool connected) { m_remoteClientConnected = connected; }
    // [Phase 12] Accessor for SettingsManager
    void setUserDisconnected(bool disconnected) { m_userDisconnected = disconnected; }
    bool isInlineSpinnerSpinning() const;
    void showInlineSpinner();
    void startInlineSpinner();
    void ensureRemoteSessionForClient(const ClientInfo& client);
    
    // [Phase 7.4] Accessor methods for UploadEventHandler
    QString getActiveUploadSessionIdentity() const { return m_activeUploadSessionIdentity; }
    void setActiveUploadSessionIdentity(const QString& identity) { m_activeUploadSessionIdentity = identity; }
    FileWatcher* getFileWatcher() const { return m_fileWatcher; }
    void reconcileRemoteFilesForSession(CanvasSession& session, const QSet<QString>& currentFileIds);
    bool areUploadSignalsConnected() const { return m_uploadSignalsConnected; }
    void connectUploadSignals();
    void setUploadSessionByUploadId(const QString& uploadId, const QString& sessionIdentity);
    QString getUploadSessionByUploadId(const QString& uploadId) const { return m_uploadSessionByUploadId.value(uploadId); }
    void removeUploadSessionByUploadId(const QString& uploadId) { m_uploadSessionByUploadId.remove(uploadId); }
    
    // [Phase 8] Additional accessor methods for CanvasSessionController
    SessionManager* getSessionManager() const { return m_sessionManager; }
    ProjectManager* getProjectManager() const { return m_projectManager; }
    void setActiveSessionIdentity(const QString& identity) { m_activeSessionIdentity = identity; }
    QPushButton* getUploadButton() const { return m_uploadButton; }
    void setUploadButton(QPushButton* button) { m_uploadButton = button; }
    bool getUploadButtonInOverlay() const { return m_uploadButtonInOverlay; }
    void setUploadButtonInOverlay(bool inOverlay) { m_uploadButtonInOverlay = inOverlay; }
    QFont getUploadButtonDefaultFont() const { return m_uploadButtonDefaultFont; }
    void setUploadButtonDefaultFont(const QFont& font) { m_uploadButtonDefaultFont = font; }
    // [Phase 12] Delegate to SettingsManager
    bool getAutoUploadImportedMedia() const; // Implementation in .cpp to avoid incomplete type
    bool useQuickCanvasRenderer() const;
    QString createIdeaId() const;
    void markCanvasLoadRequest(const QString& persistentClientId);
    void recordCanvasLoadReady(const QString& persistentClientId, int screenCount);
    
    // [Phase 14.3] Session management accessors
    CanvasSession* sessionForActiveUpload();
    CanvasSession* sessionForUploadId(const QString& uploadId);
    
    // [Phase 15] Upload tracking management (made public for UploadSignalConnector)
    void clearUploadTracking(CanvasSession& session);
    
    // [Phase 11] Accessor methods for UploadButtonStyleManager
    bool isRemoteOverlayActionsEnabled() const { return m_remoteOverlayActionsEnabled; }
    bool hasUnuploadedFilesForTarget(const QString& targetClientId) const;

    // Remote-overlay action coordinator
    void refreshOverlayActionsState(bool remoteConnected, bool propagateLoss = true);
    
    // [Phase 9] Accessor methods for WindowEventHandler
    ResponsiveLayoutManager* getResponsiveLayoutManager() const { return m_responsiveLayoutManager; }
    bool isApplicationSuspended() const { return m_applicationSuspended; }
    void setApplicationSuspended(bool suspended);
    
    // [Phase 10] Accessor methods for TimerController
    QTimer* getStatusUpdateTimer() const { return m_statusUpdateTimer; }
    QTimer* getDisplaySyncTimer() const { return m_displaySyncTimer; }

public slots:
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void handleNativeSystemSuspendedChanged(bool suspended);
    void onUploadButtonClicked();  // Made public for CanvasSessionController

private slots:
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& error);
    void onClientListReceived(const QList<ClientInfo>& clients);
    void onRegistrationConfirmed(const ClientInfo& clientInfo);
    
    // Phase 1.1: ClientListPage signals
    void onClientSelected(const ClientInfo& clientInfo, int clientIndex);
    void onOngoingSceneSelected(const QString& sceneRunId);
    
    void updateConnectionStatus();
    void onEnableDisableClicked();
    
    // Screen view slots
    void onBackToClientListClicked();
    void onRemoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName);
    void showHistoryPage();
    void onDisconnectProjectRequested();
    void onDeleteProjectRequested();
    
    // System tray slots
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    
    // Menu bar slots [Phase 6.3]
    void onMenuQuitRequested();
    void onMenuAboutRequested();
    
    void showSettingsDialog();

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    // [Phase 7.2] CanvasSession moved to public for ScreenEventHandler

    void setupUI();
    void setupTrayIcon();
    void setupConnections();
    void updateStylesheetsForTheme();
    void createScreenViewPage();
    // connectToServer() moved to public (Phase 12)
    // [Phase 7.1] scheduleReconnect, syncRegistration, setUIEnabled, setLocalNetworkStatus moved to public
    // [Phase 7.2] getLocalScreenInfo, getMachineName, getPlatformName, getSystemVolumePercent moved to public
    void setupVolumeMonitoring();
    void setupCursorMonitoring();
    // Animation durations are set directly where animations are created
    
    // Screen view methods
    void showScreenView(const ClientInfo& client);
    void showClientListView();
    void createRemoteClientInfoContainer(); // Create grouped container for remote client info
    void initializeRemoteClientInfoInTopBar(); // Initialize remote client info in top bar
    void createLocalClientInfoContainer(); // Create grouped container for local client info (You + network status)
    // [Phase 7.1] setLocalNetworkStatus moved to public
    // [Phase 7.2] updateClientNameDisplay, updateVolumeIndicator, addVolumeIndicatorToLayout moved to public
    // Legacy helper removed; ScreenCanvas renders screens directly
    // [Phase 7.1] setRemoteConnectionStatus, removeVolumeIndicatorFromLayout, addRemoteStatusToLayout moved to public
    // [Phase 8] refreshOverlayActionsState moved to public
    // Legacy screen-watch/cursor protocol was removed in protocol v2.
    // Manage presence of the remote status (and its leading separator) in the top bar layout
    void removeRemoteStatusFromLayout();
    // [Phase 7.2] Session management methods moved to public for ScreenEventHandler
    const CanvasSession* findCanvasSession(const QString& persistentClientId) const;
    const CanvasSession* findCanvasSessionByServerClientId(const QString& serverClientId) const;
    CanvasSession* findCanvasSessionByIdeaId(const QString& canvasSessionId);
    void switchToCanvasSession(const QString& persistentClientId);
    void updateUploadButtonForSession(CanvasSession& session);
    
    // PHASE 2: State synchronization after reconnection

    void rotateSessionIdea(CanvasSession& session);
    // markAllSessionsOffline() moved to public (Phase 16)
    ICanvasHost* canvasForClientId(const QString& clientId) const;
    // sessionForActiveUpload() moved to public (Phase 14.3)
    // sessionForUploadId() moved to public (Phase 14.3)
    // clearUploadTracking() moved to public (Phase 15)
    void refreshOngoingScenesList();
    void applyListWidgetStyle(QListWidget* listWidget) const;
    void updateApplicationSuspendedState(bool suspended);
    void updateHistoryVisibilityState();
    void updateHistoryUnreadBadge(int unreadCount);
    void persistProjectCanvas(const QString& targetDeviceId);
    void restoreProjectCanvas(CanvasSession& session);
    void terminateProjectRemoteSession(const QString& targetDeviceId, bool attemptRemote);
    void handleRemoteSessionReady(const QJsonObject& envelope, bool resumed);
    void handleRemoteSessionLeaseState(const QJsonObject& envelope);
    void handleRemoteSessionTerminating(const QJsonObject& envelope);
    void handleRemoteRendererTeardownSettled(const QString& remoteSessionId,
                                             bool success);
    void beginTerminalIncomingCacheCleanup(const QString& reasonCode);
    void finishTerminalIncomingCacheCleanupIfReady();
    void handleRemoteSessionClosed(const QJsonObject& envelope);
    void handleRemoteSessionError(const QJsonObject& envelope);
    void updateRemoteClientAvailability(const QString& targetDeviceId,
                                        const QString& status);
    void clearRemoteSessionRuntimeState(const QString& targetDeviceId,
                                        bool connectionLost);
    void finishDeferredProjectDeletion(const QString& targetDeviceId);
    void retryPendingTeardownAcks();
    void removeRuntimeCanvasSession(const QString& targetDeviceId);
    void refreshProjectClientList();
    void setActiveProjectVisibleIfAppropriate();
    void prepareCleanShutdown();
    void finishCleanShutdownIncomingCacheTeardownIfReady();
    void maybeFinishCleanShutdown();
    void finishCleanShutdown();
    QList<ProjectMediaReference> collectProjectMediaReferences(
        const QString& targetDeviceId, ICanvasHost* canvas) const;
    void removeInvalidMediaItems(const QList<ResizableMediaBase*>& mediaItems);
    void validateAllProjectSources();

    // UI Components
    QWidget* m_centralWidget;
    QVBoxLayout* m_mainLayout;
    QStackedWidget* m_stackedWidget;
    QWidget* m_connectionBar; // container widget for top connection layout
    
    // Phase 1.1: Client list page (extracted)
    ClientListPage* m_clientListPage;
    
    // Phase 1.2: Canvas view page (extracted)
    CanvasViewPage* m_canvasViewPage;

    // Durable notification history page.
    HistoryPage* m_historyPage = nullptr;
    
    // Connection section
    QHBoxLayout* m_connectionLayout;
    QPushButton* m_settingsButton;
    QWidget* m_historyControl = nullptr;
    QPushButton* m_historyButton = nullptr;
    QLabel* m_historyUnreadBadge = nullptr;
    QPushButton* m_connectToggleButton;
    QLabel* m_connectionStatusLabel;
    
    // Top bar components (managed by ResponsiveLayoutManager)
    QLabel* m_pageTitleLabel = nullptr;
    QPushButton* m_backButton = nullptr;
    
    // Phase 5: Remote client info manager (extracted)
    RemoteClientInfoManager* m_remoteClientInfoManager = nullptr;
    
    // Phase 3: System monitor (volume, screens, platform info)
    SystemMonitor* m_systemMonitor = nullptr;
    
    // Phase 6.1: Top bar manager (local client info)
    TopBarManager* m_topBarManager = nullptr;
    
    // Remote client info container wrapper (managed dynamically)
    QWidget* m_remoteClientInfoWrapper = nullptr;
    
    // Responsive layout manager
    ResponsiveLayoutManager* m_responsiveLayoutManager = nullptr;
    
    // Phase 4.3: FileManager injected (not singleton)
    FileManager* m_fileManager = nullptr;
    
    // Phase 4.1: Session manager (COMPLETED - replaced m_canvasSessions)
    SessionManager* m_sessionManager = nullptr;
    ProjectManager* m_projectManager = nullptr;
    SceneActivityModel* m_sceneActivityModel = nullptr;
    
    // Active canvas pointer (dynamically points to current session's canvas)
    ICanvasHost* m_screenCanvas = nullptr;
    
    // Upload button state (dynamically points to current session's upload button)
    QPushButton* m_uploadButton = nullptr;
    bool m_uploadButtonInOverlay = false;
    QFont m_uploadButtonDefaultFont;
    
    // Remote overlay actions state
    bool m_remoteOverlayActionsEnabled = false;
    
    // Animation durations (used by ScreenNavigationManager)
    int m_loaderDelayMs = 500;
    int m_loaderFadeDurationMs = 150;
    int m_fadeDurationMs = 200;

    // Menu and actions [Phase 6.3: Now managed by MenuBarManager]
    MenuBarManager* m_menuBarManager = nullptr;
    
    // Phase 6.2: System tray manager
    SystemTrayManager* m_systemTrayManager = nullptr;

    // Backend - network client must be available before handlers
    WebSocketClient* m_webSocketClient = nullptr;
    ConnectionManager* m_connectionManager = nullptr;
    
    // Phase 7.1: WebSocket message handler
    WebSocketMessageHandler* m_webSocketMessageHandler = nullptr;
    
    // Phase 7.2: Screen event handler
    ScreenEventHandler* m_screenEventHandler = nullptr;
    
    // Phase 7.3: Client list event handler
    ClientListEventHandler* m_clientListEventHandler = nullptr;
    
    // Phase 7.4: Upload event handler
    UploadEventHandler* m_uploadEventHandler = nullptr;
    
    // Phase 8: Canvas session controller
    CanvasSessionController* m_canvasSessionController = nullptr;
    
    // [Phase 9] Window event handler
    WindowEventHandler* m_windowEventHandler = nullptr;
    
    // [Phase 10] Timer controller
    TimerController* m_timerController = nullptr;
    
    // [Phase 11] Upload button style manager
    UploadButtonStyleManager* m_uploadButtonStyleManager = nullptr;
    
    // [Phase 12] Settings manager
    SettingsManager* m_settingsManager = nullptr;
    
    // [Phase 15] Upload signal connector
    UploadSignalConnector* m_uploadSignalConnector = nullptr;
    
    int m_lastConnectedClientCount = 0;
    QString m_activeSessionIdentity;
    ClientInfo m_thisClient;
    ClientInfo m_selectedClient;
    QTimer* m_statusUpdateTimer;
    QTimer* m_displaySyncTimer;
    bool m_userDisconnected = false; // suppress auto-reconnect UI flows when true
    // m_serverUrlConfig moved to SettingsManager (Phase 12)
    // m_autoUploadImportedMedia moved to SettingsManager (Phase 12)
    
    // Navigation state
    bool m_ignoreSelectionChange;
    
    // [PHASE 3] Volume monitoring moved to SystemMonitor
    
    // Upload feature state
    UploadManager* m_uploadManager;
    bool m_uploadSignalsConnected = false;
    QString m_activeUploadSessionIdentity;
    QHash<QString, QString> m_uploadSessionByUploadId; // uploadId -> session identity
    ScreenNavigationManager* m_navigationManager = nullptr; // new navigation component
    FileWatcher* m_fileWatcher = nullptr; // monitors source files for deletion
    // Canvas reveal state: ensure fade-in/recenter happen only once per selected client
    bool m_canvasRevealedForCurrentClient = false;
    // Track if canvas content has been loaded at least once (to decide between full-screen vs inline loader)
    bool m_canvasContentEverLoaded = false;
    // Preserve the current viewport (zoom/pan) across temporary connection losses
    bool m_preserveViewportOnReconnect = false;
    // Small inline spinner for reconnection (shown in client info container)
    SpinnerWidget* m_inlineSpinner = nullptr;
    QString m_activeRemoteClientId;
    bool m_remoteClientConnected = false;
    bool m_applicationSuspended = false;
    QHash<QString, qint64> m_canvasLoadRequestMsBySession;
    QList<ClientInfo> m_discoveredClients;
    QSet<QString> m_restoredProjectIds;
    QHash<QString, QString> m_remoteSessionOpenTargetByRequestId;
    QSet<QString> m_remoteSessionOpenPendingTargets;
    QSet<QString> m_remoteSessionOpenSuppressedTargets;
    QSet<QString> m_reopenAfterSessionCloseTargets;
    QSet<QString> m_disconnectPendingTargets;
    QSet<QString> m_deleteAfterSessionCloseTargets;
    QSet<QString> m_locallyTerminatingRemoteSessions;
    struct PendingTeardownAck {
        QString teardownId;
        bool sceneStopped = false;
        bool uploadsAborted = false;
        bool cacheQuarantined = false;
        int removedFileCount = 0;
        QString errorCode;
        qint64 quarantinedBytes = 0;
    };
    struct PendingRendererTeardown {
        QString ownerDeviceId;
        QString teardownId;
        quint64 generation = 0;
    };
    QHash<QString, PendingRendererTeardown> m_pendingRendererTeardowns;
    QHash<QString, PendingTeardownAck> m_pendingTeardownAcks;
    // Lease expiry/server restart has no authenticated teardownId. Keep every
    // incoming binding captured synchronously at that edge until its renderer
    // graph settles, then perform one provisional bulk cache transaction.
    QSet<QString> m_terminalRendererPendingSessionIds;
    QString m_terminalIncomingCleanupReason;
    bool m_terminalIncomingCleanupActive = false;
    bool m_terminalIncomingCacheTeardownStarted = false;
    QSet<QString> m_cleanShutdownPendingSessionIds;
    QSet<QString> m_cleanShutdownRendererPendingSessionIds;
    bool m_cleanShutdownIncomingCacheTeardownStarted = false;
    bool m_cleanShutdownPrepared = false;
    bool m_cleanShutdownFinished = false;
    bool m_cleanShutdownQuitRequested = false;
    
    // Toast notification system
    ToastNotificationSystem* m_toastSystem = nullptr;

private slots:
    // Upload-specific progress/finish now managed by UploadManager

private:
    void updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles);
};

#endif // MAINWINDOW_H
