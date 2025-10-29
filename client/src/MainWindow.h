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
#include "WebSocketClient.h"
#include "ClientInfo.h"
#include "ToastNotificationSystem.h"
#include "SessionManager.h"

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
QT_END_NAMESPACE
class QMimeData; // fwd declare for drag preview helpers
class QMediaPlayer;
class QListWidgetItem;
class OverlayPanel;
class ResizableMediaBase;

class SpinnerWidget; // forward declaration for custom loading spinner
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QProcess; // fwd decl to avoid including in header
class UploadManager; // new component for upload/unload feature
class WatchManager;  // new component for watch/unwatch feature
class ScreenNavigationManager; // manages page switching & loader UX
class ResponsiveLayoutManager; // manages responsive layout behavior
class FileWatcher; // monitors source files and removes media when files are deleted
class SessionManager; // Phase 4.1: manages canvas session lifecycle
class FileManager; // Phase 4.3: forward declaration for dependency injection
class ClientListPage; // Phase 1.1: extracted client list page
// using QStackedWidget for canvas container switching
class QFrame; // forward declare for separators in remote info container

// Forward declaration of extracted ScreenCanvas
class ScreenCanvas;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    
    // Accessor methods for ResponsiveLayoutManager
    QWidget* getRemoteClientInfoContainer() const { return m_remoteClientInfoContainer; }
    QWidget* getLocalClientInfoContainer() const { return m_localClientInfoContainer; }
    QHBoxLayout* getConnectionLayout() const { return m_connectionLayout; }
    QVBoxLayout* getMainLayout() const { return m_mainLayout; }
    QStackedWidget* getStackedWidget() const { return m_stackedWidget; }
    QWidget* getScreenViewWidget() const { return m_screenViewWidget; }
    ClientListPage* getClientListPage() const { return m_clientListPage; }
    QPushButton* getBackButton() const { return m_backButton; }
    QLabel* getConnectionStatusLabel() const { return m_connectionStatusLabel; }
    QPushButton* getConnectToggleButton() const { return m_connectToggleButton; }
    QPushButton* getSettingsButton() const { return m_settingsButton; }
    int getInnerContentGap() const;

public slots:
    void handleApplicationStateChanged(Qt::ApplicationState state);

