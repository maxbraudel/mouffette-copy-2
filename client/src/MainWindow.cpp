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
#include <QPixmap>
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
// Z-ordering constants used throughout the scene
namespace {
constexpr qreal Z_SCREENS = -1000.0;
constexpr qreal Z_MEDIA_BASE = 1.0;
constexpr qreal Z_REMOTE_CURSOR = 10000.0;
constexpr qreal Z_SCENE_OVERLAY = 12000.0; // above all scene content

// Global window content margins (between all content and window borders)
int gWindowContentMarginTop = 20;       // Top margin for all window content
int gWindowContentMarginRight = 20;     // Right margin for all window content
int gWindowContentMarginBottom = 20;    // Bottom margin for all window content
int gWindowContentMarginLeft = 20;      // Left margin for all window content

// Global window appearance variables (edit to customize)
int gWindowBorderRadiusPx = 10;                    // Rounded corner radius (px)

// Global inner content gap between sections inside the window (top container <-> hostname, hostname <-> canvas)
int gInnerContentGap = 20;

// Global dynamicBox configuration for standardized buttons and status indicators
// Edit these values to change all buttons and status boxes at once
int gDynamicBoxMinWidth = 80;         // Minimum width for buttons/status boxes
int gDynamicBoxHeight = 24;           // Fixed height for all elements
int gDynamicBoxBorderRadius = 6;      // Border radius for rounded corners
int gDynamicBoxFontPx = 13;           // Standard font size for buttons/status boxes

// Remote client info container configuration
int gRemoteClientContainerPadding = 6; // Horizontal padding for elements in remote client container


// Note: Border and background colors are now managed in AppColors.h/cpp
// Use AppColors::colorSourceToCss(AppColors::gAppBorderColorSource) and AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource)

// Global title text configuration (for headers like "Connected Clients" and hostname)
int gTitleTextFontSize = 16;          // Title font size (px)
int gTitleTextHeight = 24;            // Title fixed height (px)

// Shared button style helpers using dynamicBox configuration
inline void applyPillBtn(QPushButton* b) {
    if (!b) return;
    // Avoid platform default/autoDefault enlarging the control on macOS
    b->setAutoDefault(false);
    b->setDefault(false);
    b->setFocusPolicy(Qt::NoFocus);
    b->setMinimumWidth(gDynamicBoxMinWidth);
    b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    // Enforce exact visual height via stylesheet (min/max-height) and fixed padding
    b->setStyleSheet(QString(
        "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %4px; border: 1px solid %1;"
        " border-radius: %2px; background-color: %5; color: palette(buttonText);"
        " min-height: %3px; max-height: %3px; text-align: center; }"
        "QPushButton:hover { background-color: %6; }"
        "QPushButton:pressed { background-color: %7; }"
        "QPushButton:disabled { color: palette(mid); border-color: %1; background-color: %8; }"
    ).arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(gDynamicBoxFontPx)
     .arg(AppColors::colorToCss(AppColors::gButtonNormalBg)).arg(AppColors::colorToCss(AppColors::gButtonHoverBg)).arg(AppColors::colorToCss(AppColors::gButtonPressedBg)).arg(AppColors::colorToCss(AppColors::gButtonDisabledBg)));
    // Final enforcement (some styles ignore min/max rules)
    b->setFixedHeight(gDynamicBoxHeight);
}

// Shared title text helper
inline void applyTitleText(QLabel* l) {
    if (!l) return;
    l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    l->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    l->setStyleSheet(QString(
        "QLabel { font-size: %1px; font-weight: bold; color: palette(text); min-height: %2px; max-height: %2px; }"
    ).arg(gTitleTextFontSize).arg(gTitleTextHeight));
    l->setFixedHeight(gTitleTextHeight);
}
inline void applyPrimaryBtn(QPushButton* b) {
    if (!b) return;
    // Avoid platform default/autoDefault enlarging the control on macOS
    b->setAutoDefault(false);
    b->setDefault(false);
    b->setFocusPolicy(Qt::NoFocus);
    b->setMinimumWidth(gDynamicBoxMinWidth);
    b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    b->setStyleSheet(QString(
        "QPushButton { padding: 0px 12px; font-weight: bold; font-size: %4px; border: 1px solid %1;"
        " border-radius: %2px; background-color: %5; color: %9;"
        " min-height: %3px; max-height: %3px; }"
        "QPushButton:hover { background-color: %6; }"
        "QPushButton:pressed { background-color: %7; }"
        "QPushButton:disabled { color: palette(mid); border-color: %1; background-color: %8; }"
    ).arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(gDynamicBoxFontPx)
     .arg(AppColors::colorToCss(AppColors::gButtonPrimaryBg)).arg(AppColors::colorToCss(AppColors::gButtonPrimaryHover)).arg(AppColors::colorToCss(AppColors::gButtonPrimaryPressed)).arg(AppColors::colorToCss(AppColors::gButtonPrimaryDisabled))
     .arg(AppColors::gBrandBlue.name()));
    // Final enforcement (some styles ignore min/max rules)
    b->setFixedHeight(gDynamicBoxHeight);
}
inline void applyStatusBox(QLabel* l, const QString& borderColor, const QString& bgColor, const QString& textColor) {
    if (!l) return;
    l->setMinimumWidth(gDynamicBoxMinWidth);
    l->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet(QString(
        "QLabel { padding: 0px 8px; font-size: %5px; border: 1px solid %1; border-radius: %2px; "
        "background-color: %3; color: %4; font-weight: bold; min-height: %6px; max-height: %6px; }"
    ).arg(borderColor)
     .arg(gDynamicBoxBorderRadius)
     .arg(bgColor)
     .arg(textColor)
     .arg(gDynamicBoxFontPx)
     .arg(gDynamicBoxHeight));
    // Final enforcement
    l->setFixedHeight(gDynamicBoxHeight);
}
}


// A central container that paints a rounded rect with antialiased border
class RoundedContainer : public QWidget {
public:
    RoundedContainer(QWidget* parent = nullptr) : QWidget(parent) {}
    
protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal radius = qMax(0, gWindowBorderRadiusPx);
        QRectF r = rect();
        // Align to half-pixels for crisp 1px strokes
        QRectF borderRect = r.adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(borderRect, radius, radius);

        // Use dynamic palette color that updates automatically with theme changes
        QColor fill = AppColors::getCurrentColor(AppColors::gWindowBackgroundColorSource);
        // Avoid halo on rounded edges by drawing fill with Source composition
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillPath(path, fill);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // No outer border drawing — only rounded fill is rendered
    }
};

// A container that properly clips child widgets to its rounded shape
class ClippedContainer : public QWidget {
public:
    ClippedContainer(QWidget* parent = nullptr) : QWidget(parent) {}

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        updateMaskIfNeeded();
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        updateMaskIfNeeded();
    }

private:
    QSize m_lastMaskSize;  // Cache last size to avoid unnecessary recalculation
    
    void updateMaskIfNeeded() {
        const QSize currentSize = size();
        // Skip if size hasn't changed (common during theme switches, etc.)
        if (currentSize == m_lastMaskSize && !mask().isEmpty()) return;
        
        // Ensure we have a valid size
        if (currentSize.width() <= 0 || currentSize.height() <= 0) return;
        
        // Cache the size
        m_lastMaskSize = currentSize;
        
        // Use more efficient QRegion constructor for rounded rectangles
        const int radius = qMax(0, qMin(gDynamicBoxBorderRadius, qMin(currentSize.width(), currentSize.height()) / 2));
        const QRect r(0, 0, currentSize.width(), currentSize.height());
        
        // Create rounded region more efficiently using ellipse corners
        QRegion region(r);
        if (radius > 0) {
            // Subtract corner rectangles and add back rounded corners
            const int d = radius * 2;
            region -= QRegion(0, 0, radius, radius);                           // top-left corner
            region -= QRegion(r.width() - radius, 0, radius, radius);         // top-right corner  
            region -= QRegion(0, r.height() - radius, radius, radius);        // bottom-left corner
            region -= QRegion(r.width() - radius, r.height() - radius, radius, radius); // bottom-right corner
            
            region += QRegion(0, 0, d, d, QRegion::Ellipse);                           // top-left rounded
            region += QRegion(r.width() - d, 0, d, d, QRegion::Ellipse);               // top-right rounded
            region += QRegion(0, r.height() - d, d, d, QRegion::Ellipse);              // bottom-left rounded  
            region += QRegion(r.width() - d, r.height() - d, d, d, QRegion::Ellipse);  // bottom-right rounded
        }
        
        setMask(region);
    }
};

// Lightweight delegate that draws separators only between items (no line above first, none below last)
class ClientListSeparatorDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        // Supprimer l'effet hover / sélection pour l'item message "no clients" (flags vides)
        if (index.isValid()) {
            Qt::ItemFlags f = index.model() ? index.model()->flags(index) : Qt::NoItemFlags;
            if (f == Qt::NoItemFlags) {
                opt.state &= ~QStyle::State_MouseOver;
                opt.state &= ~QStyle::State_Selected;
                opt.state &= ~QStyle::State_HasFocus;
            }
        }
        QStyledItemDelegate::paint(painter, opt, index);
        if (!index.isValid()) return;
        const QAbstractItemModel* model = index.model();
        if (!model) return;
        // Draw a 1px separator line at the top of items after the first
        if (index.row() > 0) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, false);
            QColor c = AppColors::getCurrentColor(AppColors::gAppBorderColorSource);
            painter->setPen(QPen(c, 1));
            const QRect r = option.rect;
            const int y = r.top();
            painter->drawLine(r.left(), y, r.right(), y);
            painter->restore();
        }
    }
};

bool MainWindow::event(QEvent* event) {
    if (event->type() == QEvent::PaletteChange) {
        // Theme changed - update stylesheets that use ColorSource
        updateStylesheetsForTheme();
    }
    return QMainWindow::event(event);
}

