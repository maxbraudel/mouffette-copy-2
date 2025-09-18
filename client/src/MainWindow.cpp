#include "MainWindow.h"
#include "ScreenCanvas.h"
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
#include <QFileDialog>
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

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_centralWidget(nullptr),
      m_mainLayout(nullptr),
      m_stackedWidget(nullptr),
      m_clientListPage(nullptr),
      m_connectionLayout(nullptr),
      m_settingsButton(nullptr),
      m_connectToggleButton(nullptr),
      m_connectionStatusLabel(nullptr),
      m_clientListLabel(nullptr),
      m_clientListWidget(nullptr),
      m_noClientsLabel(nullptr),
      m_selectedClientLabel(nullptr),
      m_screenViewWidget(nullptr),
      m_screenViewLayout(nullptr),
      m_clientNameLabel(nullptr),
      m_canvasContainer(nullptr),
      m_canvasStack(nullptr),
      m_screenCanvas(nullptr),
      m_volumeIndicator(nullptr),
      m_loadingSpinner(nullptr),
      m_sendButton(nullptr),
      m_uploadButton(nullptr),
      m_backButton(nullptr),
      m_spinnerOpacity(nullptr),
      m_spinnerFade(nullptr),
      m_canvasOpacity(nullptr),
      m_canvasFade(nullptr),
      m_volumeOpacity(nullptr),
      m_volumeFade(nullptr),
      m_cursorTimer(nullptr),
      m_fileMenu(nullptr),
      m_helpMenu(nullptr),
      m_exitAction(nullptr),
      m_aboutAction(nullptr),
      m_trayIcon(nullptr),
      m_webSocketClient(new WebSocketClient(this)),
      m_statusUpdateTimer(new QTimer(this)),
      m_displaySyncTimer(new QTimer(this)),
      m_reconnectTimer(new QTimer(this)),
      m_reconnectAttempts(0),
      m_maxReconnectDelay(15000),
      m_ignoreSelectionChange(false),
      m_uploadManager(new UploadManager(this)),
      m_watchManager(new WatchManager(this)),
      m_navigationManager(nullptr)
{
    setWindowTitle("Mouffette");
    resize(1280, 900);
    setupUI();
    setupMenuBar();
    setupSystemTray();
    setupVolumeMonitoring();

    // Connect WebSocketClient signals
    connect(m_webSocketClient, &WebSocketClient::connected, this, &MainWindow::onConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_webSocketClient, &WebSocketClient::connectionError, this, &MainWindow::onConnectionError);
    connect(m_webSocketClient, &WebSocketClient::clientListReceived, this, &MainWindow::onClientListReceived);
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    connect(m_webSocketClient, &WebSocketClient::screensInfoReceived, this, &MainWindow::onScreensInfoReceived);
    connect(m_webSocketClient, &WebSocketClient::watchStatusChanged, this, &MainWindow::onWatchStatusChanged);
    connect(m_webSocketClient, &WebSocketClient::messageReceived, this, &MainWindow::onGenericMessageReceived);
    // Forward all generic messages to UploadManager so it can handle incoming upload_* and unload_media when we are the target
    connect(m_webSocketClient, &WebSocketClient::messageReceived, m_uploadManager, &UploadManager::handleIncomingMessage);
    // Upload progress forwards
    connect(m_webSocketClient, &WebSocketClient::uploadProgressReceived, m_uploadManager, &UploadManager::onUploadProgress);
    connect(m_webSocketClient, &WebSocketClient::uploadFinishedReceived, m_uploadManager, &UploadManager::onUploadFinished);
    connect(m_webSocketClient, &WebSocketClient::unloadedReceived, m_uploadManager, &UploadManager::onUnloadedRemote);

    // Managers wiring
    m_uploadManager->setWebSocketClient(m_webSocketClient);
    m_watchManager->setWebSocketClient(m_webSocketClient);

    // UI refresh when upload state changes
    auto applyUploadButtonStyle = [this]() {
        if (!m_uploadButton) return;
        // Base style strings
        const QString greyStyle = "QPushButton { padding: 12px 18px; font-weight: bold; background-color: #666; color: white; border-radius: 5px; } QPushButton:checked { background-color: #444; }";
        const QString blueStyle = "QPushButton { padding: 12px 18px; font-weight: bold; background-color: #2d6cdf; color: white; border-radius: 5px; } QPushButton:checked { background-color: #1f4ea8; }";
        const QString greenStyle = "QPushButton { padding: 12px 18px; font-weight: bold; background-color: #16a34a; color: white; border-radius: 5px; } QPushButton:checked { background-color: #15803d; }";

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
            // Monospace font for stability
#ifdef Q_OS_MACOS
            QFont mono("Menlo");
#else
            QFont mono("Courier New");
#endif
            mono.setPointSize(m_uploadButtonDefaultFont.pointSize());
            mono.setBold(true);
            m_uploadButton->setFont(mono);
        } else if (m_uploadManager->hasActiveUpload()) {
            // Uploaded & resident on target: allow unload
            m_uploadButton->setCheckable(true);
            m_uploadButton->setChecked(true);
            m_uploadButton->setEnabled(true);
            m_uploadButton->setText("Unload medias");
            m_uploadButton->setStyleSheet(greenStyle);
            m_uploadButton->setFont(m_uploadButtonDefaultFont);
        } else {
            // Idle state
            m_uploadButton->setCheckable(false);
            m_uploadButton->setChecked(false);
            m_uploadButton->setEnabled(true);
            m_uploadButton->setText("Upload to Client");
            m_uploadButton->setStyleSheet(greyStyle);
            m_uploadButton->setFont(m_uploadButtonDefaultFont);
        }
    };
    connect(m_uploadManager, &UploadManager::uiStateChanged, this, applyUploadButtonStyle);
    connect(m_uploadManager, &UploadManager::uploadProgress, this, [this, applyUploadButtonStyle](int percent, int filesCompleted, int totalFiles){
        if (!m_uploadButton) return;
        if (m_uploadManager->isUploading() && !m_uploadManager->isCancelling()) {
            m_uploadButton->setText(QString("Downloading (%1/%2) %3%")
                                    .arg(filesCompleted)
                                    .arg(totalFiles)
                                    .arg(percent));
        }
        applyUploadButtonStyle();
        
        // Update individual media progress based on server-acknowledged data
        updateIndividualProgressFromServer(percent, filesCompleted, totalFiles);
    });
    connect(m_uploadManager, &UploadManager::uploadFinished, this, [this, applyUploadButtonStyle](){ applyUploadButtonStyle(); });
    connect(m_uploadManager, &UploadManager::unloaded, this, [this, applyUploadButtonStyle](){ applyUploadButtonStyle(); });

    // Periodic connection status refresh
    m_statusUpdateTimer->setInterval(1000);
    connect(m_statusUpdateTimer, &QTimer::timeout, this, &MainWindow::updateConnectionStatus);
    m_statusUpdateTimer->start();

    // Periodic display sync only when watched
    m_displaySyncTimer->setInterval(3000);
    connect(m_displaySyncTimer, &QTimer::timeout, this, [this]() { if (m_isWatched && m_webSocketClient->isConnected()) syncRegistration(); });
    // Don't start automatically - will be started when watched

    // Smart reconnect timer
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::attemptReconnect);

    connectToServer();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // Block space bar from triggering button presses when focus is on stack/canvas container
    if ((obj == m_stackedWidget || obj == m_canvasStack || obj == m_screenViewWidget) && event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Space) { event->accept(); return true; }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::showScreenView(const ClientInfo& client) {
    if (!m_navigationManager) return;
    m_navigationManager->showScreenView(client);
    // Update upload target
    m_uploadManager->setTargetClientId(client.getId());
}

