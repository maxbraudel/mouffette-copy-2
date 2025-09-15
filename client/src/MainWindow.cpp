#include "MainWindow.h"
#include "OverlayPanels.h"
#include "UploadManager.h"
#include "WatchManager.h"
#include "ScreenNavigationManager.h"
#include "SpinnerWidget.h"
#include "RoundedRectItem.h"
#include "MediaItems.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QHostInfo>
#include <QGuiApplication>
#include <QDebug>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QStyleOption>
#include <algorithm>
#include <cmath>
#include <QDialog>
#include <QLineEdit>
#include <QNativeGestureEvent>
#include <QCursor>
#include <QRandomGenerator>
#include <QPainter>
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
#include <QGraphicsPixmapItem>
#include <QGraphicsSceneMouseEvent>
#include <QPen>
#include <QBrush>
#include <QUrl>
#include <QImage>
#include <QPixmap>
#include <QGraphicsTextItem>
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QtSvgWidgets/QGraphicsSvgItem>
#include <QtSvg/QSvgRenderer>
#include <QPainterPathStroker>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QUuid>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <climits>
#include <memory>
#ifdef Q_OS_MACOS
#include "MacCursorHider.h"
#include "MacVideoThumbnailer.h"
#endif
#include <QGraphicsItem>
#include <QSet>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QMediaMetaData>
#include <QVideoFrame>
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

const QString MainWindow::DEFAULT_SERVER_URL = "ws://192.168.0.188:8080";
// Z-ordering constants used throughout the scene
namespace {
constexpr qreal Z_SCREENS = -1000.0;
constexpr qreal Z_MEDIA_BASE = 1.0;
constexpr qreal Z_REMOTE_CURSOR = 10000.0;
constexpr qreal Z_SCENE_OVERLAY = 12000.0; // above all scene content
}

// (Upload progress & finished handlers removed; handled by UploadManager)

void MainWindow::onGenericMessageReceived(const QJsonObject& message) {
    // Non-upload generic handling placeholder (upload handled by UploadManager now)
    Q_UNUSED(message);
}

// Wire WebSocketClient signals after creation
// Look for existing connects; add upload-related ones

// Ensure all fade animations respect the configured duration
void MainWindow::applyAnimationDurations() {
    if (m_spinnerFade) m_spinnerFade->setDuration(m_loaderFadeDurationMs);
    if (m_canvasFade) m_canvasFade->setDuration(m_fadeDurationMs);
    if (m_volumeFade) m_volumeFade->setDuration(m_fadeDurationMs);
}

// SpinnerWidget now defined in SpinnerWidget.h

// Simple rounded-rectangle graphics item with a settable rect and radius

// Media item classes moved to MediaItems.h/.cpp

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_centralWidget(nullptr)
    , m_webSocketClient(new WebSocketClient(this)) // Initialize WebSocket client
    , m_statusUpdateTimer(new QTimer(this))
    , m_trayIcon(nullptr)
    , m_screenViewWidget(nullptr)
    , m_screenCanvas(nullptr)
    , m_ignoreSelectionChange(false)
    , m_displaySyncTimer(new QTimer(this))
    , m_canvasStack(new QStackedWidget(this)) // Initialize m_canvasStack
    , m_uploadManager(new UploadManager(this))
    , m_watchManager(new WatchManager(this))
{
    // Check if system tray is available
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(this, "System Tray",
                             "System tray is not available on this system.");
    }
    
    setupUI();
    setupMenuBar();
    setupSystemTray();
    setupVolumeMonitoring();
    
    // Connect WebSocket signals
    connect(m_webSocketClient, &WebSocketClient::connected, this, &MainWindow::onConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_webSocketClient, &WebSocketClient::clientListReceived, this, &MainWindow::onClientListReceived);
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    connect(m_webSocketClient, &WebSocketClient::screensInfoReceived, this, &MainWindow::onScreensInfoReceived);
    connect(m_webSocketClient, &WebSocketClient::watchStatusChanged, this, &MainWindow::onWatchStatusChanged);
    // Upload manager wiring
    m_uploadManager->setWebSocketClient(m_webSocketClient);
    m_watchManager->setWebSocketClient(m_webSocketClient);
    connect(m_webSocketClient, &WebSocketClient::uploadProgressReceived,
            m_uploadManager, &UploadManager::onUploadProgress);
    connect(m_webSocketClient, &WebSocketClient::uploadFinishedReceived,
            m_uploadManager, &UploadManager::onUploadFinished);
    connect(m_webSocketClient, &WebSocketClient::unloadedReceived,
            m_uploadManager, &UploadManager::onUnloadedRemote);
    // Generic message tap for target-side upload handling now handled by manager
    connect(m_webSocketClient, &WebSocketClient::messageReceived, this, [this](const QJsonObject& obj){
        m_uploadManager->handleIncomingMessage(obj);
        onGenericMessageReceived(obj); // preserve any other logic later
    });
    connect(m_uploadManager, &UploadManager::uploadProgress, this, [this](int percent, int filesCompleted, int totalFiles){
        Q_UNUSED(totalFiles);
        if (!m_uploadButton) return;
        // Show progress; keep monospace for stability
        m_uploadButton->setText(QString("Downloading (%1/%2) %3%")
                                    .arg(filesCompleted)
                                    .arg(totalFiles)
                                    .arg(percent));
    });
    connect(m_uploadManager, &UploadManager::uploadFinished, this, [this](){
        if (!m_uploadButton) return;
        m_uploadButton->setChecked(true);
        m_uploadButton->setText("Unload medias");
        m_uploadButton->setStyleSheet("QPushButton { padding: 12px 18px; font-weight: bold; background-color: #16a34a; color: white; border-radius: 5px; } QPushButton:checked { background-color: #15803d; }");
        m_uploadButton->setFont(m_uploadButtonDefaultFont);
    });
    connect(m_uploadManager, &UploadManager::uiStateChanged, this, [this](){
        if (!m_uploadButton) return;
        if (m_uploadManager->hasActiveUpload()) {
            m_uploadButton->setChecked(true);
            m_uploadButton->setText("Unload medias");
        } else if (m_uploadManager->isUploading()) {
            // state already represented during progress
        } else if (m_uploadManager->isCancelling()) {
            m_uploadButton->setText("Cancelling…");
        } else {
            m_uploadButton->setChecked(false);
            m_uploadButton->setText("Upload to Client");
            m_uploadButton->setStyleSheet("QPushButton { padding: 12px 18px; font-weight: bold; background-color: #666; color: white; border-radius: 5px; } QPushButton:checked { background-color: #444; }");
            m_uploadButton->setFont(m_uploadButtonDefaultFont);
            m_uploadButton->setEnabled(true);
        }
    });
    connect(m_webSocketClient, &WebSocketClient::dataRequestReceived, this, [this]() {
        // Server requested immediate state (on first watch or refresh)
        m_webSocketClient->sendStateSnapshot(getLocalScreenInfo(), getSystemVolumePercent());
    });
    
    // Setup status update timer
    m_statusUpdateTimer->setInterval(1000); // Update every second
    connect(m_statusUpdateTimer, &QTimer::timeout, this, &MainWindow::updateConnectionStatus);
    m_statusUpdateTimer->start();

    // Debounced display sync timer
    m_displaySyncTimer->setSingleShot(true);
    m_displaySyncTimer->setInterval(300); // debounce bursts of screen change signals
    connect(m_displaySyncTimer, &QTimer::timeout, this, [this]() {
        if (m_webSocketClient->isConnected() && m_isWatched) syncRegistration();
    });

    // Listen to display changes to keep server-side screen info up-to-date
    auto connectScreenSignals = [this](QScreen* s) {
        connect(s, &QScreen::geometryChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::availableGeometryChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::physicalDotsPerInchChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::primaryOrientationChanged, this, [this]() { m_displaySyncTimer->start(); });
    };
    for (QScreen* s : QGuiApplication::screens()) connectScreenSignals(s);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, connectScreenSignals](QScreen* s) {
        connectScreenSignals(s);
        m_displaySyncTimer->start();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
        m_displaySyncTimer->start();
    });

    // Smart reconnection system with exponential backoff
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectAttempts = 0;
    m_maxReconnectDelay = 60000; // Max 60 seconds between attempts
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::attemptReconnect);

        // (cleaned) avoid duplicate hookups
#ifdef Q_OS_MACOS
    // Use a normal window on macOS so the title bar and traffic lights have standard size.
    // Avoid Qt::Tool/WA_MacAlwaysShowToolWindow which force a tiny utility-chrome window.
#endif
    
    // Initially disable UI until connected
    setUIEnabled(false);
    
    // Start minimized to tray and auto-connect
    hide();
    connectToServer();
}

void MainWindow::showScreenView(const ClientInfo& client) {
    if (m_clientNameLabel)
        m_clientNameLabel->setText(QString("%1 (%2)").arg(client.getMachineName()).arg(client.getPlatform()));
    if (m_navigationManager) m_navigationManager->showScreenView(client);
}

void MainWindow::showClientListView() {
    if (m_navigationManager) m_navigationManager->showClientList();
    m_ignoreSelectionChange = true;
    m_clientListWidget->clearSelection();
    m_ignoreSelectionChange = false;
}

void MainWindow::onClientItemClicked(QListWidgetItem* item) {
    if (!item) return;
    int index = m_clientListWidget->row(item);
    if (index >= 0 && index < m_availableClients.size()) {
        const ClientInfo& client = m_availableClients[index];
        m_selectedClient = client;
        // Switch to screen view first with any cached info for immediate feedback
        showScreenView(client);
        // Then request fresh screens info on-demand
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            m_webSocketClient->requestScreens(client.getId());
        }
    }
}

void MainWindow::updateVolumeIndicator() {
    int vol = m_selectedClient.getVolumePercent();
    QString text;
    if (vol >= 0) {
        QString icon = (vol == 0) ? "🔇" : (vol < 34 ? "🔈" : (vol < 67 ? "🔉" : "🔊"));
        text = QString("%1 %2%").arg(icon).arg(vol);
    } else {
        text = QString("🔈 --");
    }
    m_volumeIndicator->setText(text);
}

void MainWindow::onBackToClientListClicked() {
    showClientListView();
}

void MainWindow::onSendMediaClicked() {
    // Placeholder implementation
    QMessageBox::information(this, "Send Media", 
        QString("Sending media to %1's screens...\n\nThis feature will be implemented in the next phase.")
        .arg(m_selectedClient.getMachineName()));
}