// Ensure placeholder item is visible when there are no clients yet (including connecting state)
void MainWindow::ensureClientListPlaceholder() {
    if (!m_clientListWidget) return;
    if (m_clientListWidget->count() == 0) {
        QListWidgetItem* item = new QListWidgetItem("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
        item->setFlags(Qt::NoItemFlags);
        item->setTextAlignment(Qt::AlignCenter);
        QFont font = item->font(); font.setItalic(true); font.setPointSize(16); item->setFont(font);
        item->setForeground(AppColors::gTextMuted);
        m_clientListWidget->addItem(item);
    }
}

void MainWindow::setRemoteConnectionStatus(const QString& status, bool propagateLoss) {
    if (!m_remoteConnectionStatusLabel) return;
    const QString up = status.toUpper();
    m_remoteConnectionStatusLabel->setText(up);
    
    if (up == "CONNECTED") {
        m_remoteClientConnected = true;
    } else if (up == "DISCONNECTED") {
        m_remoteClientConnected = false;
    } else if (up.startsWith("CONNECTING") || up == "ERROR") {
        m_remoteClientConnected = false;
    }

    // Apply same styling as main connection status with colored background
    QString textColor, bgColor;
    if (up == "CONNECTED") {
        textColor = AppColors::colorToCss(AppColors::gStatusConnectedText);
        bgColor = AppColors::colorToCss(AppColors::gStatusConnectedBg);
        if (m_inlineSpinner && m_inlineSpinner->isSpinning()) {
            m_inlineSpinner->stop();
            m_inlineSpinner->hide();
        } else if (m_inlineSpinner) {
            m_inlineSpinner->hide();
        }
    } else if (up == "ERROR" || up.startsWith("CONNECTING") || up.startsWith("RECONNECTING")) {
        textColor = AppColors::colorToCss(AppColors::gStatusWarningText);
        bgColor = AppColors::colorToCss(AppColors::gStatusWarningBg);
    } else {
        textColor = AppColors::colorToCss(AppColors::gStatusErrorText);
        bgColor = AppColors::colorToCss(AppColors::gStatusErrorBg);
    }
    
    m_remoteConnectionStatusLabel->setStyleSheet(
        QString("QLabel { "
        "    color: %1; "
        "    background-color: %2; "
        "    border: none; "
        "    border-radius: 0px; "
        "    padding: 0px %4px; "
        "    font-size: %3px; "
        "    font-weight: bold; "
        "}").arg(textColor).arg(bgColor).arg(gDynamicBoxFontPx).arg(gRemoteClientContainerPadding)
    );
    // If transitioning into a connecting-like state and list is empty, show placeholder
    if (up == "CONNECTING" || up.startsWith("CONNECTING") || up.startsWith("RECONNECTING")) {
        ensureClientListPlaceholder();
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
            m_uploadButton->setFixedHeight(40);
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
      m_centralWidget(nullptr),
      m_mainLayout(nullptr),
      m_stackedWidget(nullptr),
      m_clientListPage(nullptr),
      m_connectionLayout(nullptr),
      m_settingsButton(nullptr),
      m_connectToggleButton(nullptr),
      m_connectionStatusLabel(nullptr),
      m_localClientInfoContainer(nullptr),
      m_localClientTitleLabel(nullptr),
      m_localNetworkStatusLabel(nullptr),
      m_clientListLabel(nullptr),
      m_clientListWidget(nullptr),
      m_screenViewWidget(nullptr),
      m_screenViewLayout(nullptr),
      m_clientNameLabel(nullptr),
      m_canvasContainer(nullptr),
      m_canvasStack(nullptr),
      m_screenCanvas(nullptr),
      m_volumeIndicator(nullptr),
      m_loadingSpinner(nullptr),
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
      m_uploadManager(new UploadManager(this)),
      m_watchManager(new WatchManager(this)),
      m_fileWatcher(new FileWatcher(this)),
      m_navigationManager(nullptr),
      m_responsiveLayoutManager(new ResponsiveLayoutManager(this))
{
    setWindowTitle("Mouffette");
#if defined(Q_OS_WIN)
    setWindowIcon(QIcon(":/icons/appicon.ico"));
#endif
    // Load persisted settings (server URL, auto-upload)
    {
        QSettings settings("Mouffette", "Client");
        m_serverUrlConfig = settings.value("serverUrl", DEFAULT_SERVER_URL).toString();
        m_autoUploadImportedMedia = settings.value("autoUploadImportedMedia", false).toBool();
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
        g_remoteSceneController = new RemoteSceneController(m_webSocketClient, this);
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    // Ensure no status bar is shown at the bottom
    if (QStatusBar* sb = findChild<QStatusBar*>()) {
        sb->deleteLater();
    }
#endif
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    setupMenuBar();
#endif
    setupSystemTray();
    setupVolumeMonitoring();

    // Connect WebSocketClient signals
    connect(m_webSocketClient, &WebSocketClient::connected, this, &MainWindow::onConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_webSocketClient, &WebSocketClient::connectionError, this, &MainWindow::onConnectionError);
    connect(m_webSocketClient, &WebSocketClient::clientListReceived, this, &MainWindow::onClientListReceived);
    // Immediate status reflection without polling
    connect(m_webSocketClient, &WebSocketClient::connectionStatusChanged, this, [this](const QString& s){ setLocalNetworkStatus(s); });
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    connect(m_webSocketClient, &WebSocketClient::screensInfoReceived, this, &MainWindow::onScreensInfoReceived);
    connect(m_webSocketClient, &WebSocketClient::watchStatusChanged, this, &MainWindow::onWatchStatusChanged);
    connect(m_webSocketClient, &WebSocketClient::dataRequestReceived, this, &MainWindow::onDataRequestReceived);
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
    
    // FileManager: configure callback to send file removal commands to remote clients
    FileManager::setFileRemovalNotifier([this](const QString& fileId, const QList<QString>& clientIds) {
        qDebug() << "MainWindow: FileManager requested removal of file" << fileId << "from clients:" << clientIds;
        if (!m_webSocketClient) {
            qWarning() << "MainWindow: No WebSocket client available for file removal";
            return;
        }
        for (const QString& clientId : clientIds) {
            qDebug() << "MainWindow: Sending remove_file command for" << fileId << "to" << clientId;
            m_webSocketClient->sendRemoveFile(clientId, fileId);
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
        
        
        
        // If button is in overlay, use custom overlay styling
        if (m_uploadButtonInOverlay) {
            if (!m_remoteOverlayActionsEnabled) {
                m_uploadButton->setEnabled(false);
                m_uploadButton->setCheckable(false);
                m_uploadButton->setChecked(false);
                m_uploadButton->setStyleSheet(ScreenCanvas::overlayDisabledButtonStyle());
                m_uploadButton->setFixedHeight(40);
                return;
            }
            const QString overlayIdleStyle = 
                "QPushButton { "
                "    padding: 0px 20px; "
                "    font-weight: bold; "
                "    font-size: 12px; "
                "    color: " + AppColors::colorToCss(AppColors::gOverlayTextColor) + "; "
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
                "}";
            const QString overlayUploadingStyle = 
                "QPushButton { "
                "    padding: 0px 20px; "
                "    font-weight: bold; "
                "    font-size: 12px; "
                "    color: " + AppColors::gBrandBlue.name() + "; "
                "    background: " + AppColors::colorToCss(AppColors::gButtonPrimaryBg) + "; "
                "    border: none; "
                "    border-radius: 0px; "
                "    text-align: center; "
                "} "
                "QPushButton:hover { "
                "    color: " + AppColors::gBrandBlue.name() + "; "
                "    background: " + AppColors::colorToCss(AppColors::gButtonPrimaryHover) + "; "
                "} "
                "QPushButton:pressed { "
                "    color: " + AppColors::gBrandBlue.name() + "; "
                "    background: " + AppColors::colorToCss(AppColors::gButtonPrimaryPressed) + "; "
                "}";
            const QString overlayUnloadStyle = 
                "QPushButton { "
                "    padding: 0px 20px; "
                "    font-weight: bold; "
                "    font-size: 12px; "
                "    color: " + AppColors::colorToCss(AppColors::gMediaUploadedColor) + "; "
                "    background: " + AppColors::colorToCss(AppColors::gStatusConnectedBg) + "; "
                "    border: none; "
                "    border-radius: 0px; "
                "    text-align: center; "
                "} "
                "QPushButton:hover { "
                "    color: " + AppColors::colorToCss(AppColors::gMediaUploadedColor) + "; "
                "    background: rgba(76, 175, 80, 56); "
                "} "
                "QPushButton:pressed { "
                "    color: " + AppColors::colorToCss(AppColors::gMediaUploadedColor) + "; "
                "    background: rgba(76, 175, 80, 77); "
                "}";
            
            const QString target = m_uploadManager->targetClientId();
            const CanvasSession* targetSession = findCanvasSessionByClientId(target);
            const bool sessionHasRemote = targetSession && targetSession->upload.remoteFilesPresent;
            const bool managerHasActiveForTarget = m_uploadManager->hasActiveUpload() &&
                                                   m_uploadManager->activeUploadTargetClientId() == target;
            const bool remoteActive = sessionHasRemote || managerHasActiveForTarget;

            if (m_uploadManager->isUploading()) {
                if (m_uploadManager->isCancelling()) {
                    m_uploadButton->setText("Cancelling…");
                    m_uploadButton->setEnabled(false);
                } else {
                    if (m_uploadButton->text() == "Upload") {
                        m_uploadButton->setText("Preparing");
                    }
                    m_uploadButton->setEnabled(true);
                }
                m_uploadButton->setStyleSheet(overlayUploadingStyle);
            } else if (m_uploadManager->isFinalizing()) {
                m_uploadButton->setText("Finalizing…");
                m_uploadButton->setEnabled(false);
                m_uploadButton->setStyleSheet(overlayUploadingStyle);
            } else if (remoteActive) {
                // If there are newly added items not yet uploaded to the target, switch back to Upload
                const bool hasUnuploaded = hasUnuploadedFilesForTarget(target);
                // If target is unknown for any reason, default to offering Upload rather than Unload
                if (target.isEmpty() || hasUnuploaded) {
                    m_uploadButton->setText("Upload");
                    m_uploadButton->setEnabled(true);
                    m_uploadButton->setStyleSheet(overlayIdleStyle);
                } else {
                    m_uploadButton->setText("Unload");
                    m_uploadButton->setEnabled(true);
                    m_uploadButton->setStyleSheet(overlayUnloadStyle);
                }
            } else {
                m_uploadButton->setText("Upload");
                m_uploadButton->setEnabled(true);
                m_uploadButton->setStyleSheet(overlayIdleStyle);
            }
            m_uploadButton->setFixedHeight(40);
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
    const CanvasSession* targetSession = findCanvasSessionByClientId(target);
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
            // Monospace font for stability
#ifdef Q_OS_MACOS
            QFont mono("Menlo");
#else
            QFont mono("Courier New");
#endif
            mono.setPointSize(m_uploadButtonDefaultFont.pointSize());
            mono.setBold(true);
            m_uploadButton->setFont(mono);
        } else if (m_uploadManager->isFinalizing()) {
            // Waiting for server to ack upload_finished
            m_uploadButton->setCheckable(true);
            m_uploadButton->setChecked(true);
            m_uploadButton->setEnabled(false);
            m_uploadButton->setText("Finalizing…");
            m_uploadButton->setStyleSheet(blueStyle);
            m_uploadButton->setFixedHeight(gDynamicBoxHeight);
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
                m_uploadButton->setFont(m_uploadButtonDefaultFont);
            } else {
                // Uploaded & resident on target: allow unload
                m_uploadButton->setCheckable(true);
                m_uploadButton->setChecked(true);
                m_uploadButton->setEnabled(true);
                m_uploadButton->setText("Remove all files");
                m_uploadButton->setStyleSheet(greenStyle);
                m_uploadButton->setFixedHeight(gDynamicBoxHeight);
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
    if ((obj == m_stackedWidget || obj == m_canvasStack || obj == m_screenViewWidget) && event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Space) { event->accept(); return true; }
    }

    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::createRemoteClientInfoContainer() {
    if (m_remoteClientInfoContainer) {
        return; // Already created
    }
    
    // Create container widget with dynamic box styling and proper clipping
    m_remoteClientInfoContainer = new ClippedContainer();
    
    // Apply dynamic box styling with transparent background and clipping
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
    
    m_remoteClientInfoContainer->setStyleSheet(containerStyle);
    m_remoteClientInfoContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_remoteClientInfoContainer->setMinimumWidth(120); // Reduced minimum to allow proper hostname shrinking
    
    // Create horizontal layout for the container
    QHBoxLayout* containerLayout = new QHBoxLayout(m_remoteClientInfoContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0); // No padding at all
    containerLayout->setSpacing(0); // We'll add separators manually
    
    // Add hostname (keep title styling but transparent background)
    m_clientNameLabel->setStyleSheet(
        QString("QLabel { "
        "    background: transparent; "
        "    border: none; "
        "    padding: 0px %1px; "
        "    font-size: 16px; "
        "    font-weight: bold; "
        "    color: palette(text); "
        "}").arg(gRemoteClientContainerPadding)
    );
    // Allow client name some flexibility but with conservative minimum
    m_clientNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_clientNameLabel->setMinimumWidth(20); // Adjust this value to change hostname minimum width
    containerLayout->addWidget(m_clientNameLabel);
    
    // Add vertical separator
    m_remoteInfoSep1 = new QFrame();
    m_remoteInfoSep1->setFrameShape(QFrame::VLine);
    m_remoteInfoSep1->setFrameShadow(QFrame::Sunken);
    m_remoteInfoSep1->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    m_remoteInfoSep1->setFixedWidth(1);
    containerLayout->addWidget(m_remoteInfoSep1);
    
    // Add status (minimal styling - just inherit from container)
    // Note: Color and font styling will be applied by setRemoteConnectionStatus()
    containerLayout->addWidget(m_remoteConnectionStatusLabel);
    
    // Add vertical separator
    m_remoteInfoSep2 = new QFrame();
    m_remoteInfoSep2->setFrameShape(QFrame::VLine);
    m_remoteInfoSep2->setFrameShadow(QFrame::Sunken);
    m_remoteInfoSep2->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    m_remoteInfoSep2->setFixedWidth(1);
    containerLayout->addWidget(m_remoteInfoSep2);
    
    // Add volume indicator (ensure no individual styling)
    m_volumeIndicator->setStyleSheet(
        QString("QLabel { "
        "    background: transparent; "
        "    border: none; "
        "    padding: 0px %1px; "
        "    font-size: 16px; "
        "    font-weight: bold; "
        "}").arg(gRemoteClientContainerPadding)
    );
    containerLayout->addWidget(m_volumeIndicator);
}
// Remove remote status (and its leading separator) from layout entirely
void MainWindow::removeRemoteStatusFromLayout() {
    if (!m_remoteClientInfoContainer || !m_remoteConnectionStatusLabel) return;
    if (auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout())) {
        int idx = layout->indexOf(m_remoteConnectionStatusLabel);
        if (idx != -1) {
            layout->removeWidget(m_remoteConnectionStatusLabel);
            m_remoteConnectionStatusLabel->setParent(nullptr);
            m_remoteConnectionStatusLabel->hide();
        }
        if (m_remoteInfoSep1 && m_remoteInfoSep1->parent() == m_remoteClientInfoContainer) {
            int sidx = layout->indexOf(m_remoteInfoSep1);
            if (sidx != -1) {
                layout->removeWidget(m_remoteInfoSep1);
                m_remoteInfoSep1->setParent(nullptr);
                m_remoteInfoSep1->hide();
            }
        }
    }
}

// Add remote status (and its leading separator) into layout if missing
void MainWindow::addRemoteStatusToLayout() {
    if (!m_remoteClientInfoContainer || !m_remoteConnectionStatusLabel) return;
    auto* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;
    if (!m_remoteInfoSep1) {
        m_remoteInfoSep1 = new QFrame();
        m_remoteInfoSep1->setFrameShape(QFrame::VLine);
        m_remoteInfoSep1->setFrameShadow(QFrame::Sunken);
        m_remoteInfoSep1->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
        m_remoteInfoSep1->setFixedWidth(1);
    }
    // Remove from current position (if any) to re-insert at a fixed index
    int sepIdx = layout->indexOf(m_remoteInfoSep1);
    if (sepIdx != -1) {
        layout->removeWidget(m_remoteInfoSep1);
    }
    int statusIdx = layout->indexOf(m_remoteConnectionStatusLabel);
    if (statusIdx != -1) {
        layout->removeWidget(m_remoteConnectionStatusLabel);
    }

    // Insert after hostname label (m_clientNameLabel) to guarantee order: hostname → sep1 → status
    int baseIdx = 0;
    if (m_clientNameLabel && m_clientNameLabel->parent() == m_remoteClientInfoContainer) {
        int nameIdx = layout->indexOf(m_clientNameLabel);
        if (nameIdx != -1) baseIdx = nameIdx + 1;
    }
    layout->insertWidget(baseIdx, m_remoteInfoSep1);
    m_remoteInfoSep1->setParent(m_remoteClientInfoContainer);
    m_remoteInfoSep1->show();

    layout->insertWidget(baseIdx + 1, m_remoteConnectionStatusLabel);
    m_remoteConnectionStatusLabel->setParent(m_remoteClientInfoContainer);
    m_remoteConnectionStatusLabel->show();
}
// Remove the volume indicator (and its preceding separator) from the layout entirely
void MainWindow::removeVolumeIndicatorFromLayout() {
    if (!m_remoteClientInfoContainer || !m_volumeIndicator) return;
    if (QHBoxLayout* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout())) {
        // Remove volume label
        int idx = layout->indexOf(m_volumeIndicator);
        if (idx != -1) {
            layout->removeWidget(m_volumeIndicator);
            m_volumeIndicator->setParent(nullptr);
            m_volumeIndicator->hide();
        }
        // Remove the trailing separator if present and parented here
        if (m_remoteInfoSep2 && m_remoteInfoSep2->parent() == m_remoteClientInfoContainer) {
            int sidx = layout->indexOf(m_remoteInfoSep2);
            if (sidx != -1) {
                layout->removeWidget(m_remoteInfoSep2);
                m_remoteInfoSep2->setParent(nullptr);
                m_remoteInfoSep2->hide();
            }
        }
    }
}

// Add the volume indicator (and its separator) back into the layout if missing
void MainWindow::addVolumeIndicatorToLayout() {
    if (!m_remoteClientInfoContainer || !m_volumeIndicator) return;
    QHBoxLayout* layout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoContainer->layout());
    if (!layout) return;
    // Determine positions: after status label; we added sep2 just after status in createRemoteClientInfoContainer
    // Ensure separator exists
    if (!m_remoteInfoSep2) {
        m_remoteInfoSep2 = new QFrame();
        m_remoteInfoSep2->setFrameShape(QFrame::VLine);
        m_remoteInfoSep2->setFrameShadow(QFrame::Sunken);
        m_remoteInfoSep2->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
        m_remoteInfoSep2->setFixedWidth(1);
    }
    // Remove current instances (if any) so we can reinsert at a fixed index
    int sepIdx = layout->indexOf(m_remoteInfoSep2);
    if (sepIdx != -1) {
        layout->removeWidget(m_remoteInfoSep2);
    }
    int volIdx = layout->indexOf(m_volumeIndicator);
    if (volIdx != -1) {
        layout->removeWidget(m_volumeIndicator);
    }

    // Insert after status if present; otherwise after hostname
    int baseIdx = -1;
    if (m_remoteConnectionStatusLabel && m_remoteConnectionStatusLabel->parent() == m_remoteClientInfoContainer) {
        baseIdx = layout->indexOf(m_remoteConnectionStatusLabel);
    }
    if (baseIdx == -1 && m_clientNameLabel && m_clientNameLabel->parent() == m_remoteClientInfoContainer) {
        baseIdx = layout->indexOf(m_clientNameLabel);
    }
    if (baseIdx == -1) baseIdx = layout->count() - 1; // Fallback to end if neither found

    // Guarantee order: ... status → sep2 → volume (or hostname → sep2 → volume when status absent)
    layout->insertWidget(baseIdx + 1, m_remoteInfoSep2);
    m_remoteInfoSep2->setParent(m_remoteClientInfoContainer);
    m_remoteInfoSep2->show();

    layout->insertWidget(baseIdx + 2, m_volumeIndicator);
    m_volumeIndicator->setParent(m_remoteClientInfoContainer);
    m_volumeIndicator->show();
}

void MainWindow::createLocalClientInfoContainer() {
    if (m_localClientInfoContainer) {
        return; // Already created
    }
    
    // Create "You" title label first
    m_localClientTitleLabel = new QLabel("You");
    m_localClientTitleLabel->setStyleSheet(
        QString("QLabel { "
        "    background: transparent; "
        "    border: none; "
        "    padding: 0px %1px; "
        "    font-size: 16px; "
        "    font-weight: bold; "
        "    color: palette(text); "
        "}").arg(gRemoteClientContainerPadding)
    );
    m_localClientTitleLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    
    // Create network status label
    m_localNetworkStatusLabel = new QLabel("DISCONNECTED");
    // Make status label non-shrinkable with fixed width
    m_localNetworkStatusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_localNetworkStatusLabel->setFixedWidth(120); // Fixed width for consistency
    m_localNetworkStatusLabel->setAlignment(Qt::AlignCenter); // Center the text
    // Note: Color and font styling will be applied by setLocalNetworkStatus()
    
    // Create container widget with same styling as remote client container and proper clipping
    m_localClientInfoContainer = new ClippedContainer();
    
    // Apply same dynamic box styling as remote container with clipping
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
    
    m_localClientInfoContainer->setStyleSheet(containerStyle);
    m_localClientInfoContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_localClientInfoContainer->setMinimumWidth(120); // Same minimum as remote container
    
    // Create horizontal layout for the container
    QHBoxLayout* containerLayout = new QHBoxLayout(m_localClientInfoContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0); // No padding at all
    containerLayout->setSpacing(0); // We'll add separators manually
    
    // Add "You" title (same styling as hostname in remote container)
    containerLayout->addWidget(m_localClientTitleLabel);
    
    // Add vertical separator
    QFrame* separator = new QFrame();
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    separator->setFixedWidth(1);
    containerLayout->addWidget(separator);
    
    // Add network status (styling will be applied by setLocalNetworkStatus())
    containerLayout->addWidget(m_localNetworkStatusLabel);
}

void MainWindow::setLocalNetworkStatus(const QString& status) {
    if (!m_localNetworkStatusLabel) return;
    const QString up = status.toUpper();
    m_localNetworkStatusLabel->setText(up);
    
    // Apply same styling as remote connection status
    QString textColor, bgColor;
    if (up == "CONNECTED") {
        textColor = AppColors::colorToCss(AppColors::gStatusConnectedText);
        bgColor = AppColors::colorToCss(AppColors::gStatusConnectedBg);
    } else if (up == "ERROR" || up.startsWith("CONNECTING") || up.startsWith("RECONNECTING")) {
        textColor = AppColors::colorToCss(AppColors::gStatusWarningText);
        bgColor = AppColors::colorToCss(AppColors::gStatusWarningBg);
    } else {
        textColor = AppColors::colorToCss(AppColors::gStatusErrorText);
        bgColor = AppColors::colorToCss(AppColors::gStatusErrorBg);
    }
    
    m_localNetworkStatusLabel->setStyleSheet(
        QString("QLabel { "
        "    color: %1; "
        "    background-color: %2; "
        "    border: none; "
        "    border-radius: 0px; "
        "    padding: 0px %4px; "
        "    font-size: %3px; "
        "    font-weight: bold; "
        "}").arg(textColor).arg(bgColor).arg(gDynamicBoxFontPx).arg(gRemoteClientContainerPadding)
    );
}

void MainWindow::initializeRemoteClientInfoInTopBar() {
    // Create the container if it doesn't exist
    if (!m_remoteClientInfoContainer) {
        createRemoteClientInfoContainer();
    }
    
    // Create wrapper widget to hold container + inline spinner side by side (once)
    if (!m_remoteClientInfoWrapper) {
        m_remoteClientInfoWrapper = new QWidget();
        auto* wrapperLayout = new QHBoxLayout(m_remoteClientInfoWrapper);
        wrapperLayout->setContentsMargins(0, 0, 0, 0);
        wrapperLayout->setSpacing(8); // 8px gap between container and spinner
        wrapperLayout->addWidget(m_remoteClientInfoContainer);

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
        if (m_remoteClientInfoContainer->parent() != m_remoteClientInfoWrapper) {
            m_remoteClientInfoContainer->setParent(m_remoteClientInfoWrapper);
        }
        auto* wrapperLayout = qobject_cast<QHBoxLayout*>(m_remoteClientInfoWrapper->layout());
        if (wrapperLayout && wrapperLayout->indexOf(m_remoteClientInfoContainer) == -1) {
            wrapperLayout->insertWidget(0, m_remoteClientInfoContainer);
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

QString MainWindow::makeIdentityKey(const QString& machineName, const QString& platform) const {
    QString normalizedName = machineName.trimmed().toLower();
    if (normalizedName.isEmpty()) normalizedName = QStringLiteral("unknown");
    QString normalizedPlatform = platform.trimmed().toLower();
    if (normalizedPlatform.isEmpty()) normalizedPlatform = QStringLiteral("unknown");
    return normalizedName + QStringLiteral("|") + normalizedPlatform;
}

MainWindow::CanvasSession* MainWindow::findCanvasSession(const QString& identityKey) {
    auto it = m_canvasSessions.find(identityKey);
    if (it == m_canvasSessions.end()) {
        return nullptr;
    }
    return &it.value();
}

const MainWindow::CanvasSession* MainWindow::findCanvasSession(const QString& identityKey) const {
    auto it = m_canvasSessions.constFind(identityKey);
    if (it == m_canvasSessions.constEnd()) {
        return nullptr;
    }
    return &it.value();
}

MainWindow::CanvasSession* MainWindow::findCanvasSessionByClientId(const QString& clientId) {
    if (clientId.isEmpty()) {
        return nullptr;
    }

    for (auto it = m_canvasSessions.begin(); it != m_canvasSessions.end(); ++it) {
        if (it.value().clientId == clientId) {
            return &it.value();
        }
    }

    return nullptr;
}

const MainWindow::CanvasSession* MainWindow::findCanvasSessionByClientId(const QString& clientId) const {
    if (clientId.isEmpty()) {
        return nullptr;
    }

    for (auto it = m_canvasSessions.constBegin(); it != m_canvasSessions.constEnd(); ++it) {
        if (it.value().clientId == clientId) {
            return &it.value();
        }
    }

    return nullptr;
}

MainWindow::CanvasSession& MainWindow::ensureCanvasSession(const ClientInfo& client) {
    QString identity = !client.identityKey().isEmpty() ? client.identityKey() : makeIdentityKey(client.getMachineName(), client.getPlatform());
    CanvasSession* existing = findCanvasSession(identity);
    if (!existing) {
        CanvasSession session;
        session.identityKey = identity;
        session.clientId = client.getId();
        session.lastClientInfo = client;
        session.lastClientInfo.setIdentityKey(identity);
        session.lastClientInfo.setFromMemory(true);
        session.lastClientInfo.setOnline(client.isOnline());
    session.remoteContentClearedOnDisconnect = !client.isOnline();
    session.canvas = new ScreenCanvas(m_canvasHostStack);
        session.canvas->setWebSocketClient(m_webSocketClient);
        auto inserted = m_canvasSessions.insert(identity, session);
        CanvasSession& stored = inserted.value();
        configureCanvasSession(stored);
        if (stored.canvas) {
            if (m_canvasHostStack && m_canvasHostStack->indexOf(stored.canvas) == -1) {
                m_canvasHostStack->addWidget(stored.canvas);
            }
            if (!stored.clientId.isEmpty()) {
                stored.canvas->setRemoteSceneTarget(stored.clientId, stored.lastClientInfo.getMachineName());
            }
        }
        if (stored.lastClientInfo.isOnline()) {
            stored.remoteContentClearedOnDisconnect = false;
        }
        return stored;
    }

    CanvasSession& stored = *existing;
    stored.clientId = client.getId();
    stored.lastClientInfo = client;
    stored.lastClientInfo.setIdentityKey(identity);
    stored.lastClientInfo.setFromMemory(true);
    stored.lastClientInfo.setOnline(client.isOnline());
    if (client.isOnline()) {
        stored.remoteContentClearedOnDisconnect = false;
    }
    if (!stored.canvas) {
        stored.canvas = new ScreenCanvas(m_canvasHostStack);
        stored.canvas->setWebSocketClient(m_webSocketClient);
        stored.connectionsInitialized = false;
    }
    configureCanvasSession(stored);
    if (stored.canvas) {
        if (!stored.clientId.isEmpty()) {
            stored.canvas->setRemoteSceneTarget(stored.clientId, stored.lastClientInfo.getMachineName());
        }
        if (m_canvasHostStack && m_canvasHostStack->indexOf(stored.canvas) == -1) {
            m_canvasHostStack->addWidget(stored.canvas);
        }
    }
    return stored;
}

void MainWindow::configureCanvasSession(CanvasSession& session) {
    if (!session.canvas) return;

    session.canvas->setWebSocketClient(m_webSocketClient);
    session.canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    session.canvas->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    session.canvas->setFocusPolicy(Qt::StrongFocus);
    session.canvas->installEventFilter(this);

    if (session.canvas->viewport()) {
        QWidget* viewport = session.canvas->viewport();
        viewport->setAttribute(Qt::WA_StyledBackground, true);
        viewport->setAutoFillBackground(true);
        viewport->setStyleSheet("background: palette(base); border: none; border-radius: 5px;");
        viewport->installEventFilter(this);
    }

    if (!session.connectionsInitialized) {
        connect(session.canvas, &ScreenCanvas::mediaItemAdded, this,
                [this, identity=session.identityKey](ResizableMediaBase* mediaItem) {
                    if (m_fileWatcher && mediaItem && !mediaItem->sourcePath().isEmpty()) {
                        m_fileWatcher->watchMediaItem(mediaItem);
                        qDebug() << "MainWindow: Added media item to file watcher:" << mediaItem->sourcePath();
                    }
                    auto it = m_canvasSessions.find(identity);
                    if (it != m_canvasSessions.end()) {
                        it->lastClientInfo.setFromMemory(true);
                    }
                    if (m_autoUploadImportedMedia && m_uploadManager && !m_uploadManager->isUploading() && !m_uploadManager->isCancelling()) {
                        QTimer::singleShot(0, this, [this]() { onUploadButtonClicked(); });
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

void MainWindow::switchToCanvasSession(const QString& identityKey) {
    // Navigation between clients should NOT trigger unload - uploads persist per session
    // Unload only happens when explicitly requested via button or when remote disconnects
    
    CanvasSession* session = findCanvasSession(identityKey);
    if (!session || !session->canvas) return;

    m_activeSessionIdentity = identityKey;
    m_screenCanvas = session->canvas;
    if (m_navigationManager) {
        m_navigationManager->setActiveCanvas(m_screenCanvas);
    }

    if (m_canvasHostStack) {
        if (m_canvasHostStack->indexOf(session->canvas) == -1) {
            m_canvasHostStack->addWidget(session->canvas);
        }
        m_canvasHostStack->setCurrentWidget(session->canvas);
    }

    session->canvas->setFocus(Qt::OtherFocusReason);
    if (!session->clientId.isEmpty()) {
        session->canvas->setRemoteSceneTarget(session->clientId, session->lastClientInfo.getMachineName());
    }

    // Set upload manager target to restore per-session upload state
    if (m_uploadManager) {
        m_uploadManager->setTargetClientId(session->clientId);
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
    const QString targetId = session.clientId;
    if (targetId.isEmpty()) {
        session.remoteContentClearedOnDisconnect = true;
        return;
    }

    m_uploadManager->setTargetClientId(targetId);

    if (attemptRemote && m_webSocketClient && m_webSocketClient->isConnected()) {
        if (m_uploadManager->isUploading() || m_uploadManager->isFinalizing()) {
            m_uploadManager->requestCancel();
        } else if (m_uploadManager->hasActiveUpload()) {
            m_uploadManager->requestUnload();
        } else {
            m_uploadManager->requestRemoval(targetId);
        }

        m_webSocketClient->sendRemoteSceneStop(targetId);
    }

    FileManager::instance().unmarkAllForClient(targetId);

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

void MainWindow::markAllSessionsOffline() {
    for (auto it = m_canvasSessions.begin(); it != m_canvasSessions.end(); ++it) {
        CanvasSession& session = it.value();
        session.lastClientInfo.setIdentityKey(session.identityKey);
        session.lastClientInfo.setFromMemory(true);
        session.lastClientInfo.setOnline(false);
    }
}

QList<ClientInfo> MainWindow::buildDisplayClientList(const QList<ClientInfo>& connectedClients) {
    QList<ClientInfo> result;
    markAllSessionsOffline();
    QSet<QString> identitiesSeen;

    for (ClientInfo client : connectedClients) {
        QString identity = !client.identityKey().isEmpty() ? client.identityKey() : makeIdentityKey(client.getMachineName(), client.getPlatform());
        client.setIdentityKey(identity);
        client.setOnline(true);

        if (CanvasSession* session = findCanvasSession(identity)) {
            session->clientId = client.getId();
            session->lastClientInfo = client;
            session->lastClientInfo.setIdentityKey(identity);
            session->lastClientInfo.setFromMemory(true);
            session->lastClientInfo.setOnline(true);
            session->remoteContentClearedOnDisconnect = false;
            if (session->canvas && !session->clientId.isEmpty()) {
                session->canvas->setRemoteSceneTarget(session->clientId, session->lastClientInfo.getMachineName());
            }
            client.setFromMemory(true);
            client.setId(session->clientId);
        } else {
            client.setFromMemory(false);
        }

        identitiesSeen.insert(identity);
        result.append(client);
    }

    for (auto it = m_canvasSessions.constBegin(); it != m_canvasSessions.constEnd(); ++it) {
        const CanvasSession& session = it.value();
        if (identitiesSeen.contains(session.identityKey)) continue;
        ClientInfo info = session.lastClientInfo;
        info.setIdentityKey(session.identityKey);
        if (!session.clientId.isEmpty()) {
            info.setId(session.clientId);
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
    for (auto it = m_canvasSessions.constBegin(); it != m_canvasSessions.constEnd(); ++it) {
        const CanvasSession& session = it.value();
        if (session.clientId == clientId) {
            return session.canvas;
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
            if (CanvasSession* session = findCanvasSessionByClientId(clientId)) {
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
    session.upload.mediaIdsBeingUploaded.clear();
    session.upload.mediaIdByFileId.clear();
    session.upload.itemsByFileId.clear();
    session.upload.currentUploadFileOrder.clear();
    session.upload.serverCompletedFileIds.clear();
    session.upload.serverPerFileProgressActive.clear();
    session.upload.receivingFilesToastShown = false;
    if (!session.upload.activeUploadId.isEmpty()) {
        m_uploadSessionByUploadId.remove(session.upload.activeUploadId);
        session.upload.activeUploadId.clear();
    }
    if (m_activeUploadSessionIdentity == session.identityKey) {
        m_activeUploadSessionIdentity.clear();
    }
    if (m_uploadManager && m_uploadManager->activeSessionIdentity() == session.identityKey) {
        m_uploadManager->setActiveSessionIdentity(QString());
    }
}



void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
}

void MainWindow::showScreenView(const ClientInfo& client) {
    if (!m_navigationManager) return;
    CanvasSession& session = ensureCanvasSession(client);
    const bool sessionHasActiveScreens = session.canvas && session.canvas->hasActiveScreens();
    const bool sessionHasStoredScreens = !session.lastClientInfo.getScreens().isEmpty();
    const bool hasCachedContent = sessionHasActiveScreens || sessionHasStoredScreens;
    switchToCanvasSession(session.identityKey);
    m_activeRemoteClientId = session.clientId;
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
    m_uploadManager->setTargetClientId(session.clientId);

    // Show remote client info wrapper when viewing a client
    if (m_remoteClientInfoWrapper) {
        m_remoteClientInfoWrapper->setVisible(true);
    }
    if (m_remoteClientInfoContainer) {
        m_remoteClientInfoContainer->setVisible(true);
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

void MainWindow::updateClientNameDisplay(const ClientInfo& client) {
    if (!m_clientNameLabel) return;

    QString name = client.getMachineName().trimmed();
    QString platform = client.getPlatform().trimmed();

    if (name.isEmpty()) {
        name = tr("Unknown Machine");
    }

    QString text = name;
    if (!platform.isEmpty()) {
        text = QStringLiteral("%1 (%2)").arg(name, platform);
    }

    m_clientNameLabel->setText(text);

    if (m_remoteClientInfoContainer) {
        m_remoteClientInfoContainer->setToolTip(text);
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
    if (m_remoteClientInfoContainer) {
        m_remoteClientInfoContainer->setVisible(false);
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
    // Don't force-show here; presence in layout is managed elsewhere
    if (m_volumeOpacity) {
        if (m_volumeOpacity->opacity() < 1.0) m_volumeOpacity->setOpacity(1.0);
    }
}

void MainWindow::onUploadButtonClicked() {
    if (!m_uploadManager) return;

    CanvasSession* session = findCanvasSession(m_activeSessionIdentity);
    if (!session || !session->canvas) return;

    ScreenCanvas* canvas = session->canvas;
    auto& upload = session->upload;

    if (m_uploadManager->isUploading()) {
        if (m_activeUploadSessionIdentity == session->identityKey) {
            m_uploadManager->requestCancel();
            TOAST_WARNING("Upload cancelled");
        } else {
            TOAST_WARNING("Another client upload is currently in progress. Please wait for it to finish.");
        }
        return;
    }

    const QString targetClientId = session->clientId;
    if (targetClientId.isEmpty()) {
        TOAST_ERROR("No remote client selected for upload");
        return;
    }
    m_uploadManager->setTargetClientId(targetClientId);

    const QString clientLabel = session->lastClientInfo.getDisplayText().isEmpty()
        ? targetClientId
        : session->lastClientInfo.getDisplayText();

    const bool managerHasActive = m_uploadManager->hasActiveUpload() &&
        m_uploadManager->activeUploadTargetClientId() == targetClientId;
    const bool sessionHasRemote = upload.remoteFilesPresent;
    const bool hasRemoteFiles = sessionHasRemote || managerHasActive;
    
    qDebug() << "=== Upload Button Clicked Debug ===";
    qDebug() << "Target:" << targetClientId;
    qDebug() << "upload.remoteFilesPresent:" << upload.remoteFilesPresent;
    qDebug() << "sessionHasRemote:" << sessionHasRemote;
    qDebug() << "managerHasActive:" << managerHasActive;
    qDebug() << "  - hasActiveUpload:" << m_uploadManager->hasActiveUpload();
    qDebug() << "  - activeTargetClientId:" << m_uploadManager->activeUploadTargetClientId();
    qDebug() << "  - lastRemovalClientId:" << m_uploadManager->lastRemovalClientId();
    qDebug() << "hasRemoteFiles:" << hasRemoteFiles;

    upload.itemsByFileId.clear();
    upload.mediaIdByFileId.clear();
    upload.currentUploadFileOrder.clear();
    upload.mediaIdsBeingUploaded.clear();
    upload.serverCompletedFileIds.clear();
    upload.serverPerFileProgressActive.clear();
    upload.receivingFilesToastShown = false;

    QVector<UploadFileInfo> files;
    QList<ResizableMediaBase*> mediaItemsToRemove;
    QSet<QString> processedFileIds;

    if (canvas->scene()) {
        const QList<QGraphicsItem*> allItems = canvas->scene()->items();
        for (QGraphicsItem* it : allItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(it);
            if (!media) continue;

            const QString path = media->sourcePath();
            if (path.isEmpty()) continue;

            QFileInfo fi(path);
            if (!fi.exists() || !fi.isFile()) {
                mediaItemsToRemove.append(media);
                continue;
            }

            const QString fileId = media->fileId();
            if (fileId.isEmpty()) {
                qWarning() << "MainWindow: Media item has no fileId, skipping:" << media->mediaId();
                continue;
            }

            const bool alreadyOnTarget = FileManager::instance().isFileUploadedToClient(fileId, targetClientId);
            if (!processedFileIds.contains(fileId) && !alreadyOnTarget) {
                UploadFileInfo info;
                info.fileId = fileId;
                info.mediaId = media->mediaId();
                info.path = fi.absoluteFilePath();
                info.name = fi.fileName();
                info.extension = fi.suffix();
                info.size = fi.size();
                files.push_back(info);
                processedFileIds.insert(fileId);
                upload.currentUploadFileOrder.append(fileId);
            }

            if (!alreadyOnTarget) {
                upload.itemsByFileId[fileId].append(media);
            }
        }
    }

    for (ResizableMediaBase* mediaItem : mediaItemsToRemove) {
        if (m_fileWatcher) {
            m_fileWatcher->unwatchMediaItem(mediaItem);
        }
        QTimer::singleShot(0, [itemPtr = mediaItem]() {
            itemPtr->prepareForDeletion();
            if (itemPtr->scene()) itemPtr->scene()->removeItem(itemPtr);
            delete itemPtr;
        });
    }

    if (!mediaItemsToRemove.isEmpty()) {
        canvas->refreshInfoOverlay();
        TOAST_WARNING(QString("%1 media item(s) removed - source files not found").arg(mediaItemsToRemove.size()));
    }

    if (files.isEmpty()) {
        if (hasRemoteFiles) {
            bool promotedAny = false;
            if (canvas->scene()) {
                const QList<QGraphicsItem*> allItems = canvas->scene()->items();
                for (QGraphicsItem* it : allItems) {
                    if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                        const QString mediaId = media->mediaId();
                        if (mediaId.isEmpty()) continue;
                        if (!FileManager::instance().isMediaUploadedToClient(mediaId, targetClientId)) {
                            FileManager::instance().markMediaUploadedToClient(mediaId, targetClientId);
                            media->setUploadUploaded();
                            promotedAny = true;
                        }
                    }
                }
            }
            if (promotedAny) {
                emit m_uploadManager->uiStateChanged();
                TOAST_SUCCESS("All media already synchronized with remote client");
                return;
            }

            if (sessionHasRemote) {
                qDebug() << "Requesting remote removal for" << targetClientId << "(session" << session->identityKey << ")";
                m_uploadManager->setActiveSessionIdentity(session->identityKey);
                m_uploadManager->requestRemoval(targetClientId);
                TOAST_INFO(QString("Requesting remote removal from %1…").arg(clientLabel));
                return;
            }

            if (managerHasActive) {
                m_uploadManager->requestUnload();
            } else {
                TOAST_INFO("Remote media already cleared");
            }
        } else {
            TOAST_INFO("No new media to upload");
            qDebug() << "Upload skipped: aucun média local nouveau (popup supprimée).";
        }
        return;
    }

    for (const auto& f : files) {
        const QList<QString> allMediaIds = FileManager::instance().getMediaIdsForFile(f.fileId);
        for (const QString& mediaId : allMediaIds) {
            upload.mediaIdsBeingUploaded.insert(mediaId);
        }
    }

    m_uploadManager->clearLastRemovalClientId();

    if (canvas->scene()) {
        const QList<QGraphicsItem*> allItems = canvas->scene()->items();
        for (QGraphicsItem* it : allItems) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                if (upload.mediaIdsBeingUploaded.contains(media->mediaId())) {
                    media->setUploadUploading(0);
                }
            }
        }
    }

    if (m_uploadManager && !m_uploadSignalsConnected) {
        connect(m_uploadManager, &UploadManager::fileUploadStarted, this, [this](const QString& fileId) {
            if (CanvasSession* session = sessionForActiveUpload()) {
                if (!session->canvas || !session->canvas->scene()) return;
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
                if (session->upload.serverPerFileProgressActive.contains(fileId)) return;
                const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
                for (ResizableMediaBase* item : items) {
                    if (item && item->uploadState() != ResizableMediaBase::UploadState::Uploaded) {
                        item->setUploadUploading(qMin(99, percent));
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
                    ? session->clientId
                    : session->lastClientInfo.getDisplayText();
                TOAST_INFO(QString("Remote client %1 is receiving files...").arg(label));
                session->upload.receivingFilesToastShown = true;
            }

            for (auto it = filePercents.constBegin(); it != filePercents.constEnd(); ++it) {
                const QString& fid = it.key();
                const int p = std::clamp(it.value(), 0, 100);
                session->upload.serverPerFileProgressActive.insert(fid);
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
                    ? session->clientId
                    : session->lastClientInfo.getDisplayText();
                TOAST_SUCCESS(QString("Upload completed successfully to %1").arg(label));
                session->upload.remoteFilesPresent = true;
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
                    session = findCanvasSessionByClientId(lastClientId);
                }
            }

            if (session) {
                const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                    ? session->clientId
                    : session->lastClientInfo.getDisplayText();
                TOAST_INFO(QString("All files removed from %1").arg(label));

                session->upload.remoteFilesPresent = false;
                if (session->canvas && session->canvas->scene()) {
                    const QList<QGraphicsItem*> allItems = session->canvas->scene()->items();
                    for (QGraphicsItem* it : allItems) {
                        if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                            if (session->upload.mediaIdsBeingUploaded.isEmpty() ||
                                session->upload.mediaIdsBeingUploaded.contains(media->mediaId())) {
                                media->setUploadNotUploaded();
                            }
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

    TOAST_INFO(QString("Starting upload of %1 file(s) to %2...").arg(files.size()).arg(clientLabel));

    upload.remoteFilesPresent = false;
    m_activeUploadSessionIdentity = session->identityKey;
    m_uploadManager->setActiveSessionIdentity(session->identityKey);

    m_uploadManager->toggleUpload(files);

    if (m_uploadManager->isUploading()) {
        upload.activeUploadId = m_uploadManager->currentUploadId();
        if (!upload.activeUploadId.isEmpty()) {
            m_uploadSessionByUploadId.insert(upload.activeUploadId, session->identityKey);
        }
    } else if (m_activeUploadSessionIdentity == session->identityKey) {
        m_activeUploadSessionIdentity.clear();
        m_uploadManager->setActiveSessionIdentity(QString());
    }
}


void MainWindow::onBackToClientListClicked() { showClientListView(); }


void MainWindow::onClientItemClicked(QListWidgetItem* item) {
    if (!item) return;
    int index = m_clientListWidget->row(item);
    if (index >=0 && index < m_availableClients.size()) {
        ClientInfo client = m_availableClients[index];
        CanvasSession& session = ensureCanvasSession(client);
        switchToCanvasSession(session.identityKey);
        m_selectedClient = session.lastClientInfo;
        showScreenView(session.lastClientInfo);
        // ScreenNavigationManager will request screens; no need to duplicate here
    }
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
    if (m_clientListWidget) {
        m_clientListWidget->setStyleSheet(
            QString("QListWidget { "
            "   border: 1px solid %1; "
            "   border-radius: 5px; "
            "   padding: 0px; "
            "   background-color: %2; "
            "   outline: none; "
            "}"
            "QListWidget::item { "
            "   padding: 10px; "
            // No per-item borders; separators are painted via a custom delegate
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
            "   background-color: " + AppColors::colorToCss(AppColors::gHoverHighlight) + "; "
            "   color: palette(text); "
            "}").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)).arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
        );
    }
    
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

    if (m_canvasContainer) {
        m_canvasContainer->setStyleSheet(
            QString("QWidget#CanvasContainer { "
            "   background-color: %2; "
            "   border: 1px solid %1; "
            "   border-radius: 5px; "
            "}").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)).arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
        );
    }
    
    // Update remote client info container border
    if (m_remoteClientInfoContainer) {
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
        m_remoteClientInfoContainer->setStyleSheet(containerStyle);
    }
    
    // Update local client info container border
    if (m_localClientInfoContainer) {
        const QString containerStyle = QString(
            "QWidget { "
            "    background-color: transparent; "
            "    color: palette(button-text); "
            "    border: 1px solid %3; "
            "    border-radius: %1px; "
            "    min-height: %2px; "
            "}"
        ).arg(gDynamicBoxBorderRadius).arg(gDynamicBoxHeight).arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource));
        m_localClientInfoContainer->setStyleSheet(containerStyle);
    }
    
    // Update separators in remote client info
    QList<QFrame*> separators = m_remoteClientInfoContainer ? m_remoteClientInfoContainer->findChildren<QFrame*>() : QList<QFrame*>();
    for (QFrame* separator : separators) {
        if (separator && separator->frameShape() == QFrame::VLine) {
            separator->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
        }
    }
    // Also update the cached trailing separator if it's currently detached
    if (m_remoteInfoSep2 && (!m_remoteClientInfoContainer || m_remoteInfoSep2->parent() != m_remoteClientInfoContainer)) {
        m_remoteInfoSep2->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    }
    // And the leading separator if detached
    if (m_remoteInfoSep1 && (!m_remoteClientInfoContainer || m_remoteInfoSep1->parent() != m_remoteClientInfoContainer)) {
        m_remoteInfoSep1->setStyleSheet(QString("QFrame { color: %1; }").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)));
    }
    
    // Update separators in local client info
    QList<QFrame*> localSeparators = m_localClientInfoContainer ? m_localClientInfoContainer->findChildren<QFrame*>() : QList<QFrame*>();
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

    // Layout: [title][back][stretch][local-client-info][connect][settings]
    m_connectionLayout->addWidget(m_backButton);
    m_connectionLayout->addStretch();
    m_connectionLayout->addWidget(m_localClientInfoContainer);
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
    
    // Create client list page
    createClientListPage();
    // Show placeholder immediately (before any connection) so page isn't empty during CONNECTING state
    ensureClientListPlaceholder();
    
    // Create screen view page  
    createScreenViewPage();
    
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
        w.screenViewPage = m_screenViewWidget;
        w.backButton = m_backButton;
        w.canvasStack = m_canvasStack;
        w.loadingSpinner = m_loadingSpinner;
        w.spinnerOpacity = m_spinnerOpacity;
        w.spinnerFade = m_spinnerFade;
        w.canvasOpacity = m_canvasOpacity;
        w.canvasFade = m_canvasFade;
        w.inlineSpinner = m_inlineSpinner;
        w.canvasContentEverLoaded = &m_canvasContentEverLoaded;
        w.volumeOpacity = m_volumeOpacity;
        w.volumeFade = m_volumeFade;
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
                if (m_stackedWidget->currentWidget() != m_screenViewWidget) return;
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

void MainWindow::createClientListPage() {
    m_clientListPage = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_clientListPage);
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);
    
    // Client list widget - simple and flexible
    m_clientListWidget = new QListWidget();
    m_clientListWidget->setStyleSheet(
        QString("QListWidget { "
        "   border: 1px solid %1; "
        "   border-radius: 5px; "
        "   padding: 0px; "
        "   background-color: %2; "
        "   outline: none; "
        "}"
        "QListWidget::item { "
        "   padding: 10px; "
        "}"
        "QListWidget::item:hover { "
        "   background-color: rgba(74, 144, 226, 28); "
        "}"
        "QListWidget::item:selected { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:active { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:hover { "
        "   background-color: " + AppColors::colorToCss(AppColors::gHoverHighlight) + "; "
        "   color: palette(text); "
        "}").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource))
               .arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
    );
    
    connect(m_clientListWidget, &QListWidget::itemClicked, this, &MainWindow::onClientItemClicked);
    m_clientListWidget->setFocusPolicy(Qt::NoFocus);
    m_clientListWidget->installEventFilter(this);
    m_clientListWidget->setMouseTracking(true);
    m_clientListWidget->setItemDelegate(new ClientListSeparatorDelegate(m_clientListWidget));
    
    // Simple size policy: expand in both directions, let Qt handle sizing naturally
    m_clientListWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_clientListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_clientListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    
    layout->addWidget(m_clientListWidget);
    
    m_stackedWidget->addWidget(m_clientListPage);
}

void MainWindow::createScreenViewPage() {
    // Screen view page
    m_screenViewWidget = new QWidget();
    m_screenViewLayout = new QVBoxLayout(m_screenViewWidget);
    // Gap between canvas sections
    m_screenViewLayout->setSpacing(gInnerContentGap);
    m_screenViewLayout->setContentsMargins(0, 0, 0, 0);
    
    // Create labels for remote client info (will be used in top bar container)
    m_clientNameLabel = new QLabel();
    applyTitleText(m_clientNameLabel);
    m_clientNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Remote connection status (to the right of hostname)
    m_remoteConnectionStatusLabel = new QLabel("DISCONNECTED");
    // Initial styling - will be updated by setRemoteConnectionStatus()
    m_remoteConnectionStatusLabel->setStyleSheet(
        QString("QLabel { "
        "    color: #E53935; "
        "    background-color: rgba(244,67,54,0.15); "
        "    border: none; "
        "    border-radius: 0px; "
        "    padding: 0px %2px; "
        "    font-size: %1px; "
        "    font-weight: bold; "
        "}").arg(gDynamicBoxFontPx).arg(gRemoteClientContainerPadding)
    );
    m_remoteConnectionStatusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_remoteConnectionStatusLabel->setFixedWidth(120); // Fixed width for consistency
    m_remoteConnectionStatusLabel->setAlignment(Qt::AlignCenter); // Center the text

    m_volumeIndicator = new QLabel("🔈 --");
    m_volumeIndicator->setStyleSheet("QLabel { font-size: 16px; color: palette(text); font-weight: bold; }");
    m_volumeIndicator->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volumeIndicator->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Initialize remote client info container in top bar permanently
    initializeRemoteClientInfoInTopBar();
    
    // Canvas container holds spinner and canvas with a stacked layout
    m_canvasContainer = new QWidget();
    m_canvasContainer->setObjectName("CanvasContainer");
    // Remove minimum height to allow flexible window resizing
    // Ensure stylesheet background/border is actually painted
    m_canvasContainer->setAttribute(Qt::WA_StyledBackground, true);
    // Match the dark background used by the client list container via palette(base)
    // Canvas container previously had a bordered panel look; remove to emulate design-tool feel
    m_canvasContainer->setStyleSheet(
        QString("QWidget#CanvasContainer { "
        "   background-color: %2; "
        "   border: 1px solid %1; "
        "   border-radius: 5px; "
        "}").arg(AppColors::colorSourceToCss(AppColors::gAppBorderColorSource)).arg(AppColors::colorSourceToCss(AppColors::gInteractionBackgroundColorSource))
    );
    QVBoxLayout* containerLayout = new QVBoxLayout(m_canvasContainer);
    // Remove inner padding so content goes right to the border edge
    containerLayout->setContentsMargins(0,0,0,0);
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
    // Spinner page
    m_loadingSpinner = new SpinnerWidget();
    // Initial appearance (easy to tweak):
    m_loadingSpinner->setRadius(22);        // circle radius in px
    m_loadingSpinner->setLineWidth(6);      // line width in px
    m_loadingSpinner->setColor(AppColors::gBrandBlue); // brand blue
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

    m_canvasHostStack = new QStackedWidget();
    m_canvasHostStack->setObjectName("CanvasHostStack");
    m_canvasHostStack->setStyleSheet(
        "QStackedWidget { "
        "   background-color: transparent; "
        "   border: none; "
        "}"
    );
    canvasLayout->addWidget(m_canvasHostStack);

    QWidget* emptyCanvasPlaceholder = new QWidget();
    m_canvasHostStack->addWidget(emptyCanvasPlaceholder);
    m_canvasHostStack->setCurrentWidget(emptyCanvasPlaceholder);
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

    // Ensure focus routing is configured even before a canvas is attached
    m_screenViewWidget->installEventFilter(this);
    
    // Connect upload button in media list overlay to upload functionality

    if (m_screenCanvas) {
        if (QPushButton* overlayUploadBtn = m_screenCanvas->getUploadButton()) {
            m_uploadButton = overlayUploadBtn; // Use the overlay button as our upload button
            m_uploadButtonInOverlay = true; // Flag that this button uses overlay styling
            m_uploadButtonDefaultFont = m_uploadButton->font();
            connect(m_uploadButton, &QPushButton::clicked, this, &MainWindow::onUploadButtonClicked);
            // Apply initial style state machine
            QTimer::singleShot(0, [this]() {
                emit m_uploadManager->uiStateChanged();
            });
        }
    }

    // When a new media item is added to the canvas, re-evaluate the upload button state immediately
    if (m_screenCanvas) {
        connect(m_screenCanvas, &ScreenCanvas::mediaItemAdded, this, [this](ResizableMediaBase*){
            if (!m_uploadButton) return;
            // Post to the event loop so the scene and overlay have finished updating before we re-evaluate
            QTimer::singleShot(0, [this]() {
                if (m_uploadManager) emit m_uploadManager->uiStateChanged();
            });
        });
    }
    
    // Upload button moved to media list overlay - no action bar needed
    // Ensure canvas container expands to fill available space
    m_screenViewLayout->setStretch(0, 1); // container expands

    // Volume label opacity effect & animation
    m_volumeFade = new QPropertyAnimation(m_volumeOpacity, "opacity", this);
    m_volumeFade->setDuration(m_fadeDurationMs);
    m_volumeFade->setStartValue(0.0);
    m_volumeFade->setEndValue(1.0);

    // Loader delay timer removed (handled inside ScreenNavigationManager)
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_screenViewWidget);

    // Ensure watch session stops if application quits via menu/shortcut
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        if (!m_activeSessionIdentity.isEmpty()) {
            if (CanvasSession* activeSession = findCanvasSession(m_activeSessionIdentity)) {
                unloadUploadsForSession(*activeSession, true);
            }
        }
        if (m_watchManager) m_watchManager->unwatchIfAny();
    });
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
        qDebug() << "About dialog suppressed (no popup mode).";
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
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    
    // Update responsive layout based on window width
    if (m_responsiveLayoutManager) {
        m_responsiveLayoutManager->updateResponsiveLayout();
    }
    
    // If we're currently showing the screen view and have a canvas with content,
    // recenter the view to maintain good visibility only before first reveal or when no screens are present
    if (m_stackedWidget && m_stackedWidget->currentWidget() == m_screenViewWidget && 
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

void MainWindow::onConnected() {
    setUIEnabled(true);
    setLocalNetworkStatus("Connected"); // immediate visual update
    // Reset reconnection state on successful connection
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
    
    // Sync this client's info with the server
    syncRegistration();
    
    // If we were on a client's canvas page when the connection dropped,
    // re-request that client's screens and re-establish watch so content returns
    // when data is received.
    if (m_navigationManager && m_navigationManager->isOnScreenView()) {
        // Ensure the canvas will reveal again on fresh screens
        m_canvasRevealedForCurrentClient = false;
        // Remove volume indicator until remote is ready again
        removeVolumeIndicatorFromLayout();
        const QString selId = m_selectedClient.getId();
        if (!selId.isEmpty() && m_webSocketClient && m_webSocketClient->isConnected()) {
            // Indicate we're attempting to reach the remote again
            setRemoteConnectionStatus("CONNECTING...");
            addRemoteStatusToLayout();
            m_webSocketClient->requestScreens(selId);
            if (m_watchManager) {
                // Ensure a clean state after reconnect, then start watching the selected client again
                m_watchManager->unwatchIfAny();
                m_watchManager->toggleWatch(selId);
            }
        }
    }

    
    // Show tray notification
    TOAST_SUCCESS("Connected to server", 2000);
}

void MainWindow::onDisconnected() {
    setUIEnabled(false);
    setLocalNetworkStatus("Disconnected");
    // If user is currently on a client's canvas page, immediately switch the canvas
    // area to a loading state and clear any displayed content/overlays.
    if (m_navigationManager && m_navigationManager->isOnScreenView()) {
    // Enter loading state but preserve the current canvas content/viewport; the navigation
    // manager now hides content instead of clearing it. Mark that we should not recenter on reconnect.
    m_preserveViewportOnReconnect = true;
        // Remove volume indicator FIRST (before showing spinner) to avoid layout shift
        removeVolumeIndicatorFromLayout();
        // Our local network just dropped; show error state explicitly
        addRemoteStatusToLayout();
        setRemoteConnectionStatus("ERROR");
        // Now show the inline spinner after volume is removed
    m_navigationManager->enterLoadingStateImmediate();
    }
    
    // Inform upload manager of connection loss to cancel any ongoing upload/finalizing state
    bool hadUploadInProgress = false;
    if (m_uploadManager) {
        hadUploadInProgress = m_uploadManager->isUploading() || m_uploadManager->isFinalizing();
        m_uploadManager->onConnectionLost();
    }
    
    if (hadUploadInProgress) {
        TOAST_ERROR("Upload interrupted - connection lost", 3000);
    } else {
        TOAST_ERROR("Disconnected from server", 3000);
    }

    // Start smart reconnection if client is enabled and not manually disconnected
    if (!m_userDisconnected) {
        scheduleReconnect();
    }
    
    for (auto it = m_canvasSessions.begin(); it != m_canvasSessions.end(); ++it) {
        CanvasSession& session = it.value();
        if (session.canvas && session.canvas->scene()) {
            const QList<QGraphicsItem*> allItems = session.canvas->scene()->items();
            for (QGraphicsItem* item : allItems) {
                if (auto* media = dynamic_cast<ResizableMediaBase*>(item)) {
                    if (media->uploadState() == ResizableMediaBase::UploadState::Uploading) {
                        media->setUploadNotUploaded();
                    }
                }
            }
        }
        session.upload.remoteFilesPresent = false;
        clearUploadTracking(session);
    }
    m_uploadSessionByUploadId.clear();
    m_activeUploadSessionIdentity.clear();

    // Stop watching if any
    if (m_watchManager) m_watchManager->unwatchIfAny();
    
    // Clear client list
    m_availableClients.clear();
    updateClientList(m_availableClients);
    
    
    // Show tray notification
    // Notification supprimée (mode silencieux)
}

// startWatchingSelectedClient/stopWatchingCurrentClient removed (handled by WatchManager)

void MainWindow::onConnectionError(const QString& error) {
    qWarning() << "Failed to connect to server:" << error << "(silent mode, aucune popup)";
    setUIEnabled(false);
    setLocalNetworkStatus("Error");
    TOAST_ERROR(QString("Connection failed: %1").arg(error), 4000);
}

void MainWindow::onClientListReceived(const QList<ClientInfo>& clients) {
    qDebug() << "Received client list with" << clients.size() << "clients";
    
    // Update remote scene target ID if the target machine reconnected with a new ID
    if (m_screenCanvas) {
        m_screenCanvas->updateRemoteSceneTargetFromClientList(clients);
    }
    
    QList<ClientInfo> displayList = buildDisplayClientList(clients);

    int previousDisplayCount = m_availableClients.size();
    int previousConnectedCount = m_lastConnectedClientCount;
    m_lastConnectedClientCount = clients.size();

    m_availableClients = displayList;
    updateClientList(m_availableClients);

    if (!m_activeSessionIdentity.isEmpty()) {
        if (CanvasSession* activeSession = findCanvasSession(m_activeSessionIdentity)) {
            m_selectedClient = activeSession->lastClientInfo;
            if (activeSession->canvas && !activeSession->clientId.isEmpty()) {
                activeSession->canvas->setRemoteSceneTarget(activeSession->clientId, activeSession->lastClientInfo.getMachineName());
            }
        }
    }

    // Show notification if new connected clients appeared
    if (clients.size() > previousConnectedCount && previousConnectedCount >= 0) {
        int newClients = clients.size() - previousConnectedCount;
        if (newClients > 0) {
            QString message = QString("%1 new client%2 available for sharing")
                .arg(newClients)
                .arg(newClients == 1 ? "" : "s");
            qDebug() << "New clients available:" << message;
        }
    }

    // Update remote status and canvas behavior if we're on screen view:
    // Handle remote reconnect where the server assigns a NEW clientId.
    // Strategy:
    // 1) If selected client's id is present -> mark CONNECTED. If we're on loader, request screens + watch.
    // 2) Else, try to match by machineName (and platform). If found -> treat as same device, switch selection to new id,
    //    reset reveal flag, show screen view for it, request screens + watch.
    // 3) Else -> mark DISCONNECTED and keep loader.
    if (m_navigationManager && m_navigationManager->isOnScreenView() && !m_activeSessionIdentity.isEmpty()) {
        CanvasSession* activeSession = findCanvasSession(m_activeSessionIdentity);
        if (activeSession) {
            const QString selId = activeSession->clientId;
            const QString selName = activeSession->lastClientInfo.getMachineName();
            const QString selPlatform = activeSession->lastClientInfo.getPlatform();

            const auto findById = [&clients](const QString& id) -> std::optional<ClientInfo> {
                for (const auto& c : clients) { if (c.getId() == id) return c; }
                return std::nullopt;
            };
            const auto findByNamePlatform = [&clients](const QString& name, const QString& platform) -> std::optional<ClientInfo> {
                for (const auto& c : clients) {
                    if (c.getMachineName().compare(name, Qt::CaseInsensitive) == 0 && c.getPlatform() == platform) return c;
                }
                return std::nullopt;
            };

            std::optional<ClientInfo> byId;
            if (!selId.isEmpty()) {
                byId = findById(selId);
            }

            if (byId.has_value()) {
                activeSession->clientId = byId->getId();
                activeSession->lastClientInfo = byId.value();
                activeSession->lastClientInfo.setIdentityKey(m_activeSessionIdentity);
                activeSession->remoteContentClearedOnDisconnect = false;
                m_selectedClient = activeSession->lastClientInfo;
                if (activeSession->canvas && !activeSession->clientId.isEmpty()) {
                    activeSession->canvas->setRemoteSceneTarget(activeSession->clientId, selName);
                }
                if (m_uploadManager) m_uploadManager->setTargetClientId(activeSession->clientId);
                const bool isActiveSelection = (activeSession->identityKey == m_activeSessionIdentity);
                if (isActiveSelection) {
                    if (m_activeRemoteClientId != activeSession->clientId) {
                        m_activeRemoteClientId = activeSession->clientId;
                        m_remoteClientConnected = false;
                    }
                    if (!m_remoteClientConnected) {
                        addRemoteStatusToLayout();
                        setRemoteConnectionStatus("CONNECTING...", /*propagateLoss*/ false);
                        if (m_inlineSpinner && !m_inlineSpinner->isSpinning()) {
                            m_inlineSpinner->show();
                            m_inlineSpinner->start();
                        }
                        if (m_webSocketClient && m_webSocketClient->isConnected()) {
                            m_canvasRevealedForCurrentClient = false;
                            m_webSocketClient->requestScreens(activeSession->clientId);
                            if (m_watchManager) {
                                if (m_watchManager->watchedClientId() != activeSession->clientId) {
                                    m_watchManager->unwatchIfAny();
                                    m_watchManager->toggleWatch(activeSession->clientId);
                                }
                            }
                        }
                    }
                }
            } else {
                auto byName = findByNamePlatform(selName, selPlatform);
                if (byName.has_value()) {
                    activeSession->clientId = byName->getId();
                    activeSession->lastClientInfo = byName.value();
                    activeSession->lastClientInfo.setIdentityKey(m_activeSessionIdentity);
                    activeSession->remoteContentClearedOnDisconnect = false;
                    m_selectedClient = activeSession->lastClientInfo;
                    if (activeSession->canvas && !activeSession->clientId.isEmpty()) {
                        activeSession->canvas->setRemoteSceneTarget(activeSession->clientId, selName);
                    }
                    if (m_uploadManager) m_uploadManager->setTargetClientId(activeSession->clientId);
                    if (m_navigationManager) {
                        m_navigationManager->refreshActiveClientPreservingCanvas(activeSession->lastClientInfo);
                    }
                    updateClientNameDisplay(activeSession->lastClientInfo);
                    const bool isActiveSelection = (activeSession->identityKey == m_activeSessionIdentity);
                    if (isActiveSelection) {
                        if (m_activeRemoteClientId != activeSession->clientId) {
                            m_activeRemoteClientId = activeSession->clientId;
                            m_remoteClientConnected = false;
                        }
                        if (!m_remoteClientConnected) {
                            setRemoteConnectionStatus("CONNECTING...");
                            addRemoteStatusToLayout();
                            if (m_inlineSpinner && !m_inlineSpinner->isSpinning()) {
                                m_inlineSpinner->show();
                                m_inlineSpinner->start();
                            }
                            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                                m_canvasRevealedForCurrentClient = false;
                                m_webSocketClient->requestScreens(activeSession->clientId);
                                if (m_watchManager) {
                                    if (m_watchManager->watchedClientId() != activeSession->clientId) {
                                        m_watchManager->unwatchIfAny();
                                        m_watchManager->toggleWatch(activeSession->clientId);
                                    }
                                }
                            }
                        }
                    }
                } else {
                    if (!activeSession->remoteContentClearedOnDisconnect) {
                        unloadUploadsForSession(*activeSession, true);
                    }
                    addRemoteStatusToLayout();
                    setRemoteConnectionStatus("DISCONNECTED");
                    m_preserveViewportOnReconnect = true;
                    if (m_inlineSpinner) {
                        m_inlineSpinner->show();
                        m_inlineSpinner->start();
                    }
                    if (m_uploadManager) {
                        m_uploadManager->setTargetClientId(QString());
                    }
                    if (m_watchManager) {
                        m_watchManager->unwatchIfAny();
                    }
                    if (activeSession->lastClientInfo.getVolumePercent() >= 0) {
                        m_selectedClient = activeSession->lastClientInfo;
                        addVolumeIndicatorToLayout();
                        updateVolumeIndicator();
                    } else {
                        removeVolumeIndicatorFromLayout();
                    }
                }
            }
        }
    }
}

void MainWindow::onRegistrationConfirmed(const ClientInfo& clientInfo) {
    m_thisClient = clientInfo;
    qDebug() << "Registration confirmed for:" << clientInfo.getMachineName();
}

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
    // Build per-screen uiZones (taskbar/menu/dock)
    if (!screens.isEmpty()) {
#if defined(Q_OS_WIN)
        // Build a list of physical monitors (rcMonitor/rcWork) to align with ScreenInfo (physical px)
        std::vector<WinMonRect> mons; mons.reserve(8);
        EnumDisplayMonitors(nullptr, nullptr, MouffetteEnumMonProc, reinterpret_cast<LPARAM>(&mons));
        auto findMatchingMon = [&](const ScreenInfo& s) -> const WinMonRect* {
            for (const auto &m : mons) {
                const int mw = m.rc.right - m.rc.left;
                const int mh = m.rc.bottom - m.rc.top;
                if (m.rc.left == s.x && m.rc.top == s.y && mw == s.width && mh == s.height) return &m;
            }
            return nullptr;
        };
        for (auto &screen : screens) {
            const WinMonRect* mp = findMatchingMon(screen);
            if (!mp) continue;
            const auto &m = *mp;
            const int screenW = m.rc.right - m.rc.left;
            const int screenH = m.rc.bottom - m.rc.top;
            const int workW = m.rcWork.right - m.rcWork.left;
            const int workH = m.rcWork.bottom - m.rcWork.top;
            // Compute taskbar thickness and side by comparing rcMonitor and rcWork
            if (workH < screenH) {
                const int h = screenH - workH; if (h > 0) {
                    if (m.rcWork.top > m.rc.top) screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("taskbar"), 0, 0, screenW, h});
                    else screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("taskbar"), 0, screenH - h, screenW, h});
                }
            } else if (workW < screenW) {
                const int w = screenW - workW; if (w > 0) {
                    if (m.rcWork.left > m.rc.left) screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("taskbar"), 0, 0, w, screenH});
                    else screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("taskbar"), screenW - w, 0, w, screenH});
                }
            }
        }
#elif defined(Q_OS_MACOS)
        QList<QScreen*> qScreens = QGuiApplication::screens();
        for (auto &screen : screens) {
            if (screen.id < 0 || screen.id >= qScreens.size()) continue;
            QScreen* qs = qScreens[screen.id]; if (!qs) continue;
            QRect geom = qs->geometry();
            QRect avail = qs->availableGeometry();
            // Menu bar
            if (avail.y() > geom.y()) {
                int h = avail.y() - geom.y(); if (h>0)
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("menu_bar"), 0, 0, geom.width(), h});
            }
            // Dock: one differing edge
            if (avail.bottom() < geom.bottom()) { // bottom dock
                int h = geom.bottom() - avail.bottom(); if (h>0)
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), 0, geom.height()-h, geom.width(), h});
            } else if (avail.x() > geom.x()) { // left dock
                int w = avail.x() - geom.x(); if (w>0)
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), 0, 0, w, geom.height()});
            } else if (avail.right() < geom.right()) { // right dock
                int w = geom.right() - avail.right(); if (w>0)
                    screen.uiZones.append(ScreenInfo::UIZone{QStringLiteral("dock"), geom.width()-w, 0, w, geom.height()});
            }
        }