void MainWindow::showClientListView() {
    if (m_navigationManager) m_navigationManager->showClientList();
    if (m_uploadButton) m_uploadButton->setText("Upload to Client");
    m_uploadManager->setTargetClientId(QString());
}

QWidget* MainWindow::createScreenWidget(const ScreenInfo& screen, int index) {
    // Legacy helper (may be simplified); currently ScreenCanvas draws screens itself; return nullptr
    Q_UNUSED(screen); Q_UNUSED(index); return nullptr;
}

void MainWindow::updateVolumeIndicator() {
    if (!m_volumeIndicator) return;
    int vol = -1;
    if (!m_selectedClient.getId().isEmpty()) {
        vol = m_selectedClient.getVolumePercent();
    }
    if (vol < 0) { m_volumeIndicator->setText("🔈 --"); return; }
    QString icon;
    if (vol == 0) icon = "🔇"; else if (vol < 34) icon = "🔈"; else if (vol < 67) icon = "🔉"; else icon = "🔊";
    m_volumeIndicator->setText(QString("%1 %2%" ).arg(icon).arg(vol));
    if (m_volumeIndicator->isHidden()) m_volumeIndicator->show();
    if (m_volumeOpacity) {
        if (m_volumeOpacity->opacity() < 1.0) m_volumeOpacity->setOpacity(1.0);
    }
}

