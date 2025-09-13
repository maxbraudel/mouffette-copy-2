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
#include "WebSocketClient.h"
#include "ClientInfo.h"

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
QT_END_NAMESPACE
class QMimeData; // fwd declare for drag preview helpers
class QMediaPlayer;
class QVideoSink;
class QAudioOutput;

class SpinnerWidget; // forward declaration for custom loading spinner
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QProcess; // fwd decl to avoid including in header
// using QStackedWidget for canvas container switching

// Custom screen canvas widget with zoom and pan capabilities
class ScreenCanvas : public QGraphicsView {
    Q_OBJECT

public:
    explicit ScreenCanvas(QWidget* parent = nullptr);
    void setScreens(const QList<ScreenInfo>& screens);
    void clearScreens();
    void recenterWithMargin(int marginPx = 33);
    // Configure fade-in duration (ms) for drag preview appearance
    void setDragPreviewFadeDurationMs(int ms) { m_dragPreviewFadeMs = qMax(0, ms); }
    // Configure fade-in duration (ms) for video controls when they appear
    void setVideoControlsFadeDurationMs(int ms) { m_videoControlsFadeMs = qMax(0, ms); }
    // Remote cursor visualization
    void updateRemoteCursor(int globalX, int globalY);
    void hideRemoteCursor();
    // Config: sizes of media resize handles (px, item coordinates)
    void setMediaHandleSelectionSizePx(int px); // hit area for grabbing
    void setMediaHandleVisualSizePx(int px);    // drawn square size
    // Back-compat helper: set both at once
    void setMediaHandleSizePx(int px);
    // Screen border thickness (in pixels). Changing this updates existing screen items.
    void setScreenBorderWidthPx(int px);

signals:

protected:
    bool gestureEvent(QGestureEvent* event);
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    // Drag & drop images
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QGraphicsScene* m_scene;
    QList<QGraphicsRectItem*> m_screenItems;
    QList<ScreenInfo> m_screens;
    bool m_panning;
    QPoint m_lastPanPoint;
    QPoint m_lastMousePos;
    // If an overlay (controls) handled the mouse press, swallow the full click sequence
    bool m_overlayMouseDown = false;
    // macOS: track native pinch session to avoid handling two-finger scroll simultaneously
    bool m_nativePinchActive = false;
    QTimer* m_nativePinchGuardTimer = nullptr;
    // Momentum suppression after recentring: ignore decaying inertial wheel deltas
    bool m_ignorePanMomentum = false;     // active when we just centered
    bool m_momentumPrimed = false;        // first delta captured as baseline
    double m_lastMomentumMag = 0.0;       // last observed |delta| magnitude
    QPoint m_lastMomentumDelta;           // last observed raw delta vector
    QElapsedTimer m_momentumTimer;        // time since suppression was (re)started
    // Remote cursor overlay
    QGraphicsEllipseItem* m_remoteCursorDot = nullptr;
    // Unified scale factor used to lay out screens and to scale dropped media (scene pixels per device pixel)
    double m_scaleFactor = 0.2;
    // Resize handle sizes for media items
    int m_mediaHandleSelectionSizePx = 30; // large hit area by default
    int m_mediaHandleVisualSizePx = 12;     // smaller visual indicator
    // Screen border thickness (px). Applied fully inside the screen rect.
    int m_screenBorderWidthPx = 1;
    // Drag-and-drop live preview
    QGraphicsItem* m_dragPreviewItem = nullptr; // pixmap preview during drag
    QSize m_dragPreviewBaseSize; // logical base size in scene units before scale
    bool m_dragPreviewIsVideo = false;
    QPixmap m_dragPreviewPixmap; // for images or rendered video placeholder
    bool m_dragCursorHidden = false; // hide cursor while dragging over canvas
    QPointF m_dragPreviewLastScenePos; // last known scene position for preview center
    // Fade-in for drag preview
    int m_dragPreviewFadeMs = 180; // configurable duration in milliseconds
    class QVariantAnimation* m_dragPreviewFadeAnim = nullptr; // forward-declared type
    qreal m_dragPreviewTargetOpacity = 0.85; // final opacity during drag
    // Fade-in for video controls
    int m_videoControlsFadeMs = 140; // configurable duration in milliseconds
    // Live video thumbnail probe
    QMediaPlayer* m_dragPreviewPlayer = nullptr;
    QVideoSink* m_dragPreviewSink = nullptr;
    QAudioOutput* m_dragPreviewAudio = nullptr;
    bool m_dragPreviewGotFrame = false;
    QTimer* m_dragPreviewFallbackTimer = nullptr; // fallback to QMediaPlayer if fast thumbnail is slow
    void ensureDragPreview(const QMimeData* mime);
    void updateDragPreviewPos(const QPointF& scenePos);
    void clearDragPreview();
    QPixmap makeVideoPlaceholderPixmap(const QSize& pxSize);
    void startVideoPreviewProbe(const QString& localFilePath);
    void startVideoPreviewProbeFallback(const QString& localFilePath);
    void stopVideoPreviewProbe();
    void startDragPreviewFadeIn();
    void stopDragPreviewFade();
    void onFastVideoThumbnailReady(const QImage& img);
    
    void createScreenItems();
    QGraphicsRectItem* createScreenItem(const ScreenInfo& screen, int index, const QRectF& position);
    QMap<int, QRectF> calculateCompactPositions(double scaleFactor, double hSpacing, double vSpacing) const;
    QRectF screensBoundingRect() const;
    void zoomAroundViewportPos(const QPointF& vpPos, qreal factor);
    void ensureZOrder();
};

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
    void onSendMediaClicked();
    void onScreensInfoReceived(const ClientInfo& clientInfo);
    void onWatchStatusChanged(bool watched);
    
    // System tray slots
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void setupUI();
    void createClientListPage();
    void createScreenViewPage();
    void setupMenuBar();
    void setupSystemTray();
    void connectToServer();
    void showSettingsDialog();
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
    QWidget* createScreenWidget(const ScreenInfo& screen, int index);
    void updateVolumeIndicator();
    void startWatchingSelectedClient();
    void stopWatchingCurrentClient();

    // UI Components
    QWidget* m_centralWidget;
    QVBoxLayout* m_mainLayout;
    QStackedWidget* m_stackedWidget;
    
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
    // Canvas container keeps border visible; inside we switch between spinner and canvas
    QWidget* m_canvasContainer;
    QStackedWidget* m_canvasStack;
    ScreenCanvas* m_screenCanvas;
    QLabel* m_volumeIndicator;
    SpinnerWidget* m_loadingSpinner;
    QPushButton* m_sendButton;
    QPushButton* m_backButton;
    // Loader/content animations
    QTimer* m_loaderDelayTimer = nullptr;
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
    QString m_watchedClientId;
    
    // Constants
    static const QString DEFAULT_SERVER_URL;
    
    // Volume monitoring (to avoid 1s spikes)
    int m_cachedSystemVolume = -1;        // last known value (0-100), -1 unknown
#ifdef Q_OS_MACOS
    QProcess* m_volProc = nullptr;       // reused for async osascript calls
    QTimer* m_volTimer = nullptr;        // polls in background
#endif
};

#endif // MAINWINDOW_H