// Upload/Unload button handler
void MainWindow::onUploadButtonClicked() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected() || m_selectedClient.getId().isEmpty()) {
        QMessageBox::warning(this, "Upload", "Not connected or no target selected");
        return;
    }
    // Inform manager of target and trigger
    m_uploadManager->setTargetClientId(m_selectedClient.getId());
    QGraphicsScene* scene = m_screenCanvas ? m_screenCanvas->scene() : nullptr;
    // Prepare button immediate visual state for starting upload if idle
    if (!m_uploadManager->hasActiveUpload() && !m_uploadManager->isUploading()) {
        m_uploadButton->setCheckable(true);
        m_uploadButton->setChecked(true);
        m_uploadButton->setText("Preparing download");
        QFont mono = m_uploadButtonDefaultFont;
        mono.setStyleHint(QFont::Monospace);
        mono.setFixedPitch(true);
#if defined(Q_OS_MAC)
        mono.setFamily("Menlo");
#else
        mono.setFamily("Courier New");
#endif
        m_uploadButton->setFont(mono);
        m_uploadButton->setStyleSheet("QPushButton { padding: 12px 18px; font-weight: bold; background-color: #2d6cdf; color: white; border-radius: 5px; } QPushButton:checked { background-color: #1f4ea8; }");
    }
    // Gather files explicitly from scene (local knowledge of ResizableMediaBase)
    QVector<UploadFileInfo> files;
    if (scene) {
        for (QGraphicsItem* it : scene->items()) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                QString path = media->sourcePath();
                if (path.isEmpty()) continue;
                QFileInfo info(path);
                if (!info.exists() || !info.isFile()) continue;
                UploadFileInfo fi { QUuid::createUuid().toString(QUuid::WithoutBraces), path, info.fileName(), info.size() };
                files.push_back(fi);
            }
        }
    }
    m_uploadManager->toggleUpload(files);
}



bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // Swallow spacebar outside of the canvas to prevent accidental activations,
    // but let it reach the canvas for the recenter shortcut.
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space) {
            QWidget* w = qobject_cast<QWidget*>(obj);
            bool onCanvas = false;
            if (m_screenCanvas) {
                QWidget* canvasWidget = m_screenCanvas;
                onCanvas = (w == canvasWidget) || (w && canvasWidget->isAncestorOf(w));
            }
            if (!onCanvas) {
                return true; // consume outside the canvas
            } else {
                // Recenter shortcut on canvas regardless of exact focus widget
                m_screenCanvas->recenterWithMargin(33);
                return true; // consume
            }
        }
    }
    // Apply rounded clipping to canvas widgets so border radius is visible
    if (obj == m_canvasContainer || obj == m_canvasStack || (m_screenCanvas && obj == m_screenCanvas->viewport())) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            if (QWidget* w = qobject_cast<QWidget*>(obj)) {
                const int r = 5; // match clients list radius
                QPainterPath path;
                path.addRoundedRect(w->rect(), r, r);
                QRegion mask(path.toFillPolygon().toPolygon());
                w->setMask(mask);
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ScreenCanvas implementation
ScreenCanvas::ScreenCanvas(QWidget* parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
    , m_panning(false)
{
    setScene(m_scene);
    setDragMode(QGraphicsView::NoDrag);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Use the application palette for consistent theming with the client list container
    const QBrush bgBrush = palette().brush(QPalette::Base);
    setBackgroundBrush(bgBrush);        // outside the scene rect
    if (m_scene) m_scene->setBackgroundBrush(bgBrush); // inside the scene rect
    setFrameShape(QFrame::NoFrame);
    setRenderHint(QPainter::Antialiasing);
    // Use manual anchoring logic for consistent behavior across platforms
    setTransformationAnchor(QGraphicsView::NoAnchor);
    // When the view is resized, keep the view-centered anchor (we also recenter explicitly on window resize)
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    // Enable drag & drop
    setAcceptDrops(true);
    
    // Enable mouse tracking for panning
    setMouseTracking(true);
    m_lastMousePos = QPoint(0, 0);
    // Pinch guard timer prevents two-finger scroll handling during active pinch
    m_nativePinchGuardTimer = new QTimer(this);
    m_nativePinchGuardTimer->setSingleShot(true);
    m_nativePinchGuardTimer->setInterval(60); // short grace period after last pinch delta
    connect(m_nativePinchGuardTimer, &QTimer::timeout, this, [this]() {
        m_nativePinchActive = false;
    });
    
    // Enable pinch gesture (except on macOS where we'll rely on NativeGesture to avoid double handling)
#ifndef Q_OS_MACOS
    grabGesture(Qt::PinchGesture);
#endif
    // Remote cursor overlay (hidden by default)
    m_remoteCursorDot = m_scene->addEllipse(0, 0, 10, 10, QPen(QColor(74, 144, 226)), QBrush(Qt::white));
    if (m_remoteCursorDot) {
        m_remoteCursorDot->setZValue(10000);
        m_remoteCursorDot->setVisible(false);
    }
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    clearScreens();
    createScreenItems();
    
    // Keep a large scene for free movement and center on screens
    const double LARGE_SCENE_SIZE = 100000.0;
    QRectF sceneRect(-LARGE_SCENE_SIZE/2, -LARGE_SCENE_SIZE/2, LARGE_SCENE_SIZE, LARGE_SCENE_SIZE);
    m_scene->setSceneRect(sceneRect);
    if (!m_screens.isEmpty()) {
        recenterWithMargin(53);
    }
}

void ScreenCanvas::clearScreens() {
    // Clear existing screen items
    for (QGraphicsRectItem* item : m_screenItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_screenItems.clear();
}

void ScreenCanvas::createScreenItems() {
    const double SCALE_FACTOR = m_scaleFactor; // Use member scale factor so drops match
    const double H_SPACING = 0.0;  // No horizontal gap between adjacent screens
    const double V_SPACING = 5.0;  // Keep a small vertical gap between rows
    
    // Calculate compact positioning
    QMap<int, QRectF> compactPositions = calculateCompactPositions(SCALE_FACTOR, H_SPACING, V_SPACING);
    
    for (int i = 0; i < m_screens.size(); ++i) {
        const ScreenInfo& screen = m_screens[i];
        QGraphicsRectItem* screenItem = createScreenItem(screen, i, compactPositions[i]);
        m_screenItems.append(screenItem);
        m_scene->addItem(screenItem);
    }
    ensureZOrder();
}

void ScreenCanvas::updateRemoteCursor(int globalX, int globalY) {
    if (!m_remoteCursorDot || m_screens.isEmpty() || m_screenItems.size() != m_screens.size()) {
        if (m_remoteCursorDot) m_remoteCursorDot->setVisible(false);
        return;
    }
    int idx = -1;
    int localX = 0, localY = 0;
    for (int i = 0; i < m_screens.size(); ++i) {
        const auto& s = m_screens[i];
        if (globalX >= s.x && globalX < s.x + s.width && globalY >= s.y && globalY < s.y + s.height) {
            idx = i;
            localX = globalX - s.x;
            localY = globalY - s.y;
            break;
        }
    }
    if (idx < 0) {
        m_remoteCursorDot->setVisible(false);
        return;
    }
    QGraphicsRectItem* item = m_screenItems[idx];
    if (!item) { m_remoteCursorDot->setVisible(false); return; }
    const QRectF r = item->rect();
    if (m_screens[idx].width <= 0 || m_screens[idx].height <= 0 || r.width() <= 0 || r.height() <= 0) { m_remoteCursorDot->setVisible(false); return; }
    const double fx = static_cast<double>(localX) / static_cast<double>(m_screens[idx].width);
    const double fy = static_cast<double>(localY) / static_cast<double>(m_screens[idx].height);
    const double sceneX = r.left() + fx * r.width();
    const double sceneY = r.top() + fy * r.height();
    const double dotSize = 10.0;
    m_remoteCursorDot->setRect(sceneX - dotSize/2.0, sceneY - dotSize/2.0, dotSize, dotSize);
    m_remoteCursorDot->setVisible(true);
}

void ScreenCanvas::hideRemoteCursor() {
    if (m_remoteCursorDot) m_remoteCursorDot->setVisible(false);
}

void ScreenCanvas::ensureZOrder() {
    // Put screens at a low Z, remote cursor very high, media in between
    for (QGraphicsRectItem* r : m_screenItems) {
        if (r) r->setZValue(Z_SCREENS);
    }
    if (m_remoteCursorDot) m_remoteCursorDot->setZValue(Z_REMOTE_CURSOR);
    if (!m_scene) return;
    // Normalize media base Z so overlays (set to ~9000) always win
    for (QGraphicsItem* it : m_scene->items()) {
        auto* media = dynamic_cast<ResizableMediaBase*>(it);
    if (media) media->setZValue(Z_MEDIA_BASE);
    }
}

void ScreenCanvas::setMediaHandleSelectionSizePx(int px) {
    m_mediaHandleSelectionSizePx = qMax(4, px);
    if (!m_scene) return;
    const QList<QGraphicsItem*> sel = m_scene->selectedItems();
    for (QGraphicsItem* it : sel) {
        if (auto* rp = dynamic_cast<ResizablePixmapItem*>(it)) {
            rp->setHandleSelectionSize(m_mediaHandleSelectionSizePx);
        } else if (auto* rv = dynamic_cast<ResizableMediaBase*>(it)) {
            rv->setHandleSelectionSize(m_mediaHandleSelectionSizePx);
        }
    }
}

void ScreenCanvas::setMediaHandleVisualSizePx(int px) {
    m_mediaHandleVisualSizePx = qMax(4, px);
    if (!m_scene) return;
    const QList<QGraphicsItem*> sel = m_scene->selectedItems();
    for (QGraphicsItem* it : sel) {
        if (auto* rp = dynamic_cast<ResizablePixmapItem*>(it)) {
            rp->setHandleVisualSize(m_mediaHandleVisualSizePx);
        } else if (auto* rv = dynamic_cast<ResizableMediaBase*>(it)) {
            rv->setHandleVisualSize(m_mediaHandleVisualSizePx);
        }
    }
}

void ScreenCanvas::setMediaHandleSizePx(int px) {
    setMediaHandleVisualSizePx(px);
    setMediaHandleSelectionSizePx(px);
}

// Drag & drop handlers
void ScreenCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) {
    ensureDragPreview(event->mimeData());
    m_dragPreviewLastScenePos = mapToScene(event->position().toPoint());
    updateDragPreviewPos(m_dragPreviewLastScenePos);
    if (!m_dragCursorHidden) {
#ifdef Q_OS_MACOS
        MacCursorHider::hide();
#else
        QApplication::setOverrideCursor(Qt::BlankCursor);
#endif
        m_dragCursorHidden = true;
    }
        event->acceptProposedAction();
    } else {
        QGraphicsView::dragEnterEvent(event);
    }
}

void ScreenCanvas::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) {
    ensureDragPreview(event->mimeData());
    m_dragPreviewLastScenePos = mapToScene(event->position().toPoint());
    updateDragPreviewPos(m_dragPreviewLastScenePos);
        event->acceptProposedAction();
    } else {
        QGraphicsView::dragMoveEvent(event);
    }
}

void ScreenCanvas::dragLeaveEvent(QDragLeaveEvent* event) {
    Q_UNUSED(event);
    clearDragPreview();
    if (m_dragCursorHidden) {
#ifdef Q_OS_MACOS
    MacCursorHider::show();
#else
    QApplication::restoreOverrideCursor();
#endif
    m_dragCursorHidden = false;
    }
}