void MainWindow::onUploadButtonClicked() {
    if (!m_uploadManager) return;
    if (m_uploadManager->isUploading()) { m_uploadManager->requestCancel(); return; }
    if (m_uploadManager->hasActiveUpload()) { m_uploadManager->requestUnload(); return; }
    // Gather media items present on the scene (using unique mediaId for each item)
    QVector<UploadFileInfo> files;
    if (m_screenCanvas && m_screenCanvas->scene()) {
        const QList<QGraphicsItem*> allItems = m_screenCanvas->scene()->items();
        for (QGraphicsItem* it : allItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(it);
            if (!media) continue;
            const QString path = media->sourcePath();
            if (path.isEmpty()) continue;
            QFileInfo fi(path);
            if (!fi.exists() || !fi.isFile()) continue;
            UploadFileInfo info; 
            info.fileId = QUuid::createUuid().toString(QUuid::WithoutBraces); 
            info.mediaId = media->mediaId(); // Use the unique mediaId from the media item
            info.path = fi.absoluteFilePath(); 
            info.name = fi.fileName(); 
            info.size = fi.size();
            files.push_back(info);
            // Map fileId to both mediaId and media item pointer for efficient lookups
            m_mediaIdByFileId.insert(info.fileId, info.mediaId);
            m_itemByFileId.insert(info.fileId, media);
        }
    }
    if (files.isEmpty()) {
        QMessageBox::information(this, "Upload", "Aucun média local à uploader sur le canevas (les éléments doivent provenir de fichiers locaux)." );
        return;
    }

    // Initialize per-media upload state using unique mediaId
    m_mediaIdsBeingUploaded.clear();
    for (const auto& f : files) {
        m_mediaIdsBeingUploaded.insert(f.mediaId);
    }
    // Set initial upload state for all media items being uploaded
    if (m_screenCanvas && m_screenCanvas->scene()) {
        const QList<QGraphicsItem*> allItems = m_screenCanvas->scene()->items();
        for (QGraphicsItem* it : allItems) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                if (m_mediaIdsBeingUploaded.contains(media->mediaId())) {
                    media->setUploadUploading(0);
                }
            }
        }
    }

    // Wire upload manager signals to update progress and completion (connect only once)
    if (m_uploadManager && !m_uploadSignalsConnected) {
        // Note: we do not use aggregate uploadProgress for per-item bars; only per-file signals below
        // Per-file progress signals: only advance the active file's bar; others remain at 0 until their turn
        connect(m_uploadManager, &UploadManager::fileUploadStarted, this, [this](const QString& fileId){
            if (!m_screenCanvas || !m_screenCanvas->scene()) return;
            // Use direct mapping from fileId to media item pointer
            if (auto item = m_itemByFileId.value(fileId)) { 
                item->setUploadUploading(0); 
            }
        });
        // Per-file progress now calculated from server-acknowledged global progress
        // (removed sender-side fileUploadProgress connection to prevent too-fast progress)
        connect(m_uploadManager, &UploadManager::fileUploadFinished, this, [this](const QString& fileId){
            if (!m_screenCanvas || !m_screenCanvas->scene()) return;
            // Use direct mapping from fileId to media item pointer
            if (auto item = m_itemByFileId.value(fileId)) { 
                item->setUploadUploaded(); 
            }
        });
        connect(m_uploadManager, &UploadManager::uploadFinished, this, [this](){
            if (!m_screenCanvas || !m_screenCanvas->scene()) { 
                m_mediaIdsBeingUploaded.clear(); 
                m_mediaIdByFileId.clear();
                m_itemByFileId.clear();
                return; 
            }
            // Set uploaded state for all media items that were being uploaded
            const QList<QGraphicsItem*> allItems = m_screenCanvas->scene()->items();
            for (QGraphicsItem* it : allItems) {
                if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                    if (m_mediaIdsBeingUploaded.contains(media->mediaId())) {
                        media->setUploadUploaded();
                    }
                }
            }
            // Clear tracking data
            m_mediaIdsBeingUploaded.clear();
            m_mediaIdByFileId.clear();
            m_itemByFileId.clear();
        });
        connect(m_uploadManager, &UploadManager::unloaded, this, [this](){
            // Reset to NotUploaded if user toggles unload after upload
            if (!m_screenCanvas || !m_screenCanvas->scene()) return;
            const QList<QGraphicsItem*> allItems = m_screenCanvas->scene()->items();
            for (QGraphicsItem* it : allItems) {
                if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                    // Only reset those that were part of the last upload set if any
                    if (m_mediaIdsBeingUploaded.isEmpty()) { 
                        media->setUploadNotUploaded(); 
                    }
                    else if (m_mediaIdsBeingUploaded.contains(media->mediaId())) {
                        media->setUploadNotUploaded();
                    }
                }
            }
        });
        m_uploadSignalsConnected = true;
    }

    m_uploadManager->toggleUpload(files);
}


