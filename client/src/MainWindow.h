#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
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
// using QStackedWidget for canvas container switching

// Forward declaration of extracted ScreenCanvas
class ScreenCanvas;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& error);
    void onClientListReceived(const QList<ClientInfo>& clients);
    void onRegistrationConfirmed(const ClientInfo& clientInfo);
    void onClientSelectionChanged();
    void onClientItemClicked(QListWidgetItem* item);
    void updateConnectionStatus();
    void onEnableDisableClicked();
    void attemptReconnect();
    
    // Screen view slots
    void onBackToClientListClicked();
    void onScreensInfoReceived(const ClientInfo& clientInfo);
    void onWatchStatusChanged(bool watched);
    
    // System tray slots
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void onUploadButtonClicked();
    void showSettingsDialog();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
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
    void initializeRemoteClientInfoInTopBar(); // Initialize container in top bar permanently
    QWidget* createScreenWidget(const ScreenInfo& screen, int index);
    void updateVolumeIndicator();
    void setRemoteConnectionStatus(const QString& status);
    // watch management handled by WatchManager component now

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
    QLabel* m_noClientsLabel;
    
    // Selected client info
    QLabel* m_selectedClientLabel;
    
    // Screen view section
    QWidget* m_screenViewWidget;
    QVBoxLayout* m_screenViewLayout;
    QLabel* m_clientNameLabel;
    QLabel* m_remoteConnectionStatusLabel;
    QWidget* m_remoteClientInfoContainer = nullptr; // Container for hostname, status, volume
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
    // Direct mapping from fileId to media item pointer for efficient progress updates
    QHash<QString, ResizableMediaBase*> m_itemByFileId;
    WatchManager* m_watchManager = nullptr;   // extracted watch logic
    ScreenNavigationManager* m_navigationManager = nullptr; // new navigation component
    // Canvas reveal state: ensure fade-in/recenter happen only once per selected client
    bool m_canvasRevealedForCurrentClient = false;

private slots:
    void onGenericMessageReceived(const QJsonObject& message);
    // Upload-specific progress/finish now managed by UploadManager

private:
    void updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles);
};

#endif // MAINWINDOW_H