void ScreenCanvas::dropEvent(QDropEvent* event) {
    QImage image;
    QString filename;
    QString droppedPath;
    if (event->mimeData()->hasImage()) {
        image = qvariant_cast<QImage>(event->mimeData()->imageData());
        filename = "pasted-image";
    } else if (event->mimeData()->hasUrls()) {
        const auto urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            const QUrl& url = urls.first();
            const QString path = url.toLocalFile();
            if (!path.isEmpty()) {
                droppedPath = path;
                filename = QFileInfo(path).fileName();
                image.load(path); // may fail if it's a video
            }
        }
    }
    const QPointF scenePos = mapToScene(event->position().toPoint());
    if (!image.isNull()) {
        const double w = image.width() * m_scaleFactor;
        const double h = image.height() * m_scaleFactor;
        auto* item = new ResizablePixmapItem(QPixmap::fromImage(image), m_mediaHandleVisualSizePx, m_mediaHandleSelectionSizePx, filename);
        if (!droppedPath.isEmpty()) item->setSourcePath(droppedPath);
        item->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        item->setPos(scenePos.x() - w/2.0, scenePos.y() - h/2.0);
        if (image.width() > 0) item->setScale(w / image.width());
        m_scene->addItem(item);
        // Auto-select the newly dropped item
        if (m_scene) m_scene->clearSelection();
        item->setSelected(true);
    } else if (!droppedPath.isEmpty()) {
        // Decide if it's a video by extension
        const QString ext = QFileInfo(droppedPath).suffix().toLower();
        static const QSet<QString> kVideoExts = {"mp4","mov","m4v","avi","mkv","webm"};
        if (kVideoExts.contains(ext)) {
            // Create with placeholder logical size; adopt real size on first frame
            auto* vitem = new ResizableVideoItem(droppedPath, m_mediaHandleVisualSizePx, m_mediaHandleSelectionSizePx, filename, m_videoControlsFadeMs);
            vitem->setSourcePath(droppedPath);
            vitem->setInitialScaleFactor(m_scaleFactor);
            // Start with a neutral 1.0 scale on placeholder size and center approx.
            vitem->setScale(m_scaleFactor);
            const double w = 640.0 * m_scaleFactor;
            const double h = 360.0 * m_scaleFactor;
            vitem->setPos(scenePos.x() - w/2.0, scenePos.y() - h/2.0);
            m_scene->addItem(vitem);
            // Auto-select the newly dropped video item
            if (m_scene) m_scene->clearSelection();
            vitem->setSelected(true);
            // If a real preview frame exists, set it as poster after adding to the scene
            if (!m_dragPreviewPixmap.isNull()) {
                vitem->setExternalPosterImage(m_dragPreviewPixmap.toImage());
            }
        } else {
            QGraphicsView::dropEvent(event);
            return;
        }
    } else {
        QGraphicsView::dropEvent(event);
        return;
    }
    ensureZOrder();
    event->acceptProposedAction();
    // Keep the preview visible at the drop location briefly until the new item paints, then clear
    clearDragPreview();
    if (m_dragCursorHidden) {
#ifdef Q_OS_MACOS
    MacCursorHider::show();
#else
    QApplication::restoreOverrideCursor();
#endif
    m_dragCursorHidden = false;
    }
}

void ScreenCanvas::ensureDragPreview(const QMimeData* mime) {
    if (!m_scene) return;
    if (m_dragPreviewItem) return; // already created
    QPixmap preview;
    m_dragPreviewIsVideo = false;
    m_dragPreviewPixmap = QPixmap();
    m_dragPreviewBaseSize = QSize();
    // Try image data directly
    if (mime->hasImage()) {
        QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            preview = QPixmap::fromImage(img);
        }
    }
    // Or a local file URL
    if (preview.isNull() && mime->hasUrls()) {
        const auto urls = mime->urls();
        if (!urls.isEmpty()) {
            const QUrl& url = urls.first();
            const QString path = url.toLocalFile();
            if (!path.isEmpty()) {
                const QString ext = QFileInfo(path).suffix().toLower();
                static const QSet<QString> kVideoExts = {"mp4","mov","m4v","avi","mkv","webm"};
                if (kVideoExts.contains(ext)) {
                    m_dragPreviewIsVideo = true;
                    // Start background probe to fetch first frame ASAP; do not show any placeholder
                    startVideoPreviewProbe(path);
                    // Return early: we'll create the pixmap item when the first frame arrives
                    return;
                } else {
                    preview.load(path); // may fail if not an image
                }
            }
        }
    }
    if (preview.isNull()) return; // nothing to preview yet
    m_dragPreviewPixmap = preview;
    m_dragPreviewBaseSize = preview.size();
    // Create a QGraphicsPixmapItem as the preview
    auto* pmItem = new QGraphicsPixmapItem(preview);
    // Start from transparent and fade in
    pmItem->setOpacity(0.0);
    pmItem->setZValue(5000.0); // on top of everything during drag
    pmItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, false); // scale with scene factor
    // Apply initial scale to match our canvas scale factor
    pmItem->setScale(m_scaleFactor);
    m_scene->addItem(pmItem);
    m_dragPreviewItem = pmItem;
    startDragPreviewFadeIn();
}

void ScreenCanvas::updateDragPreviewPos(const QPointF& scenePos) {
    if (!m_dragPreviewItem) return;
    const QSize sz = m_dragPreviewBaseSize.isValid() ? m_dragPreviewBaseSize : QSize(320,180);
    const double w = sz.width() * m_scaleFactor;
    const double h = sz.height() * m_scaleFactor;
    m_dragPreviewItem->setPos(scenePos.x() - w/2.0, scenePos.y() - h/2.0);
}

void ScreenCanvas::clearDragPreview() {
    stopDragPreviewFade();
    if (m_dragPreviewItem && m_scene) {
        m_scene->removeItem(m_dragPreviewItem);
        delete m_dragPreviewItem;
    }
    m_dragPreviewItem = nullptr;
    m_dragPreviewBaseSize = QSize();
    m_dragPreviewPixmap = QPixmap();
    m_dragPreviewIsVideo = false;
    stopVideoPreviewProbe();
}

QPixmap ScreenCanvas::makeVideoPlaceholderPixmap(const QSize& pxSize) {
    QSize s = pxSize.isValid() ? pxSize : QSize(320, 180);
    QPixmap pm(s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg(0,0,0,180);
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    const qreal r = 8.0;
    p.drawRoundedRect(QRectF(0,0,s.width(), s.height()), r, r);
    // Draw a simple play triangle in the center
    const qreal triW = s.width() * 0.18;
    const qreal triH = triW * 1.0;
    QPointF c(s.width()/2.0, s.height()/2.0);
    QPolygonF tri;
    tri << QPointF(c.x() - triW/3.0, c.y() - triH/2.0)
        << QPointF(c.x() - triW/3.0, c.y() + triH/2.0)
        << QPointF(c.x() + triW*0.7, c.y());
    p.setBrush(QColor(255,255,255,230));
    p.drawPolygon(tri);
    p.end();
    return pm;
}

void ScreenCanvas::startVideoPreviewProbe(const QString& localFilePath) {
    stopVideoPreviewProbe();
    m_dragPreviewGotFrame = false;
#ifdef Q_OS_MACOS
    // Launch a very fast first-frame extraction via AVFoundation on a background thread
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    QString pathCopy = localFilePath;
    std::thread([this, pathCopy]() {
        QImage img = MacVideoThumbnailer::firstFrame(pathCopy);
        if (!img.isNull()) {
            QImage copy = img;
            QMetaObject::invokeMethod(this, [this, copy]() { onFastVideoThumbnailReady(copy); }, Qt::QueuedConnection);
        }
    }).detach();
    // Fallback: if fast path doesn't answer quickly, start the decoder-based probe
    m_dragPreviewFallbackTimer = new QTimer(this);
    m_dragPreviewFallbackTimer->setSingleShot(true);
    connect(m_dragPreviewFallbackTimer, &QTimer::timeout, this, [this, localFilePath]() {
        startVideoPreviewProbeFallback(localFilePath);
    });
    m_dragPreviewFallbackTimer->start(120); // short grace period
#else
    startVideoPreviewProbeFallback(localFilePath);
#endif
}

void ScreenCanvas::startVideoPreviewProbeFallback(const QString& localFilePath) {
    if (m_dragPreviewPlayer) return; // already running
    m_dragPreviewPlayer = new QMediaPlayer(this);
    m_dragPreviewAudio = new QAudioOutput(this);
    m_dragPreviewAudio->setMuted(true);
    m_dragPreviewPlayer->setAudioOutput(m_dragPreviewAudio);
    m_dragPreviewSink = new QVideoSink(this);
    m_dragPreviewPlayer->setVideoSink(m_dragPreviewSink);
    m_dragPreviewPlayer->setSource(QUrl::fromLocalFile(localFilePath));
    connect(m_dragPreviewSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f){
        if (m_dragPreviewGotFrame || !f.isValid()) return;
        QImage img = f.toImage();
        if (img.isNull()) return;
        m_dragPreviewGotFrame = true;
        QPixmap newPm = QPixmap::fromImage(img);
        if (newPm.isNull()) return;
        m_dragPreviewPixmap = newPm;
        m_dragPreviewBaseSize = newPm.size();
        if (!m_dragPreviewItem) {
            auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap);
            pmItem->setOpacity(0.0);
            pmItem->setZValue(5000.0);
            pmItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, false);
            pmItem->setScale(m_scaleFactor);
            m_scene->addItem(pmItem);
            m_dragPreviewItem = pmItem;
            updateDragPreviewPos(m_dragPreviewLastScenePos);
            startDragPreviewFadeIn();
        } else if (auto* pmItem = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) {
            pmItem->setPixmap(m_dragPreviewPixmap);
            updateDragPreviewPos(m_dragPreviewLastScenePos);
        }
        if (m_dragPreviewPlayer) m_dragPreviewPlayer->pause();
        if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    });
    m_dragPreviewPlayer->play();
}

void ScreenCanvas::onFastVideoThumbnailReady(const QImage& img) {
    if (img.isNull()) return;
    if (m_dragPreviewGotFrame) return; // already satisfied by fallback
    m_dragPreviewGotFrame = true;
    QPixmap pm = QPixmap::fromImage(img);
    if (pm.isNull()) return;
    m_dragPreviewPixmap = pm;
    m_dragPreviewBaseSize = pm.size();
    if (!m_dragPreviewItem) {
        auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap);
        pmItem->setOpacity(0.0);
        pmItem->setZValue(5000.0);
        pmItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, false);
        pmItem->setScale(m_scaleFactor);
        if (m_scene) m_scene->addItem(pmItem);
        m_dragPreviewItem = pmItem;
        updateDragPreviewPos(m_dragPreviewLastScenePos);
        startDragPreviewFadeIn();
    } else if (auto* pix = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) {
        pix->setPixmap(m_dragPreviewPixmap);
        updateDragPreviewPos(m_dragPreviewLastScenePos);
    }
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    // If decoder path started, stop it
    if (m_dragPreviewPlayer) { m_dragPreviewPlayer->stop(); m_dragPreviewPlayer->deleteLater(); m_dragPreviewPlayer = nullptr; }
    if (m_dragPreviewSink) { m_dragPreviewSink->deleteLater(); m_dragPreviewSink = nullptr; }
    if (m_dragPreviewAudio) { m_dragPreviewAudio->deleteLater(); m_dragPreviewAudio = nullptr; }
}

void ScreenCanvas::stopVideoPreviewProbe() {
    if (m_dragPreviewFallbackTimer) {
        m_dragPreviewFallbackTimer->stop();
        m_dragPreviewFallbackTimer->deleteLater();
        m_dragPreviewFallbackTimer = nullptr;
    }
    if (m_dragPreviewPlayer) {
        m_dragPreviewPlayer->stop();
        m_dragPreviewPlayer->deleteLater();
        m_dragPreviewPlayer = nullptr;
    }
    if (m_dragPreviewSink) {
        m_dragPreviewSink->deleteLater();
        m_dragPreviewSink = nullptr;
    }
    if (m_dragPreviewAudio) {
        m_dragPreviewAudio->deleteLater();
        m_dragPreviewAudio = nullptr;
    }
}