void MainWindow::onBackToClientListClicked() { showClientListView(); }

void MainWindow::onSendMediaClicked() {
    // Placeholder: iterate scene media and send placement in future
    QMessageBox::information(this, "Send Media", "Send Media functionality not yet implemented.");
}

void MainWindow::onClientItemClicked(QListWidgetItem* item) {
    if (!item) return;
    int index = m_clientListWidget->row(item);
    if (index >=0 && index < m_availableClients.size()) {
        const ClientInfo& client = m_availableClients[index];
        m_selectedClient = client;
        showScreenView(client);
        if (m_webSocketClient && m_webSocketClient->isConnected()) m_webSocketClient->requestScreens(client.getId());
    }
}

void MainWindow::onGenericMessageReceived(const QJsonObject& message) {
    Q_UNUSED(message);
    // Currently unused; placeholder for future protocol extensions
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
                if (!m_screenCanvas) return;
                if (m_stackedWidget->currentWidget() != m_screenViewWidget) return;
                bool matchWatch = (m_watchManager && targetId == m_watchManager->watchedClientId());
                bool matchSelected = (!m_selectedClient.getId().isEmpty() && targetId == m_selectedClient.getId());
                if (matchWatch || matchSelected) {
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
    // Canvas container previously had a bordered panel look; remove to emulate design-tool feel
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
        "   background-color: transparent; "
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
        // Keep viewport visually seamless inside white container while allowing scene clearing to white
    m_screenCanvas->viewport()->setStyleSheet("background: palette(base); border: none; border-radius: 5px;");
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
    // Apply initial style state machine
    QTimer::singleShot(0, this, [this]() {
        emit m_uploadManager->uiStateChanged();
    });
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
    
    // Start/stop display sync timer based on watch status to prevent unnecessary canvas reloads
    if (watched) {
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

void MainWindow::updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles) {
    if (!m_screenCanvas || !m_screenCanvas->scene() || totalFiles == 0) return;
    
    // Get list of files being uploaded in consistent order
    QStringList orderedFileIds;
    for (auto it = m_itemByFileId.constBegin(); it != m_itemByFileId.constEnd(); ++it) {
        orderedFileIds.append(it.key());
    }
    
    // Calculate progress for each file based on server acknowledgment
    for (int i = 0; i < orderedFileIds.size() && i < totalFiles; ++i) {
        const QString& fileId = orderedFileIds[i];
        ResizableMediaBase* item = m_itemByFileId.value(fileId);
        if (!item) continue;
        
        int fileProgress;
        if (i < filesCompleted) {
            // This file is completely received by server
            fileProgress = 100;
        } else if (i == filesCompleted && filesCompleted < totalFiles) {
            // This is the currently uploading file
            // Estimate progress: remaining global progress for current file
            int progressForCompletedFiles = filesCompleted * 100;
            int totalExpectedProgress = totalFiles * 100;
            int currentFileProgress = (globalPercent * totalFiles) - progressForCompletedFiles;
            fileProgress = std::clamp(currentFileProgress, 0, 100);
        } else {
            // Future files not yet started by server
            fileProgress = 0;
        }
        
        item->setUploadUploading(fileProgress);
    }
}