#endif
        // uiZones prepared during syncRegistration
    }
    
    qDebug() << "Sync registration:" << machineName << "on" << platform << "with" << screens.size() << "screens";
    
    m_webSocketClient->registerClient(machineName, platform, screens, volumePercent);
}

void MainWindow::onScreensInfoReceived(const ClientInfo& clientInfo) {
    QString identity = !clientInfo.identityKey().isEmpty()
        ? clientInfo.identityKey()
        : makeIdentityKey(clientInfo.getMachineName(), clientInfo.getPlatform());

    CanvasSession* session = findCanvasSession(identity);
    if (!session && !clientInfo.getId().isEmpty()) {
        session = findCanvasSessionByClientId(clientInfo.getId());
    }

    if (!session) {
        session = &ensureCanvasSession(clientInfo);
    } else {
        session->clientId = clientInfo.getId();
        session->lastClientInfo = clientInfo;
        session->lastClientInfo.setIdentityKey(identity);
        session->lastClientInfo.setFromMemory(true);
        session->lastClientInfo.setOnline(true);
    }

    if (!session->canvas) {
        session->canvas = new ScreenCanvas(m_canvasHostStack);
        session->canvas->setWebSocketClient(m_webSocketClient);
        session->connectionsInitialized = false;
        if (m_canvasHostStack && m_canvasHostStack->indexOf(session->canvas) == -1) {
            m_canvasHostStack->addWidget(session->canvas);
        }
        configureCanvasSession(*session);
    }

    const QList<ScreenInfo> screens = clientInfo.getScreens();
    const bool hasScreens = !screens.isEmpty();

    if (session->canvas) {
        if (!session->clientId.isEmpty()) {
            session->canvas->setRemoteSceneTarget(session->clientId, session->lastClientInfo.getMachineName());
        }
        session->canvas->setScreens(screens);
    }

    const bool isActiveSession = (session->identityKey == m_activeSessionIdentity);
    if (isActiveSession) {
        m_screenCanvas = session->canvas;
        m_selectedClient = session->lastClientInfo;
    }

    session->lastClientInfo.setScreens(screens);

    if (isActiveSession && session->canvas) {
        if (!m_canvasRevealedForCurrentClient && hasScreens) {
            if (m_navigationManager) {
                m_navigationManager->revealCanvas();
            } else if (m_canvasStack) {
                m_canvasStack->setCurrentIndex(1);
            }

            session->canvas->requestDeferredInitialRecenter(53);
            if (!m_preserveViewportOnReconnect) {
                session->canvas->recenterWithMargin(53);
            }
            session->canvas->setFocus(Qt::OtherFocusReason);

            m_preserveViewportOnReconnect = false;
            m_canvasRevealedForCurrentClient = true;
            m_canvasContentEverLoaded = true;
        }

        if (m_inlineSpinner && m_inlineSpinner->isSpinning()) {
            m_inlineSpinner->stop();
            m_inlineSpinner->hide();
        }

        if (hasScreens && m_volumeIndicator) {
            addVolumeIndicatorToLayout();
            updateVolumeIndicator();
        }

        addRemoteStatusToLayout();
        setRemoteConnectionStatus(session->lastClientInfo.isOnline() ? "CONNECTED" : "DISCONNECTED");

        updateClientNameDisplay(session->lastClientInfo);
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
    // Target-side: server asked us to send fresh state now (screens + volume)
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) return;
    QList<ScreenInfo> screens = getLocalScreenInfo();
    int volumePercent = getSystemVolumePercent();
    // Build uiZones immediately for snapshot
#if defined(Q_OS_WIN)
    {
        std::vector<WinMonRect> mons; mons.reserve(8);
        EnumDisplayMonitors(nullptr, nullptr, MouffetteEnumMonProc, reinterpret_cast<LPARAM>(&mons));
        auto findMatchingMon = [&](const ScreenInfo& s) -> const WinMonRect* {
            for (const auto &m : mons) {
                const int mw = m.rc.right - m.rc.left;
                const int mh = m.rc.bottom - m.rc.top;
                if (m.rc.left == s.x && m.rc.top == s.y && mw == s.width && mh == s.height) return &m;
            }
            return nullptr;
        };
        for (auto &screen : screens) {
            const WinMonRect* mp = findMatchingMon(screen);
            if (!mp) continue;
            const auto &m = *mp;
            const int screenW = m.rc.right - m.rc.left;
            const int screenH = m.rc.bottom - m.rc.top;
            const int workW = m.rcWork.right - m.rcWork.left;
            const int workH = m.rcWork.bottom - m.rcWork.top;
            if (workH < screenH) {
                const int h = screenH - workH; if (h > 0) {
                    if (m.rcWork.top > m.rc.top) screen.uiZones.append(ScreenInfo::UIZone{"taskbar", 0, 0, screenW, h});
                    else screen.uiZones.append(ScreenInfo::UIZone{"taskbar", 0, screenH - h, screenW, h});
                }
            } else if (workW < screenW) {
                const int w = screenW - workW; if (w > 0) {
                    if (m.rcWork.left > m.rc.left) screen.uiZones.append(ScreenInfo::UIZone{"taskbar", 0, 0, w, screenH});
                    else screen.uiZones.append(ScreenInfo::UIZone{"taskbar", screenW - w, 0, w, screenH});
                }
            }
        }
    }
#elif defined(Q_OS_MACOS)
    {
        QList<QScreen*> qScreens = QGuiApplication::screens();
        for (auto &screen : screens) {
            if (screen.id < 0 || screen.id >= qScreens.size()) continue; QScreen* qs = qScreens[screen.id]; if (!qs) continue;
            QRect geom = qs->geometry(); QRect avail = qs->availableGeometry();
            if (avail.y() > geom.y()) { int h = avail.y()-geom.y(); if (h>0) screen.uiZones.append(ScreenInfo::UIZone{"menu_bar", 0, 0, geom.width(), h}); }
            if (avail.bottom() < geom.bottom()) { int h = geom.bottom()-avail.bottom(); if (h>0) screen.uiZones.append(ScreenInfo::UIZone{"dock", 0, geom.height()-h, geom.width(), h}); }
            else if (avail.x() > geom.x()) { int w = avail.x()-geom.x(); if (w>0) screen.uiZones.append(ScreenInfo::UIZone{"dock", 0, 0, w, geom.height()}); }
            else if (avail.right() < geom.right()) { int w = geom.right()-avail.right(); if (w>0) screen.uiZones.append(ScreenInfo::UIZone{"dock", geom.width()-w, 0, w, geom.height()}); }
        }
    }
#endif
    m_webSocketClient->sendStateSnapshot(screens, volumePercent);
}