void ScreenCanvas::startDragPreviewFadeIn() {
    // Stop any existing animation first
    stopDragPreviewFade();
    if (!m_dragPreviewItem) return;
    const qreal target = m_dragPreviewTargetOpacity;
    if (m_dragPreviewItem->opacity() >= target - 0.001) return;
    auto* anim = new QVariantAnimation(this);
    m_dragPreviewFadeAnim = anim;
    anim->setStartValue(0.0);
    anim->setEndValue(target);
    anim->setDuration(m_dragPreviewFadeMs);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){
        if (m_dragPreviewItem) m_dragPreviewItem->setOpacity(v.toReal());
    });
    connect(anim, &QVariantAnimation::finished, this, [this](){
        m_dragPreviewFadeAnim = nullptr;
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void ScreenCanvas::stopDragPreviewFade() {
    if (m_dragPreviewFadeAnim) {
        m_dragPreviewFadeAnim->stop();
        m_dragPreviewFadeAnim = nullptr;
    }
}

QGraphicsRectItem* ScreenCanvas::createScreenItem(const ScreenInfo& screen, int index, const QRectF& position) {
    // Border must be fully inside so the outer size matches the logical screen size.
    const int penWidth = m_screenBorderWidthPx; // configurable
    // Inset the rect by half the pen width so the stroke stays entirely inside.
    QRectF inner = position.adjusted(penWidth / 2.0, penWidth / 2.0,
                                     -penWidth / 2.0, -penWidth / 2.0);
    QGraphicsRectItem* item = new QGraphicsRectItem(inner);
    
    // Set appearance
    if (screen.primary) {
        item->setBrush(QBrush(QColor(74, 144, 226, 180))); // Primary screen - blue
        item->setPen(QPen(QColor(74, 144, 226), penWidth));
    } else {
        item->setBrush(QBrush(QColor(80, 80, 80, 180))); // Secondary screen - gray
        item->setPen(QPen(QColor(160, 160, 160), penWidth));
    }
    
    // Store screen index for click handling
    item->setData(0, index);
    
    // Add screen label
    QGraphicsTextItem* label = new QGraphicsTextItem(QString("Screen %1\n%2×%3")
        .arg(index + 1)
        .arg(screen.width)
        .arg(screen.height));
    label->setDefaultTextColor(Qt::white);
    label->setFont(QFont("Arial", 12, QFont::Bold));
    
    // Center the label on the screen
    QRectF labelRect = label->boundingRect();
    QRectF screenRect = item->rect();
    label->setPos(screenRect.center() - labelRect.center());
    label->setParentItem(item);
    
    return item;
}

void ScreenCanvas::setScreenBorderWidthPx(int px) {
    m_screenBorderWidthPx = qMax(0, px);
    // Update existing screen items to keep the same outer size while drawing the stroke fully inside
    // We know m_screenItems aligns with m_screens by index after createScreenItems().
    if (!m_scene) return;
    for (int i = 0; i < m_screenItems.size() && i < m_screens.size(); ++i) {
        QGraphicsRectItem* item = m_screenItems[i];
        if (!item) continue;
        const int penW = m_screenBorderWidthPx;
        // Compute the outer rect from the current item rect by reversing the previous inset.
        // Previous inset may differ; reconstruct outer by expanding by old half-pen, then re-inset by new half-pen.
        // Since we don't track the old pen per-item, approximate using current pen width on the item.
        int oldPenW = static_cast<int>(item->pen().widthF());
        QRectF currentInner = item->rect();
        QRectF outer = currentInner.adjusted(-(oldPenW/2.0), -(oldPenW/2.0), (oldPenW/2.0), (oldPenW/2.0));
        QRectF newInner = outer.adjusted(penW/2.0, penW/2.0, -penW/2.0, -penW/2.0);
        item->setRect(newInner);
        QPen p = item->pen();
        p.setWidthF(penW);
        item->setPen(p);
    }
}

QMap<int, QRectF> ScreenCanvas::calculateCompactPositions(double scaleFactor, double hSpacing, double vSpacing) const {
    QMap<int, QRectF> positions;
    
    if (m_screens.isEmpty()) {
        return positions;
    }
    
    // Simple left-to-right, top-to-bottom compact layout
    
    // First, sort screens by their actual position (left to right, then top to bottom)
    QList<QPair<int, ScreenInfo>> screenPairs;
    for (int i = 0; i < m_screens.size(); ++i) {
        screenPairs.append(qMakePair(i, m_screens[i]));
    }
    
    // Sort by Y first (top to bottom), then by X (left to right)
    std::sort(screenPairs.begin(), screenPairs.end(), 
              [](const QPair<int, ScreenInfo>& a, const QPair<int, ScreenInfo>& b) {
                  if (qAbs(a.second.y - b.second.y) < 100) { // If roughly same height
                      return a.second.x < b.second.x; // Sort by X
                  }
                  return a.second.y < b.second.y; // Sort by Y
              });
    
    // Layout screens with compact positioning
    double currentX = 0;
    double currentY = 0;
    double rowHeight = 0;
    int lastY = INT_MIN;
    
    for (const auto& pair : screenPairs) {
        int index = pair.first;
        const ScreenInfo& screen = pair.second;
        
        double screenWidth = screen.width * scaleFactor;
        double screenHeight = screen.height * scaleFactor;
        
        // Start new row if Y position changed significantly
        if (lastY != INT_MIN && qAbs(screen.y - lastY) > 100) {
            currentX = 0;
            currentY += rowHeight + vSpacing;
            rowHeight = 0;
        }
        
        // Position the screen
        QRectF rect(currentX, currentY, screenWidth, screenHeight);
    positions[index] = rect;
        
        // Update for next screen
    currentX += screenWidth + hSpacing;
        rowHeight = qMax(rowHeight, screenHeight);
        lastY = screen.y;
    }
    
    return positions;
}

QRectF ScreenCanvas::screensBoundingRect() const {
    QRectF bounds;
    bool first = true;
    for (auto* item : m_screenItems) {
        if (!item) continue;
        QRectF r = item->sceneBoundingRect();
        if (first) { bounds = r; first = false; }
        else { bounds = bounds.united(r); }
    }
    return bounds;
}

void ScreenCanvas::recenterWithMargin(int marginPx) {
    QRectF bounds = screensBoundingRect();
    if (bounds.isNull() || !bounds.isValid()) return;
    // Fit content with a fixed pixel margin in the viewport
    const QSize vp = viewport() ? viewport()->size() : size();
    const qreal availW = static_cast<qreal>(vp.width())  - 2.0 * marginPx;
    const qreal availH = static_cast<qreal>(vp.height()) - 2.0 * marginPx;

    if (availW <= 1 || availH <= 1 || bounds.width() <= 0 || bounds.height() <= 0) {
        // Fallback to a simple fit if viewport is too small
        fitInView(bounds, Qt::KeepAspectRatio);
        centerOn(bounds.center());
        return;
    }

    const qreal sx = availW / bounds.width();
    const qreal sy = availH / bounds.height();
    const qreal s = std::min(sx, sy);

    // Apply the scale in one shot and center on content
    QTransform t;
    t.scale(s, s);
    setTransform(t);
    centerOn(bounds.center());
    // After recenter, refresh overlays only for selected items (cheaper and sufficient)
    if (m_scene) {
        const QList<QGraphicsItem*> sel = m_scene->selectedItems();
        for (QGraphicsItem* it : sel) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                v->requestOverlayRelayout();
            }
            if (auto* b = dynamic_cast<ResizableMediaBase*>(it)) {
                b->requestLabelRelayout();
            }
        }
    }
    // Start momentum suppression: ignore decaying inertial scroll deltas until an increase occurs
    m_ignorePanMomentum = true;
    m_momentumPrimed = false;
    m_lastMomentumMag = 0.0;
    m_lastMomentumDelta = QPoint(0, 0);
    m_momentumTimer.restart();
}

