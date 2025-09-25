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
#include <QSet>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QWidget>
#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMessageBox>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QGraphicsEllipseItem>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QScrollBar>
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QFont>
#include "WebSocketClient.h"
#include "ClientInfo.h"

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
    QWidget* getClientListPage() const { return m_clientListPage; }
    QPushButton* getBackButton() const { return m_backButton; }
    QLabel* getConnectionStatusLabel() const { return m_connectionStatusLabel; }
    QPushButton* getConnectToggleButton() const { return m_connectToggleButton; }
    QPushButton* getSettingsButton() const { return m_settingsButton; }
    int getInnerContentGap() const;

private slots:
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& error);
    void onClientListReceived(const QList<ClientInfo>& clients);
    void onRegistrationConfirmed(const ClientInfo& clientInfo);
    void onClientSelectionChanged();
    void onClientItemClicked(QListWidgetItem* item);
    void adjustClientListHeight();
    void updateConnectionStatus();
    void onEnableDisableClicked();
    void attemptReconnect();
    
    // Screen view slots
    void onBackToClientListClicked();
    void onScreensInfoReceived(const ClientInfo& clientInfo);
    void onWatchStatusChanged(bool watched);
    void onDataRequestReceived();
    
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
    // Dragging support for frameless window (macOS/Windows)
    bool m_dragging = false;
    QPoint m_dragStartGlobal;
    QPoint m_windowStartPos;
    // macOS traffic lights state
    enum class TrafficLightsMode { NoFocus, WindowHover, ButtonHover };
    void updateTrafficLightsIcons(TrafficLightsMode mode);
    TrafficLightsMode m_trafficLightsMode = TrafficLightsMode::NoFocus;
    bool m_anyTrafficLightHovered = false;

private:
    void setupUI();
    void setupTrayIcon();
    void setupConnections();
    void updateStylesheetsForTheme();
    void createClientListPage();
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
    void updateClientList(const QList<ClientInfo>& clients);
    void setUIEnabled(bool enabled);
    void showTrayMessage(const QString& title, const QString& message);
    void applyAnimationDurations();
    
    // Screen view methods
    void showScreenView(const ClientInfo& client);
    void showClientListView();
    void createRemoteClientInfoContainer(); // Create grouped container for remote client info
    void initializeRemoteClientInfoInTopBar(); // Initialize remote client info in top bar
    void createLocalClientInfoContainer(); // Create grouped container for local client info (You + network status)
    void setLocalNetworkStatus(const QString& status); // Update local network status
    QWidget* createScreenWidget(const ScreenInfo& screen, int index);
    void updateVolumeIndicator();
    void setRemoteConnectionStatus(const QString& status);
    // watch management handled by WatchManager component now
    // Manage presence of the volume indicator in the top bar layout
    void removeVolumeIndicatorFromLayout();
    void addVolumeIndicatorToLayout();
    // Manage presence of the remote status (and its leading separator) in the top bar layout
    void removeRemoteStatusFromLayout();
    void addRemoteStatusToLayout();

    // UI Components
    QWidget* m_centralWidget;
    QVBoxLayout* m_mainLayout;
    QStackedWidget* m_stackedWidget;
    QWidget* m_connectionBar; // container widget for top connection layout
    
    // Client list page
    QWidget* m_clientListPage;
    
    // Connection section
    QHBoxLayout* m_connectionLayout;
    QPushButton* m_settingsButton;
    QPushButton* m_connectToggleButton;
    QLabel* m_connectionStatusLabel;
    
    // Client list section
    QLabel* m_clientListLabel;
    QListWidget* m_clientListWidget;
    
    // Selected client info
    QLabel* m_selectedClientLabel;
    
    // Screen view section
    QWidget* m_screenViewWidget;
    QVBoxLayout* m_screenViewLayout;
    QLabel* m_clientNameLabel;
    QLabel* m_remoteConnectionStatusLabel;
    // Top bar contextual title (e.g., "Connected Clients" on client list page)
    QLabel* m_pageTitleLabel = nullptr;
    QWidget* m_remoteClientInfoContainer = nullptr; // Container for hostname, status, volume
    QFrame* m_remoteInfoSep2 = nullptr; // Trailing separator before volume indicator
    QFrame* m_remoteInfoSep1 = nullptr; // Leading separator before remote status
    QWidget* m_localClientInfoContainer = nullptr; // Container for "You" and network status
    QLabel* m_localClientTitleLabel = nullptr; // "You" label in local client container
    QLabel* m_localNetworkStatusLabel = nullptr; // Network status in local client container
    // Canvas container keeps border visible; inside we switch between spinner and canvas
    QWidget* m_canvasContainer;
    QStackedWidget* m_canvasStack;
    ScreenCanvas* m_screenCanvas;
    QLabel* m_volumeIndicator;
    SpinnerWidget* m_loadingSpinner;
    QPushButton* m_uploadButton;
    bool m_uploadButtonInOverlay = false; // Track if upload button is in overlay (uses custom styling)
    QPushButton* m_backButton;
    // Frameless window custom controls (macOS/Windows)
    QPushButton* m_btnClose = nullptr;
    QPushButton* m_btnMinimize = nullptr;
    QPushButton* m_btnMaximize = nullptr;
    QFont m_uploadButtonDefaultFont;
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

    // Menu and actions
    QMenu* m_fileMenu;
    QMenu* m_helpMenu;
    QAction* m_exitAction;
    QAction* m_aboutAction;
    
    // System tray
    QSystemTrayIcon* m_trayIcon;
    
    // Backend
    WebSocketClient* m_webSocketClient;
    QList<ClientInfo> m_availableClients;
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
    // Track currently uploading media by unique mediaId (no more path/name-based tracking)
    QSet<QString> m_mediaIdsBeingUploaded;
    bool m_uploadSignalsConnected = false;
    // Map upload fileId <-> mediaId for per-file progress tracking
    QHash<QString, QString> m_mediaIdByFileId;
    // Direct mapping from fileId to media item pointers (multiple media can share same fileId)
    QHash<QString, QList<ResizableMediaBase*>> m_itemsByFileId;
    // Deterministic order of files for the current upload session (matches sender streaming order)
    QStringList m_currentUploadFileOrder;
    // Files that the server has acknowledged as fully received in the current session
    QSet<QString> m_serverCompletedFileIds;
    WatchManager* m_watchManager = nullptr;   // extracted watch logic
    ScreenNavigationManager* m_navigationManager = nullptr; // new navigation component
    FileWatcher* m_fileWatcher = nullptr; // monitors source files for deletion
    // Canvas reveal state: ensure fade-in/recenter happen only once per selected client
    bool m_canvasRevealedForCurrentClient = false;
    // Preserve the current viewport (zoom/pan) across temporary connection losses
    bool m_preserveViewportOnReconnect = false;

private slots:
    void onGenericMessageReceived(const QJsonObject& message);
    // Upload-specific progress/finish now managed by UploadManager

private:
    void updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles);
};

#endif // MAINWINDOW_H