QList<ScreenInfo> MainWindow::getLocalScreenInfo() {
    QList<ScreenInfo> screens;
    

#ifdef Q_OS_WIN
    // Use WinAPI to enumerate monitors in PHYSICAL pixels with correct origins (no logical gaps)
    std::vector<WinMonRect> mons; mons.reserve(8);
    EnumDisplayMonitors(nullptr, nullptr, MouffetteEnumMonProc, reinterpret_cast<LPARAM>(&mons));
    if (mons.empty()) {
        QList<QScreen*> screenList = QGuiApplication::screens();
        for (int i = 0; i < screenList.size(); ++i) {
            QScreen* s = screenList[i];
            const QRect g = s->geometry();
            screens.append(ScreenInfo(i, g.width(), g.height(), g.x(), g.y(), s == QGuiApplication::primaryScreen()));
        }
    } else {
        for (size_t i = 0; i < mons.size(); ++i) {
            const auto& m = mons[i];
            const int px = m.rc.left; const int py = m.rc.top;
            const int pw = m.rc.right - m.rc.left; const int ph = m.rc.bottom - m.rc.top;
            // For ScreenInfo we keep absolute coordinates so that cursor mapping (also physical) matches exactly.
            screens.append(ScreenInfo(static_cast<int>(i), pw, ph, px, py, m.primary));
        }
    }
#else
    QList<QScreen*> screenList = QGuiApplication::screens();
    for (int i = 0; i < screenList.size(); ++i) {
        QScreen* screen = screenList[i];
        QRect geometry = screen->geometry();
        bool isPrimary = (screen == QGuiApplication::primaryScreen());
        screens.append(ScreenInfo(i, geometry.width(), geometry.height(), geometry.x(), geometry.y(), isPrimary));
    }
#endif

    return screens;
}