void ScreenCanvas::keyPressEvent(QKeyEvent* event) {
    // Delete selected media items
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_scene) {
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) {
                if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) {
                    base->ungrabMouse();
                    m_scene->removeItem(base);
                    delete base;
                }
            }
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Space) {
        recenterWithMargin(53);
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

bool ScreenCanvas::event(QEvent* event) {
    if (event->type() == QEvent::Gesture) {
#ifdef Q_OS_MACOS
        // On macOS, rely on QNativeGestureEvent for trackpad pinch to avoid duplicate scaling
        return false;
#else
        return gestureEvent(static_cast<QGestureEvent*>(event));
#endif
    }
    // Handle native gestures (macOS trackpad pinch)
    if (event->type() == QEvent::NativeGesture) {
        auto* ng = static_cast<QNativeGestureEvent*>(event);
        if (ng->gestureType() == Qt::ZoomNativeGesture) {
            // Mark pinch as active and extend guard
            m_nativePinchActive = true;
            m_nativePinchGuardTimer->start();
            // ng->value() is a delta; use exponential to convert to multiplicative factor
            const qreal factor = std::pow(2.0, ng->value());
            // Figma/Canva-style: anchor at the cursor position first
            QPoint vpPos = viewport()->mapFromGlobal(QCursor::pos());
            if (!viewport()->rect().contains(vpPos)) {
                // Fallback to gesture position (view coords -> viewport) then last mouse pos/center
                QPoint viewPos = ng->position().toPoint();
                vpPos = viewport()->mapFrom(this, viewPos);
                if (!viewport()->rect().contains(vpPos)) {
                    vpPos = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
                }
            }
            zoomAroundViewportPos(vpPos, factor);
            event->accept();
            return true;
        }
    }
    return QGraphicsView::event(event);
}

bool ScreenCanvas::gestureEvent(QGestureEvent* event) {
#ifdef Q_OS_MACOS
    Q_UNUSED(event);
    return false; // handled via NativeGesture on macOS
#endif
    if (QGesture* pinch = event->gesture(Qt::PinchGesture)) {
        auto* pinchGesture = static_cast<QPinchGesture*>(pinch);
        if (pinchGesture->changeFlags() & QPinchGesture::ScaleFactorChanged) {
            const qreal factor = pinchGesture->scaleFactor();
            const QPoint anchor = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
            zoomAroundViewportPos(anchor, factor);
        }
        event->accept();
        return true;
    }
    return false;
}

void ScreenCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // Priority 0: forward to controls of currently selected videos before any other handling
        if (m_scene) {
            const QPointF scenePosEarly = mapToScene(event->pos());
            const QList<QGraphicsItem*> selEarly = m_scene->selectedItems();
            for (QGraphicsItem* it : selEarly) {
                if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                    const QPointF itemPos = v->mapFromScene(scenePosEarly);
                    if (v->handleControlsPressAtItemPos(itemPos)) {
                        m_overlayMouseDown = true; // consume subsequent release as well
                        event->accept();
                        return;
                    }
                }
            }
        }
        // Try to start resize on the topmost media item whose handle contains the point
        // BUT only if the item is already selected
        const QPointF scenePos = mapToScene(event->pos());
        ResizableMediaBase* topHandleItem = nullptr;
        qreal topZ = -std::numeric_limits<qreal>::infinity();
    const QList<QGraphicsItem*> sel = m_scene ? m_scene->selectedItems() : QList<QGraphicsItem*>();
    for (QGraphicsItem* it : sel) {
            if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
                // Only allow resize if item is selected
                if (rp->isSelected() && rp->isOnHandleAtItemPos(rp->mapFromScene(scenePos))) {
                    if (rp->zValue() > topZ) { topZ = rp->zValue(); topHandleItem = rp; }
                }
            }
        }
        if (topHandleItem) {
            if (topHandleItem->beginResizeAtScenePos(scenePos)) {
                // Set resize cursor during active resize
                Qt::CursorShape cursor = topHandleItem->cursorForScenePos(scenePos);
                viewport()->setCursor(cursor);
                event->accept();
                return;
            }
        }
        // Decide based on item type under cursor: media (or its overlays) -> scene, screens/empty -> pan
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        // If any hit item is an overlay panel/element (tagged with data(0) == "overlay"), swallow click without deselecting
        bool clickedOverlayOnly = false;
        if (!hitItems.isEmpty()) {
            bool anyMedia = false;
            for (QGraphicsItem* hi : hitItems) {
                if (hi->data(0).toString() == QLatin1String("overlay")) {
                    // keep flag but continue to see if there is also media underneath
                    clickedOverlayOnly = true;
                }
                // Detect media by dynamic_cast; if found, treat as normal media click (handled below)
                QGraphicsItem* scan = hi;
                while (scan) {
                    if (dynamic_cast<ResizableMediaBase*>(scan)) { anyMedia = true; break; }
                    scan = scan->parentItem();
                }
            }
            if (clickedOverlayOnly && !anyMedia) {
                // Pure overlay (e.g., filename label) under cursor: keep current selection and do nothing
                event->accept();
                return;
            }
        }
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        ResizableMediaBase* mediaHit = nullptr;
        QGraphicsItem* rawHit = nullptr;
        for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) { rawHit = it; break; } }
        if (mediaHit) {
            // For simple UX: modifiers do nothing; always single-select the clicked media
            if (m_scene) m_scene->clearSelection();
            if (!mediaHit->isSelected()) mediaHit->setSelected(true);
            // Map click to item pos and let video handle controls
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) {
                const QPointF itemPos = v->mapFromScene(mapToScene(event->pos()));
                if (v->handleControlsPressAtItemPos(itemPos)) {
                    event->accept();
                    return;
                }
            }
            // Let default behavior run (for move/drag, etc.), but ignore modifiers
            QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(),
                                  event->button(), event->buttons(), Qt::NoModifier);
            QGraphicsView::mousePressEvent(&synthetic);
            // Enforce single selection after default handling
            if (m_scene) m_scene->clearSelection();
            mediaHit->setSelected(true);
            return;
        }

        // No item hit: allow interacting with controls of currently selected video items
        // This covers the case where the floating controls are positioned outside the item's hittable area
        for (QGraphicsItem* it : scene()->selectedItems()) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                const QPointF itemPos = v->mapFromScene(mapToScene(event->pos()));
                if (v->handleControlsPressAtItemPos(itemPos)) {
                    event->accept();
                    return;
                }
            }
        }
        // Otherwise start panning the view
        if (m_scene) {
            m_scene->clearSelection(); // single-click on empty space deselects items
        }
        m_panning = true;
        m_lastPanPoint = event->pos();
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void ScreenCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_scene) {
            const QPointF scenePosSel = mapToScene(event->pos());
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) {
                if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                    const QPointF itemPos = v->mapFromScene(scenePosSel);
                    if (v->handleControlsPressAtItemPos(itemPos)) {
                        // Swallow the remainder of this double-click sequence (final release)
                        m_overlayMouseDown = true;
                        event->accept();
                        return;
                    }
                }
            }
        }
        const QPointF scenePos = mapToScene(event->pos());
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        ResizableMediaBase* mediaHit = nullptr;
        for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (mediaHit) {
            if (scene()) scene()->clearSelection();
            if (!mediaHit->isSelected()) mediaHit->setSelected(true);
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) {
                const QPointF itemPos = v->mapFromScene(scenePos);
                if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; }
            }
            QGraphicsView::mouseDoubleClickEvent(event);
            // Enforce single selection again
            if (scene()) scene()->clearSelection();
            mediaHit->setSelected(true);
            return;
        }
        // If no media hit, pass through
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ScreenCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (m_overlayMouseDown) {
        // If an overlay initiated the interaction, forward moves to any dragging sliders
        if (m_scene) {
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) {
                if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                    if (v->isDraggingProgress() || v->isDraggingVolume()) {
                        v->updateDragWithScenePos(mapToScene(event->pos()));
                        event->accept();
                        return;
                    }
                }
            }
        }
        // Not a drag-type overlay; just swallow move
        event->accept();
        return;
    }
    // Track last mouse pos in viewport coords (used for zoom anchoring)
    m_lastMousePos = event->pos();
    
    // PRIORITY: Check if we're over a resize handle and set cursor accordingly
    // This must be done BEFORE any other cursor management to ensure it's not overridden
    // BUT only if the item is selected!
    const QPointF scenePos = mapToScene(event->pos());
    Qt::CursorShape resizeCursor = Qt::ArrowCursor;
    bool onResizeHandle = false;
    
    // Check only selected media items for handle hover (prioritize topmost)
    qreal topZ = -std::numeric_limits<qreal>::infinity();
    const QList<QGraphicsItem*> sel = m_scene ? m_scene->selectedItems() : QList<QGraphicsItem*>();
    for (QGraphicsItem* it : sel) {
        if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
            // Only show resize cursor if the item is selected
            if (rp->isSelected() && rp->zValue() >= topZ) {
                Qt::CursorShape itemCursor = rp->cursorForScenePos(scenePos);
                if (itemCursor != Qt::ArrowCursor) {
                    resizeCursor = itemCursor;
                    onResizeHandle = true;
                    topZ = rp->zValue();
                }
            }
        }
    }
    
    // Set the cursor with highest priority
    if (onResizeHandle) {
        viewport()->setCursor(resizeCursor);
    } else {
        viewport()->unsetCursor();
    }
    
    // Handle dragging and panning logic
    if (event->buttons() & Qt::LeftButton) {
        // If a selected video is currently dragging its sliders, update it even if cursor left its shape
    for (QGraphicsItem* it : sel) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) {
                    v->updateDragWithScenePos(mapToScene(event->pos()));
                    event->accept();
                    return;
                }
            }
        }
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        bool hitMedia = false;
        for (QGraphicsItem* it : hitItems) { if (toMedia(it)) { hitMedia = true; break; } }
        if (hitMedia) { QGraphicsView::mouseMoveEvent(event); return; }
    }
    if (m_panning) {
        // Pan the view
        QPoint delta = event->pos() - m_lastPanPoint;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        m_lastPanPoint = event->pos();
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ScreenCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // If overlay consumed the press, finish any active slider drag and swallow release
        if (m_overlayMouseDown) {
            if (m_scene) {
                const QList<QGraphicsItem*> sel = m_scene->selectedItems();
                for (QGraphicsItem* it : sel) {
                    if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                        if (v->isDraggingProgress() || v->isDraggingVolume()) {
                            v->endDrag();
                        }
                    }
                }
            }
            m_overlayMouseDown = false;
            event->accept();
            return;
        }
        // Finalize any active drag on selected video controls
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) {
                    v->endDrag();
                    event->accept();
                    return;
                }
            }
        }
        if (m_panning) {
            m_panning = false;
            event->accept();
            return;
        }
        // Check if any item was being resized and reset cursor
        bool wasResizing = false;
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
                if (rp->isActivelyResizing()) {
                    wasResizing = true;
                    break;
                }
            }
        }
        if (wasResizing) {
            // Reset cursor after resize
            viewport()->unsetCursor();
        }
        // Route to base with modifiers stripped to avoid Qt toggling selection on Ctrl/Cmd
        QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(),
                              event->button(), event->buttons(), Qt::NoModifier);
        QGraphicsView::mouseReleaseEvent(&synthetic);
        // Enforce single-select policy regardless of modifiers once the click finishes
        if (m_scene) {
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            if (!sel.isEmpty()) {
                // Keep the topmost media item under cursor if possible; otherwise keep the first selected
                const QList<QGraphicsItem*> hitItems = items(event->pos());
                auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
                    while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
                    return nullptr;
                };
                ResizableMediaBase* keep = nullptr;
                for (QGraphicsItem* it : hitItems) { if ((keep = toMedia(it))) break; }
                if (!keep) {
                    for (QGraphicsItem* it : sel) { if ((keep = dynamic_cast<ResizableMediaBase*>(it))) break; }
                }
                m_scene->clearSelection();
                if (keep) keep->setSelected(true);
            }
        }
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ScreenCanvas::wheelEvent(QWheelEvent* event) {
    // On macOS during an active pinch, ignore wheel-based two-finger scroll to avoid jitter
#ifdef Q_OS_MACOS
    if (m_nativePinchActive) {
        event->ignore();
        return;
    }
#endif
    // If Cmd (macOS) or Ctrl (others) is held, treat the wheel as zoom, anchored under cursor
#ifdef Q_OS_MACOS
    const bool zoomModifier = event->modifiers().testFlag(Qt::MetaModifier);
#else
    const bool zoomModifier = event->modifiers().testFlag(Qt::ControlModifier);
#endif

    if (zoomModifier) {
        qreal deltaY = 0.0;
        if (!event->pixelDelta().isNull()) {
            deltaY = event->pixelDelta().y();
        } else if (!event->angleDelta().isNull()) {
            // angleDelta is in 1/8 degree; 120 typically represents one step
            deltaY = event->angleDelta().y() / 8.0; // convert to degrees-ish
        }
        if (deltaY != 0.0) {
            // Convert delta to an exponential zoom factor for smoothness
            const qreal factor = std::pow(1.0015, deltaY);
            zoomAroundViewportPos(event->position(), factor);
            event->accept();
            return;
        }
    }

    // Default behavior: scroll/pan content
    // Prefer pixelDelta for trackpads (gives smooth 2D vector), fallback to angleDelta for mouse wheels.
    QPoint delta;
    if (!event->pixelDelta().isNull()) {
        delta = event->pixelDelta();
    } else if (!event->angleDelta().isNull()) {
        delta = event->angleDelta() / 8; // small scaling for smoother feel
    }
    if (!delta.isNull()) {
        // Momentum suppression: after recenter, ignore decaying inertial deltas robustly
        if (m_ignorePanMomentum) {
            // If the platform provides scroll phase, end suppression on a new phase begin
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
            if (event->phase() == Qt::ScrollUpdate && !m_momentumPrimed) {
                // still within the same fling; keep suppression
            }
            if (event->phase() == Qt::ScrollBegin) {
                // New gesture explicitly began
                m_ignorePanMomentum = false;
            }
            if (!m_ignorePanMomentum) {
                // process normally
            } else
#endif
            {
                // Hysteresis thresholds
                const double noiseGate = 1.0;   // ignore tiny bumps
                const double boostRatio = 1.25; // require 25% increase to consider momentum ended
                const qint64 graceMs = 180;     // ignore everything for a short time after recenter
                const qint64 elapsed = m_momentumTimer.isValid() ? m_momentumTimer.elapsed() : LLONG_MAX;

                // Use Manhattan length for magnitude; direction-agnostic
                const double mag = std::abs(delta.x()) + std::abs(delta.y());
                if (elapsed < graceMs) {
                    // Short time gate immediately after centering
                    event->accept();
                    return;
                }
                if (mag <= noiseGate) {
                    // Ignore tiny jitter
                    event->accept();
                    return;
                }
                if (!m_momentumPrimed) {
                    // First observed delta after centering becomes the baseline and direction
                    m_momentumPrimed = true;
                    m_lastMomentumMag = mag;
                    m_lastMomentumDelta = delta;
                    event->accept();
                    return; // cut immediately
                } else {
                    // If direction stays the same and magnitude does not significantly increase, keep ignoring
                    const bool sameDir = (m_lastMomentumDelta.x() == 0 || (delta.x() == 0) || (std::signbit(m_lastMomentumDelta.x()) == std::signbit(delta.x()))) &&
                                         (m_lastMomentumDelta.y() == 0 || (delta.y() == 0) || (std::signbit(m_lastMomentumDelta.y()) == std::signbit(delta.y())));
                    if (sameDir) {
                        if (mag <= m_lastMomentumMag * boostRatio) {
                            // still decaying or not enough increase: continue suppressing
                            m_lastMomentumMag = mag; // track new mag so we continue to follow the decay
                            m_lastMomentumDelta = delta;
                            event->accept();
                            return;
                        }
                        // Significant boost in same direction -> consider it a new intentional scroll; end suppression
                        m_ignorePanMomentum = false;
                    } else {
                        // Direction changed (user reversed) -> end suppression immediately to honor the user's input
                        m_ignorePanMomentum = false;
                    }
                }
            }
        }
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void ScreenCanvas::zoomAroundViewportPos(const QPointF& vpPosF, qreal factor) {
    QPoint vpPos = vpPosF.toPoint();
    if (!viewport()->rect().contains(vpPos)) {
        vpPos = viewport()->rect().center();
    }
    const QPointF sceneAnchor = mapToScene(vpPos);
    // Compose a new transform that scales around the scene anchor directly
    QTransform t = transform();
    t.translate(sceneAnchor.x(), sceneAnchor.y());
    t.scale(factor, factor);
    t.translate(-sceneAnchor.x(), -sceneAnchor.y());
    setTransform(t);
    // After zoom, refresh overlays only for selected items
    if (m_scene) {
        const QList<QGraphicsItem*> sel = m_scene->selectedItems();
        for (QGraphicsItem* it : sel) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                v->requestOverlayRelayout();
            }
            if (auto* b = dynamic_cast<ResizableMediaBase*>(it)) {
                b->requestLabelRelayout();
            }
        }
    }
}