private slots:
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& error);
    void onClientListReceived(const QList<ClientInfo>& clients);
    void onRegistrationConfirmed(const ClientInfo& clientInfo);
    
    // Phase 1.1: ClientListPage signals
    void onClientSelected(const ClientInfo& clientInfo, int clientIndex);
    void onOngoingSceneSelected(const QString& persistentClientId);
    
    void updateConnectionStatus();
    void onEnableDisableClicked();
    void attemptReconnect();
    
    // Screen view slots
    void onBackToClientListClicked();
    void onScreensInfoReceived(const ClientInfo& clientInfo);
    void onWatchStatusChanged(bool watched);
    void onDataRequestReceived();
    void onRemoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName);
    
    // System tray slots
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void onUploadButtonClicked();
    void showSettingsDialog();
    
    // Helper methods
    bool hasUnuploadedFilesForTarget(const QString& targetClientId) const;

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    // Phase 4.1: Use SessionManager's CanvasSession structure
    using CanvasSession = SessionManager::CanvasSession;

    void setupUI();
    void setupTrayIcon();
    void setupConnections();
    void updateStylesheetsForTheme();
    void createScreenViewPage();
    void setupMenuBar();
    void setupSystemTray();
    void connectToServer();
    void scheduleReconnect();
    // Sync local display/machine info with the server (used on connect and on display changes)
    void syncRegistration();
    QList<ScreenInfo> getLocalScreenInfo();
    QString getMachineName();
    QString getPlatformName();
    // Returns last known system volume without blocking the UI thread.
    // On macOS, updated asynchronously; on Windows, queried on demand.
    int getSystemVolumePercent();
    void setupVolumeMonitoring();
    void setupCursorMonitoring();
    void setUIEnabled(bool enabled);
    // Animation durations are set directly where animations are created
    
    // Screen view methods
    void showScreenView(const ClientInfo& client);
    void showClientListView();
    void createRemoteClientInfoContainer(); // Create grouped container for remote client info
    void initializeRemoteClientInfoInTopBar(); // Initialize remote client info in top bar
    void createLocalClientInfoContainer(); // Create grouped container for local client info (You + network status)
    void setLocalNetworkStatus(const QString& status); // Update local network status
    void updateClientNameDisplay(const ClientInfo& client);
    // Legacy helper removed; ScreenCanvas renders screens directly
    void updateVolumeIndicator();
    void setRemoteConnectionStatus(const QString& status, bool propagateLoss = true);
    void refreshOverlayActionsState(bool remoteConnected, bool propagateLoss = true);
    // watch management handled by WatchManager component now
    // Manage presence of the volume indicator in the top bar layout
    void removeVolumeIndicatorFromLayout();
    void addVolumeIndicatorToLayout();
    // Manage presence of the remote status (and its leading separator) in the top bar layout
    void removeRemoteStatusFromLayout();
    void addRemoteStatusToLayout();
    CanvasSession& ensureCanvasSession(const ClientInfo& client);
    CanvasSession* findCanvasSession(const QString& persistentClientId);
    const CanvasSession* findCanvasSession(const QString& persistentClientId) const;
    CanvasSession* findCanvasSessionByServerClientId(const QString& serverClientId);
    const CanvasSession* findCanvasSessionByServerClientId(const QString& serverClientId) const;
    CanvasSession* findCanvasSessionByIdeaId(const QString& canvasSessionId);
    void configureCanvasSession(CanvasSession& session);
    void switchToCanvasSession(const QString& persistentClientId);
    void updateUploadButtonForSession(CanvasSession& session);
    
    // PHASE 2: State synchronization after reconnection
    void handleStateSyncFromServer(const QJsonObject& message);

    void unloadUploadsForSession(CanvasSession& session, bool attemptRemote);
    QString createIdeaId() const;
    void rotateSessionIdea(CanvasSession& session);
    void reconcileRemoteFilesForSession(CanvasSession& session, const QSet<QString>& currentFileIds);
    QList<ClientInfo> buildDisplayClientList(const QList<ClientInfo>& connectedClients);
    void markAllSessionsOffline();
    ScreenCanvas* canvasForClientId(const QString& clientId) const;
    CanvasSession* sessionForActiveUpload();
    CanvasSession* sessionForUploadId(const QString& uploadId);
    void clearUploadTracking(CanvasSession& session);
    void refreshOngoingScenesList();
    void applyListWidgetStyle(QListWidget* listWidget) const;
    void updateApplicationSuspendedState(bool suspended);

    // UI Components
    QWidget* m_centralWidget;
    QVBoxLayout* m_mainLayout;
    QStackedWidget* m_stackedWidget;
    QWidget* m_connectionBar; // container widget for top connection layout
    
    // Phase 1.1: Client list page (extracted)
    ClientListPage* m_clientListPage;
    
    // Connection section
    QHBoxLayout* m_connectionLayout;
    QPushButton* m_settingsButton;
    QPushButton* m_connectToggleButton;
    QLabel* m_connectionStatusLabel;
    
    // Screen view section
    QWidget* m_screenViewWidget;
    QVBoxLayout* m_screenViewLayout;
    QLabel* m_clientNameLabel;
    QLabel* m_remoteConnectionStatusLabel;
    // Top bar contextual title (e.g., "Connected Clients" on client list page)
    QLabel* m_pageTitleLabel = nullptr;
    QWidget* m_remoteClientInfoContainer = nullptr; // Container for hostname, status, volume
    QWidget* m_remoteClientInfoWrapper = nullptr;   // Wrapper holding container and inline spinner
    QFrame* m_remoteInfoSep2 = nullptr; // Trailing separator before volume indicator
    QFrame* m_remoteInfoSep1 = nullptr; // Leading separator before remote status
    QWidget* m_localClientInfoContainer = nullptr; // Container for "You" and network status
    QLabel* m_localClientTitleLabel = nullptr; // "You" label in local client container
    QLabel* m_localNetworkStatusLabel = nullptr; // Network status in local client container
    // Canvas container keeps border visible; inside we switch between spinner and canvas
    QWidget* m_canvasContainer;
    QStackedWidget* m_canvasStack;
    QStackedWidget* m_canvasHostStack = nullptr;
    ScreenCanvas* m_screenCanvas;
    QLabel* m_volumeIndicator;
    SpinnerWidget* m_loadingSpinner;
    QPushButton* m_uploadButton;
    bool m_uploadButtonInOverlay = false; // Track if upload button is in overlay (uses custom styling)
    QPushButton* m_backButton;
    QFont m_uploadButtonDefaultFont;
    bool m_remoteOverlayActionsEnabled = false;
    // Loader/content animations
    int m_loaderDelayMs = 1000;       // show spinner after this delay
    int m_loaderFadeDurationMs = 500; // fade-in duration for spinner (loader)
    int m_fadeDurationMs = 50;        // fade-in duration for canvas and indicators
    QGraphicsOpacityEffect* m_spinnerOpacity = nullptr;
    QPropertyAnimation* m_spinnerFade = nullptr;
    QGraphicsOpacityEffect* m_canvasOpacity = nullptr;
    QPropertyAnimation* m_canvasFade = nullptr;
    QGraphicsOpacityEffect* m_volumeOpacity = nullptr;
    QPropertyAnimation* m_volumeFade = nullptr;
    // Cursor monitoring (only when this client is watched by someone else)
    QTimer* m_cursorTimer = nullptr;
    int m_cursorUpdateIntervalMs = 33; // ~30 Hz by default
    
    // Responsive layout manager
    ResponsiveLayoutManager* m_responsiveLayoutManager = nullptr;
    
    // Phase 4.3: FileManager injected (not singleton)
    FileManager* m_fileManager = nullptr;
    
    // Phase 4.1: Session manager (COMPLETED - replaced m_canvasSessions)
    SessionManager* m_sessionManager = nullptr;

    // Menu and actions
    QMenu* m_fileMenu;
    QMenu* m_helpMenu;
    QAction* m_exitAction;
    QAction* m_aboutAction;
    
    // System tray
    QSystemTrayIcon* m_trayIcon = nullptr; // deprecated; retained pointer to avoid widespread removal
    
    // Backend
    WebSocketClient* m_webSocketClient;
    int m_lastConnectedClientCount = 0;
    QString m_activeSessionIdentity;
    ClientInfo m_thisClient;
    ClientInfo m_selectedClient;
    QTimer* m_statusUpdateTimer;
    QTimer* m_displaySyncTimer;
    // Smart reconnection system
    QTimer* m_reconnectTimer;
    int m_reconnectAttempts;
    int m_maxReconnectDelay;
    bool m_isWatched = false; // true when at least one remote client is watching us
    bool m_userDisconnected = false; // suppress auto-reconnect UI flows when true
    QString m_serverUrlConfig; // configurable server URL
    bool m_autoUploadImportedMedia = false; // new setting: auto-upload on drop
    
    // Navigation state
    bool m_ignoreSelectionChange;
    // watched client id moved to WatchManager
    
    // Constants
    static const QString DEFAULT_SERVER_URL;
    
    // Volume monitoring (to avoid 1s spikes)
    int m_cachedSystemVolume = -1;        // last known value (0-100), -1 unknown
#ifdef Q_OS_MACOS
    QProcess* m_volProc = nullptr;       // reused for async osascript calls
    QTimer* m_volTimer = nullptr;        // polls in background
#endif

    // Upload feature state
    UploadManager* m_uploadManager;
    bool m_uploadSignalsConnected = false;
    QString m_activeUploadSessionIdentity;
    QHash<QString, QString> m_uploadSessionByUploadId; // uploadId -> session identity
    WatchManager* m_watchManager = nullptr;   // extracted watch logic
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
    
    // Toast notification system
    ToastNotificationSystem* m_toastSystem = nullptr;

private slots:
    // Upload-specific progress/finish now managed by UploadManager

private:
    void updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles);
};

#endif // MAINWINDOW_H