void MainWindow::connectToServer() {
    if (!m_webSocketClient) return;
    const QString url = m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig;
    qDebug() << "Connecting to server:" << url;
    m_webSocketClient->connectToServer(url);
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
        // Simple "no clients" message
        QListWidgetItem* item = new QListWidgetItem("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
        item->setFlags(Qt::NoItemFlags);
        item->setTextAlignment(Qt::AlignCenter);
        QFont font = item->font();
        font.setItalic(true);
        font.setPointSize(16);
        item->setFont(font);
        item->setForeground(AppColors::gTextMuted);
        m_clientListWidget->addItem(item);
    } else {
        // Add client items
        for (const ClientInfo& client : clients) {
            QListWidgetItem* item = new QListWidgetItem(client.getDisplayText());
            m_clientListWidget->addItem(item);
        }
    }
}

void MainWindow::setUIEnabled(bool enabled) {
    // Client list depends on connection
    m_clientListWidget->setEnabled(enabled);
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

bool MainWindow::hasUnuploadedFilesForTarget(const QString& targetClientId) const {
    ScreenCanvas* canvas = canvasForClientId(targetClientId);
    if (!canvas || !canvas->scene()) return false;
    const QList<QGraphicsItem*> allItems = canvas->scene()->items();
    for (QGraphicsItem* it : allItems) {
        if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
            const QString mediaId = media->mediaId();
            if (mediaId.isEmpty()) continue;
            if (!FileManager::instance().isMediaUploadedToClient(mediaId, targetClientId)) return true;
        }
    }
    return false;
}