MainWindow::~MainWindow() {
    if (m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
    }
}

void MainWindow::setupUI() {
    m_centralWidget = new QWidget(this);
    setCentralWidget(m_centralWidget);
    
    m_mainLayout = new QVBoxLayout(m_centralWidget);
    m_mainLayout->setSpacing(0); // Remove spacing, we'll handle it manually
    m_mainLayout->setContentsMargins(0, 0, 0, 0); // Remove margins from main layout
    
    // Top section with margins
    QWidget* topSection = new QWidget();
    QVBoxLayout* topLayout = new QVBoxLayout(topSection);
    topLayout->setContentsMargins(20, 20, 20, 20); // Apply margins only to top section
    topLayout->setSpacing(20);
    
    // Connection section (always visible)
    m_connectionLayout = new QHBoxLayout();
    
    // Back button (left-aligned, initially hidden)
    m_backButton = new QPushButton("← Back to Client List");
    m_backButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    m_backButton->setAutoDefault(false);
    m_backButton->setDefault(false);
    m_backButton->setFocusPolicy(Qt::NoFocus);
    m_backButton->hide(); // Initially hidden, shown only on screen view
    connect(m_backButton, &QPushButton::clicked, this, &MainWindow::onBackToClientListClicked);
    
    // Status label (no "Status:")
    m_connectionStatusLabel = new QLabel("DISCONNECTED");
    m_connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");

    // Enable/Disable toggle button with fixed width (left of Settings)
    m_connectToggleButton = new QPushButton("Disable");
    m_connectToggleButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    m_connectToggleButton->setFixedWidth(111);
    connect(m_connectToggleButton, &QPushButton::clicked, this, &MainWindow::onEnableDisableClicked);

    // Settings button
    m_settingsButton = new QPushButton("Settings");
    m_settingsButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::showSettingsDialog);

    // Layout: [back][stretch][status][connect][settings]
    m_connectionLayout->addWidget(m_backButton);
    m_connectionLayout->addStretch();
    m_connectionLayout->addWidget(m_connectionStatusLabel);
    m_connectionLayout->addWidget(m_connectToggleButton);
    m_connectionLayout->addWidget(m_settingsButton);
    
    topLayout->addLayout(m_connectionLayout);
    m_mainLayout->addWidget(topSection);
    
    // Bottom section with margins (no separator line)
    QWidget* bottomSection = new QWidget();
    QVBoxLayout* bottomLayout = new QVBoxLayout(bottomSection);
    bottomLayout->setContentsMargins(20, 20, 20, 20); // Apply margins only to bottom section
    bottomLayout->setSpacing(20);
    
    // Create stacked widget for page navigation
    m_stackedWidget = new QStackedWidget();
    // Block stray key events (like space) at the stack level
    m_stackedWidget->installEventFilter(this);
    bottomLayout->addWidget(m_stackedWidget);
    m_mainLayout->addWidget(bottomSection);
    
    // Create client list page
    createClientListPage();
    
    // Create screen view page  
    createScreenViewPage();
    
    // Start with client list page
    m_stackedWidget->setCurrentWidget(m_clientListPage);

    // Initialize navigation manager (after widgets exist)
    m_navigationManager = new ScreenNavigationManager(this);
    {
        ScreenNavigationManager::Widgets w;
        w.stack = m_stackedWidget;
        w.clientListPage = m_clientListPage;
        w.screenViewPage = m_screenViewWidget;
        w.backButton = m_backButton;
        w.canvasStack = m_canvasStack;
        w.loadingSpinner = m_loadingSpinner;
        w.spinnerOpacity = m_spinnerOpacity;
        w.spinnerFade = m_spinnerFade;
        w.canvasOpacity = m_canvasOpacity;
        w.canvasFade = m_canvasFade;
        w.volumeOpacity = m_volumeOpacity;
        w.volumeFade = m_volumeFade;
        w.screenCanvas = m_screenCanvas;
        m_navigationManager->setWidgets(w);
        m_navigationManager->setDurations(m_loaderDelayMs, m_loaderFadeDurationMs, m_fadeDurationMs);
        connect(m_navigationManager, &ScreenNavigationManager::requestScreens, this, [this](const QString& id){
            if (m_webSocketClient && m_webSocketClient->isConnected()) m_webSocketClient->requestScreens(id);
        });
        connect(m_navigationManager, &ScreenNavigationManager::watchTargetRequested, this, [this](const QString& id){
            if (m_watchManager && m_webSocketClient && m_webSocketClient->isConnected()) m_watchManager->toggleWatch(id);
        });
        connect(m_navigationManager, &ScreenNavigationManager::clientListEntered, this, [this](){
            if (m_watchManager) m_watchManager->unwatchIfAny();
            if (m_screenCanvas) m_screenCanvas->hideRemoteCursor();
        });
    }

    // Receive remote cursor updates when watching
    connect(m_webSocketClient, &WebSocketClient::cursorPositionReceived, this,
            [this](const QString& targetId, int x, int y) {
                if (m_stackedWidget->currentWidget() == m_screenViewWidget && m_watchManager && targetId == m_watchManager->watchedClientId() && m_screenCanvas) {
                    m_screenCanvas->updateRemoteCursor(x, y);
                }
            });
}

void MainWindow::createClientListPage() {
    m_clientListPage = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_clientListPage);
    layout->setSpacing(15);
    layout->setContentsMargins(0, 0, 0, 0);
    
    // Client list section
    m_clientListLabel = new QLabel("Connected Clients:");
    m_clientListLabel->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    layout->addWidget(m_clientListLabel);
    
    m_clientListWidget = new QListWidget();
    // Use palette-based colors so light/dark themes adapt automatically
    // Add subtle hover effect and remove persistent selection highlight
    m_clientListWidget->setStyleSheet(
        "QListWidget { "
        "   border: 1px solid palette(mid); "
        "   border-radius: 5px; "
        "   padding: 5px; "
        "   background-color: palette(base); "
        "   color: palette(text); "
        "}"
        "QListWidget::item { "
        "   padding: 10px; "
        "   border-bottom: 1px solid palette(midlight); "
        "}" 
        // Hover: very light blue tint
        "QListWidget::item:hover { "
        "   background-color: rgba(74, 144, 226, 28); "
        "}"
        // Suppress active/selected highlight colors
        "QListWidget::item:selected { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:active { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:hover { "
        "   background-color: rgba(74, 144, 226, 28); "
        "   color: palette(text); "
        "}"
    );
    connect(m_clientListWidget, &QListWidget::itemClicked, this, &MainWindow::onClientItemClicked);
    // Prevent keyboard (space/enter) from triggering navigation
    m_clientListWidget->setFocusPolicy(Qt::NoFocus);
    m_clientListWidget->installEventFilter(this);
    // Enable hover state over items (for :hover style)
    m_clientListWidget->setMouseTracking(true);
    layout->addWidget(m_clientListWidget);
    
    m_noClientsLabel = new QLabel("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
    m_noClientsLabel->setStyleSheet("QLabel { color: #666; font-style: italic; text-align: center; }");
    m_noClientsLabel->setAlignment(Qt::AlignCenter);
    m_noClientsLabel->setWordWrap(true);
    layout->addWidget(m_noClientsLabel);
    
    // Selected client info
    m_selectedClientLabel = new QLabel();
    m_selectedClientLabel->setStyleSheet("QLabel { background-color: #e8f4fd; padding: 10px; border-radius: 5px; }");
    m_selectedClientLabel->setWordWrap(true);
    m_selectedClientLabel->hide();
    layout->addWidget(m_selectedClientLabel);
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_clientListPage);
    
    // Initially hide the separate "no clients" label since we'll show it in the list widget itself
    m_noClientsLabel->hide();
}

void MainWindow::createScreenViewPage() {
    // Screen view page
    m_screenViewWidget = new QWidget();
    m_screenViewLayout = new QVBoxLayout(m_screenViewWidget);
    m_screenViewLayout->setSpacing(15);
    m_screenViewLayout->setContentsMargins(0, 0, 0, 0);
    
    // Header row: hostname on the left, indicators on the right (replaces "Connected Clients:" title)
    QHBoxLayout* headerLayout = new QHBoxLayout();

    m_clientNameLabel = new QLabel();
    m_clientNameLabel->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: palette(text); }");
    m_clientNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_volumeIndicator = new QLabel("🔈 --");
    m_volumeIndicator->setStyleSheet("QLabel { font-size: 16px; color: palette(text); font-weight: bold; }");
    m_volumeIndicator->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volumeIndicator->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    headerLayout->addWidget(m_clientNameLabel, 0, Qt::AlignLeft);
    headerLayout->addStretch();
    headerLayout->addWidget(m_volumeIndicator, 0, Qt::AlignRight);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    m_screenViewLayout->addLayout(headerLayout);
    
    // Canvas container holds spinner and canvas with a stacked layout
    m_canvasContainer = new QWidget();
    m_canvasContainer->setObjectName("CanvasContainer");
    m_canvasContainer->setMinimumHeight(400);
    // Ensure stylesheet background/border is actually painted
    m_canvasContainer->setAttribute(Qt::WA_StyledBackground, true);
    // Match the dark background used by the client list container via palette(base)
    m_canvasContainer->setStyleSheet(
        "QWidget#CanvasContainer { "
        "   background-color: palette(base); "
        "   border: 1px solid palette(mid); "
        "   border-radius: 5px; "
        "}"
    );
    QVBoxLayout* containerLayout = new QVBoxLayout(m_canvasContainer);
    // Provide real inner padding so child doesn't cover the border area
    containerLayout->setContentsMargins(5,5,5,5);
    containerLayout->setSpacing(0);
    m_canvasStack = new QStackedWidget();
    // Match client list container: base background, no border on inner stack
    m_canvasStack->setStyleSheet(
        "QStackedWidget { "
        "   background-color: palette(base); "
        "   border: none; "
        "}"
    );
    containerLayout->addWidget(m_canvasStack);
    // Clip stack to rounded corners
    m_canvasStack->installEventFilter(this);

    // Spinner page
    m_loadingSpinner = new SpinnerWidget();
    // Initial appearance (easy to tweak):
    m_loadingSpinner->setRadius(22);        // circle radius in px
    m_loadingSpinner->setLineWidth(6);      // line width in px
    m_loadingSpinner->setColor(QColor("#4a90e2")); // brand blue
    m_loadingSpinner->setMinimumSize(QSize(48, 48));
    // Spinner page widget wraps the spinner centered
    QWidget* spinnerPage = new QWidget();
    QVBoxLayout* spinnerLayout = new QVBoxLayout(spinnerPage);
    spinnerLayout->setContentsMargins(0,0,0,0);
    spinnerLayout->setSpacing(0);
    spinnerLayout->addStretch();
    spinnerLayout->addWidget(m_loadingSpinner, 0, Qt::AlignCenter);
    spinnerLayout->addStretch();
    // Spinner page opacity effect & animation (fade entire loader area)
    m_spinnerOpacity = new QGraphicsOpacityEffect(spinnerPage);
    spinnerPage->setGraphicsEffect(m_spinnerOpacity);
    m_spinnerOpacity->setOpacity(0.0);
    m_spinnerFade = new QPropertyAnimation(m_spinnerOpacity, "opacity", this);
    m_spinnerFade->setDuration(m_loaderFadeDurationMs);
    m_spinnerFade->setStartValue(0.0);
    m_spinnerFade->setEndValue(1.0);
    // spinnerPage already created above

    // Canvas page
    QWidget* canvasPage = new QWidget();
    QVBoxLayout* canvasLayout = new QVBoxLayout(canvasPage);
    canvasLayout->setContentsMargins(0,0,0,0);
    canvasLayout->setSpacing(0);
    m_screenCanvas = new ScreenCanvas();
    m_screenCanvas->setMinimumHeight(400);
    // Ensure the viewport background matches and is rounded
    if (m_screenCanvas->viewport()) {
        m_screenCanvas->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        m_screenCanvas->viewport()->setAutoFillBackground(true);
        // No border on the viewport to avoid inner border effects
        m_screenCanvas->viewport()->setStyleSheet("background-color: transparent; border: none;");
    }
    m_screenCanvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Full updates avoid ghosting when moving scene-level overlays with ItemIgnoresTransformations
    m_screenCanvas->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    // Screens are not clickable; canvas supports panning and media placement
    canvasLayout->addWidget(m_screenCanvas);
    // Canvas/content opacity effect & animation (apply to the page, not the QGraphicsView viewport to avoid heavy repaints)
    m_canvasOpacity = new QGraphicsOpacityEffect(canvasPage);
    canvasPage->setGraphicsEffect(m_canvasOpacity);
    m_canvasOpacity->setOpacity(0.0);
    m_canvasFade = new QPropertyAnimation(m_canvasOpacity, "opacity", this);
    m_canvasFade->setDuration(m_fadeDurationMs);
    m_canvasFade->setStartValue(0.0);
    m_canvasFade->setEndValue(1.0);

    // Add pages and container to main layout
    m_canvasStack->addWidget(spinnerPage); // index 0: spinner
    m_canvasStack->addWidget(canvasPage);  // index 1: canvas
    m_canvasStack->setCurrentIndex(1);     // default to canvas page hidden (opacity 0) until data
    m_screenViewLayout->addWidget(m_canvasContainer, 1);
    // Clip container and viewport to rounded corners
    m_canvasContainer->installEventFilter(this);
    if (m_screenCanvas && m_screenCanvas->viewport()) m_screenCanvas->viewport()->installEventFilter(this);

    // Ensure focus is on canvas, and block stray key events
    m_screenViewWidget->installEventFilter(this);
    m_screenCanvas->setFocusPolicy(Qt::StrongFocus);
    m_screenCanvas->installEventFilter(this);
    
    // Bottom action bar with Upload and Send
    QWidget* actionBar = new QWidget();
    auto* actionLayout = new QHBoxLayout(actionBar);
    actionLayout->setContentsMargins(0, 8, 0, 0);
    actionLayout->setSpacing(12);
    // Upload button
    m_uploadButton = new QPushButton("Upload to Client");
    m_uploadButton->setStyleSheet("QPushButton { padding: 12px 18px; font-weight: bold; background-color: #666; color: white; border-radius: 5px; } QPushButton:checked { background-color: #444; }");
    m_uploadButtonDefaultFont = m_uploadButton->font();
    m_uploadButton->setFixedWidth(260);
    m_uploadButton->setEnabled(true);
    connect(m_uploadButton, &QPushButton::clicked, this, &MainWindow::onUploadButtonClicked);
    actionLayout->addWidget(m_uploadButton, 0, Qt::AlignRight);
    // Send button
    m_sendButton = new QPushButton("Send Media to All Screens");
    m_sendButton->setStyleSheet("QPushButton { padding: 12px 24px; font-weight: bold; background-color: #4a90e2; color: white; border-radius: 5px; }");
    m_sendButton->setEnabled(false); // Initially disabled until media is placed
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::onSendMediaClicked);
    actionLayout->addWidget(m_sendButton, 0, Qt::AlignLeft);
    m_screenViewLayout->addWidget(actionBar, 0, Qt::AlignHCenter);
    // Ensure header has no stretch, container expands, button fixed
    m_screenViewLayout->setStretch(0, 0); // header
    m_screenViewLayout->setStretch(1, 1); // container expands
    m_screenViewLayout->setStretch(2, 0); // button fixed

    // Volume label opacity effect & animation
    m_volumeOpacity = new QGraphicsOpacityEffect(m_volumeIndicator);
    m_volumeIndicator->setGraphicsEffect(m_volumeOpacity);
    m_volumeOpacity->setOpacity(0.0);
    m_volumeFade = new QPropertyAnimation(m_volumeOpacity, "opacity", this);
    m_volumeFade->setDuration(m_fadeDurationMs);
    m_volumeFade->setStartValue(0.0);
    m_volumeFade->setEndValue(1.0);

    // Loader delay timer removed (handled inside ScreenNavigationManager)
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_screenViewWidget);
}

void MainWindow::setupMenuBar() {
    // File menu
    m_fileMenu = menuBar()->addMenu("File");
    
    m_exitAction = new QAction("Quit Mouffette", this);
    m_exitAction->setShortcut(QKeySequence::Quit);
    connect(m_exitAction, &QAction::triggered, this, [this]() {
        if (m_webSocketClient->isConnected()) {
            m_webSocketClient->disconnect();
        }
        QApplication::quit();
    });
    m_fileMenu->addAction(m_exitAction);
    
    // Help menu
    m_helpMenu = menuBar()->addMenu("Help");
    
    m_aboutAction = new QAction("About", this);
    connect(m_aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "About Mouffette", 
            "Mouffette v1.0.0\n\n"
            "A cross-platform media sharing application that allows users to "
            "share and display media on other connected users' screens.\n\n"
            "Built with Qt and WebSocket technology.");
    });
    m_helpMenu->addAction(m_aboutAction);
}

void MainWindow::setupSystemTray() {
    // Create tray icon (no context menu, just click handling)
    m_trayIcon = new QSystemTrayIcon(this);
    
    // Set icon - try to load from resources, fallback to simple icon
    QIcon trayIconIcon(":/icons/mouffette.png");
    if (trayIconIcon.isNull()) {
        // Fallback to simple colored icon
        QPixmap pixmap(16, 16);
        pixmap.fill(Qt::blue);
        trayIconIcon = QIcon(pixmap);
    }
    m_trayIcon->setIcon(trayIconIcon);
    
    // Set tooltip
    m_trayIcon->setToolTip("Mouffette - Media Sharing");
    
    // Connect tray icon activation for non-context menu clicks
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::onTrayIconActivated);
    
    // Show the tray icon
    m_trayIcon->show();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_trayIcon && m_trayIcon->isVisible()) {
        // Hide to tray instead of closing
        hide();
        event->ignore();
        
        // Show message first time
        static bool firstHide = true;
        if (firstHide) {
            showTrayMessage("Mouffette", "Application is now running in the background. Click the tray icon to show the window again.");
            firstHide = false;
        }
    } else {
        event->accept();
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    
    // If we're currently showing the screen view and have a canvas with content,
    // recenter the view to maintain good visibility after window resize
    if (m_stackedWidget && m_stackedWidget->currentWidget() == m_screenViewWidget && 
        m_screenCanvas && !m_selectedClient.getScreens().isEmpty()) {
        m_screenCanvas->recenterWithMargin(33);
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
                } else {
                    show();
                }
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

void MainWindow::showTrayMessage(const QString& title, const QString& message) {
    if (m_trayIcon) {
        m_trayIcon->showMessage(title, message, QSystemTrayIcon::Information, 3000);
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

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton* cancelBtn = new QPushButton("Cancel");
    QPushButton* saveBtn = new QPushButton("Save");
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    v->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, [this, urlEdit, &dialog]() {
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
        dialog.accept();
    });

    dialog.exec();
}

void MainWindow::connectToServer() {
    const QString url = m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig;
    m_webSocketClient->connectToServer(url);
}

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

void MainWindow::onConnected() {
    setUIEnabled(true);
    // Reset reconnection state on successful connection
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
    
    // Sync this client's info with the server
    syncRegistration();
    
    statusBar()->showMessage("Connected to server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Connected", "Successfully connected to Mouffette server");
}

void MainWindow::onDisconnected() {
    setUIEnabled(false);
    
    // Start smart reconnection if client is enabled and not manually disconnected
    if (!m_userDisconnected) {
        scheduleReconnect();
    }
    
    // Stop watching if any
    if (m_watchManager) m_watchManager->unwatchIfAny();
    
    // Clear client list
    m_availableClients.clear();
    updateClientList(m_availableClients);
    
    statusBar()->showMessage("Disconnected from server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Disconnected", "Disconnected from Mouffette server");
}

// startWatchingSelectedClient/stopWatchingCurrentClient removed (handled by WatchManager)

void MainWindow::onConnectionError(const QString& error) {
    QMessageBox::warning(this, "Connection Error", 
        QString("Failed to connect to server:\n%1").arg(error));
    
    setUIEnabled(false);
    // No direct connect/disconnect buttons anymore
}

void MainWindow::onClientListReceived(const QList<ClientInfo>& clients) {
    qDebug() << "Received client list with" << clients.size() << "clients";
    
    // Check for new clients
    int previousCount = m_availableClients.size();
    m_availableClients = clients;
    updateClientList(clients);
    
    // Show notification if new clients appeared
    if (clients.size() > previousCount && previousCount >= 0) {
        int newClients = clients.size() - previousCount;
        if (newClients > 0) {
            QString message = QString("%1 new client%2 available for sharing")
                .arg(newClients)
                .arg(newClients == 1 ? "" : "s");
            showTrayMessage("New Clients Available", message);
        }
    }
}

void MainWindow::onRegistrationConfirmed(const ClientInfo& clientInfo) {
    m_thisClient = clientInfo;
    qDebug() << "Registration confirmed for:" << clientInfo.getMachineName();
    
    // Request initial client list
    m_webSocketClient->requestClientList();
}

void MainWindow::onClientSelectionChanged() {
    if (m_ignoreSelectionChange) {
        return;
    }
    
    QListWidgetItem* currentItem = m_clientListWidget->currentItem();
    
    if (currentItem) {
        int index = m_clientListWidget->row(currentItem);
        if (index >= 0 && index < m_availableClients.size()) {
            const ClientInfo& client = m_availableClients[index];
            m_selectedClient = client;
            
            // Show the screen view for the selected client
            showScreenView(client);
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                m_webSocketClient->requestScreens(client.getId());
            }
        }
    } else {
        m_selectedClientLabel->hide();
    }
}

// (Duplicate removed) onScreensInfoReceived is implemented later in the file

void MainWindow::syncRegistration() {
    QString machineName = getMachineName();
    QString platform = getPlatformName();
    QList<ScreenInfo> screens;
    int volumePercent = -1;
    // Only include screens/volume when actively watched; otherwise identity-only
    if (m_isWatched) {
        screens = getLocalScreenInfo();
        volumePercent = getSystemVolumePercent();
    }
    
    qDebug() << "Sync registration:" << machineName << "on" << platform << "with" << screens.size() << "screens";
    
    m_webSocketClient->registerClient(machineName, platform, screens, volumePercent);
}

void MainWindow::onScreensInfoReceived(const ClientInfo& clientInfo) {
    // Update the canvas only if it matches the currently selected client
    if (!clientInfo.getId().isEmpty() && clientInfo.getId() == m_selectedClient.getId()) {
        qDebug() << "Updating canvas with fresh screens for" << clientInfo.getMachineName();
        m_selectedClient = clientInfo; // keep selected client in sync
        // Update screen canvas content
        if (m_screenCanvas) {
            m_screenCanvas->setScreens(clientInfo.getScreens());
            m_screenCanvas->recenterWithMargin(33);
            m_screenCanvas->setFocus(Qt::OtherFocusReason);
        }

        // Delegate reveal (spinner stop + canvas fade) to navigation manager
        if (m_navigationManager) {
            m_navigationManager->revealCanvas();
        } else {
            // Fallback if navigation manager not present (should not happen now)
            if (m_canvasStack) m_canvasStack->setCurrentIndex(1);
        }

        // Update volume UI
        if (m_volumeIndicator) {
            updateVolumeIndicator();
            m_volumeIndicator->show();
        }

        // Refresh client label
        if (m_clientNameLabel)
            m_clientNameLabel->setText(QString("%1 (%2)").arg(clientInfo.getMachineName()).arg(clientInfo.getPlatform()));
    }
}

void MainWindow::onWatchStatusChanged(bool watched) {
    // Store watched state locally (as this client being watched by someone else)
    // We don't need a member; we can gate sending by this flag at runtime.
    // For simplicity, keep a static so our timers can read it.
    m_isWatched = watched;
    qDebug() << "Watch status changed:" << (watched ? "watched" : "not watched");

    // Begin/stop sending our cursor position to watchers (target side)
    if (watched) {
        if (!m_cursorTimer) {
            m_cursorTimer = new QTimer(this);
            m_cursorTimer->setInterval(m_cursorUpdateIntervalMs); // configurable
            connect(m_cursorTimer, &QTimer::timeout, this, [this]() {
                static int lastX = INT_MIN, lastY = INT_MIN;
                const QPoint p = QCursor::pos();
                if (p.x() != lastX || p.y() != lastY) {
                    lastX = p.x();
                    lastY = p.y();
                    if (m_webSocketClient && m_webSocketClient->isConnected() && m_isWatched) {
                        m_webSocketClient->sendCursorUpdate(p.x(), p.y());
                    }
                }
            });
        }
        // Apply any updated interval before starting
        m_cursorTimer->setInterval(m_cursorUpdateIntervalMs);
        if (!m_cursorTimer->isActive()) m_cursorTimer->start();
    } else {
        if (m_cursorTimer) m_cursorTimer->stop();
    }
}

QList<ScreenInfo> MainWindow::getLocalScreenInfo() {
    QList<ScreenInfo> screens;
    QList<QScreen*> screenList = QGuiApplication::screens();
    
    for (int i = 0; i < screenList.size(); ++i) {
        QScreen* screen = screenList[i];
        QRect geometry = screen->geometry();
        bool isPrimary = (screen == QGuiApplication::primaryScreen());
        
        ScreenInfo screenInfo(i, geometry.width(), geometry.height(), geometry.x(), geometry.y(), isPrimary);
        screens.append(screenInfo);
    }
    
    return screens;
}

QString MainWindow::getMachineName() {
    QString hostName = QHostInfo::localHostName();
    if (hostName.isEmpty()) {
        hostName = "Unknown Machine";
    }
    return hostName;
}

QString MainWindow::getPlatformName() {
#ifdef Q_OS_MACOS
    return "macOS";
#elif defined(Q_OS_WIN)
    return "Windows";
#elif defined(Q_OS_LINUX)
    return "Linux";
#else
    return "Unknown";
#endif
}

int MainWindow::getSystemVolumePercent() {
#ifdef Q_OS_MACOS
    // Return cached value; updated asynchronously in setupVolumeMonitoring()
    return m_cachedSystemVolume;
#elif defined(Q_OS_WIN)
    // Use Windows Core Audio APIs (MMDevice + IAudioEndpointVolume)
    // Headers are included only on Windows builds.
    HRESULT hr;
    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioEndpointVolume* pEndpointVol = nullptr;
    bool coInit = SUCCEEDED(CoInitialize(nullptr));
    int result = -1;
    do {
        hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnum);
        if (FAILED(hr) || !pEnum) break;
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        if (FAILED(hr) || !pDevice) break;
        hr = pDevice->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, (void**)&pEndpointVol);
        if (FAILED(hr) || !pEndpointVol) break;
        float volScalar = 0.0f;
        hr = pEndpointVol->GetMasterVolumeLevelScalar(&volScalar);
        if (FAILED(hr)) break;
        int vol = static_cast<int>(std::round(volScalar * 100.0f));
        vol = std::clamp(vol, 0, 100);
        result = vol;
    } while (false);
    if (pEndpointVol) pEndpointVol->Release();
    if (pDevice) pDevice->Release();
    if (pEnum) pEnum->Release();
    if (coInit) CoUninitialize();
    return result;
#elif defined(Q_OS_LINUX)
    return -1; // TODO: Implement via PulseAudio/PipeWire if needed
#else
    return -1;
#endif
}

void MainWindow::setupVolumeMonitoring() {
#ifdef Q_OS_MACOS
    // Asynchronous polling to avoid blocking the UI thread.
    if (!m_volProc) {
        m_volProc = new QProcess(this);
        // No visible window; ensure fast exit
        connect(m_volProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
            const QByteArray out = m_volProc->readAllStandardOutput().trimmed();
            bool ok = false;
            int vol = QString::fromUtf8(out).toInt(&ok);
            if (ok) {
                vol = std::clamp(vol, 0, 100);
                if (vol != m_cachedSystemVolume) {
                    m_cachedSystemVolume = vol;
                    if (m_webSocketClient->isConnected() && m_isWatched) {
                        syncRegistration();
                    }
                }
            }
        });
    }
    if (!m_volTimer) {
        m_volTimer = new QTimer(this);
        m_volTimer->setInterval(1200); // ~1.2s cadence
        connect(m_volTimer, &QTimer::timeout, this, [this]() {
            if (m_volProc->state() == QProcess::NotRunning) {
                m_volProc->start("/usr/bin/osascript", {"-e", "output volume of (get volume settings)"});
            }
        });
        m_volTimer->start();
    }
#else
    // Non-macOS: simple polling; Windows call is fast.
    QTimer* volTimer = new QTimer(this);
    volTimer->setInterval(1200);
    connect(volTimer, &QTimer::timeout, this, [this]() {
        int v = getSystemVolumePercent();
        if (v != m_cachedSystemVolume) {
            m_cachedSystemVolume = v;
            if (m_webSocketClient->isConnected() && m_isWatched) {
                syncRegistration();
            }
        }
    });
    volTimer->start();
#endif
}

void MainWindow::updateClientList(const QList<ClientInfo>& clients) {
    m_clientListWidget->clear();
    
    if (clients.isEmpty()) {
        // Show the "no clients" message centered in the list widget with larger font
        QListWidgetItem* item = new QListWidgetItem("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
        item->setFlags(Qt::NoItemFlags); // Make it non-selectable and non-interactive
        item->setTextAlignment(Qt::AlignCenter);
        QFont font = item->font();
        font.setItalic(true);
        font.setPointSize(16); // Make the font larger
        item->setFont(font);
        item->setForeground(QColor(102, 102, 102)); // #666 color
        
    // Set a custom size hint to center the item vertically in the list widget.
    // Use the viewport height (content area) to avoid off-by-margins that cause scrollbars.
    const int viewportH = m_clientListWidget->viewport() ? m_clientListWidget->viewport()->height() : m_clientListWidget->height();
    item->setSizeHint(QSize(m_clientListWidget->width(), qMax(0, viewportH)));
        
        m_clientListWidget->addItem(item);
    // Ensure no scrollbars are shown for the single placeholder item
    m_clientListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_clientListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_noClientsLabel->hide(); // Hide the separate label since we show message in list
    } else {
        m_noClientsLabel->hide();
    // Restore scrollbar policies when there are items to potentially scroll
    m_clientListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_clientListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        
        for (const ClientInfo& client : clients) {
            QString displayText = client.getDisplayText();
            QListWidgetItem* item = new QListWidgetItem(displayText);
            item->setToolTip(QString("ID: %1\nStatus: %2").arg(client.getId()).arg(client.getStatus()));
            m_clientListWidget->addItem(item);
        }
    }
    
    // Hide selected client info when list changes
    m_selectedClientLabel->hide();
}

void MainWindow::setUIEnabled(bool enabled) {
    // Client list depends on connection
    m_clientListWidget->setEnabled(enabled);
}

void MainWindow::updateConnectionStatus() {
    QString status = m_webSocketClient->getConnectionStatus();
    // Always display status in uppercase
    m_connectionStatusLabel->setText(status.toUpper());
    
    if (status == "Connected") {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: green; font-weight: bold; }");
    } else if (status.startsWith("Connecting") || status.startsWith("Reconnecting")) {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: orange; font-weight: bold; }");
    } else {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
    }
}


