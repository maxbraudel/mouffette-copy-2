#include "ScreenCanvas.h"
#include "MediaItems.h"
#include "OverlayPanels.h"
#include "Theme.h"
#include "AppColors.h"
#include <algorithm>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QPushButton>
#include <QHBoxLayout>
#include <QTimer>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QScrollArea>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QNativeGestureEvent>
#include <QFileInfo>
#include <QVariantAnimation>
#include <QVideoSink>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QGuiApplication>
#include <QScreen>
#include <QStyleHints>
#include <QCursor>
#include <QGraphicsPixmapItem>
#include <QCoreApplication>
#include <QtMath>
#include <QRandomGenerator>
#include <QJsonObject>
#include <QPainter>
#include <QBuffer>
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QImageReader>
#include "Theme.h" // for overlay colors
#include <QSizePolicy>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <cmath>

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
        
        // Use overlay corner radius instead of dynamic box radius
        const int radius = qMax(0, qMin(gOverlayCornerRadiusPx, qMin(currentSize.width(), currentSize.height()) / 2));
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

// Configuration constants
static const int gMediaListItemSpacing = 3; // Spacing between media list items (name, status, details)
static const int gScrollbarAutoHideDelayMs = 500; // Time in milliseconds before scrollbar auto-hides after scroll inactivity

// Helper: relayout overlays for all media items so absolute panels (settings) stay pinned
namespace {
static void relayoutAllMediaOverlays(QGraphicsScene* scene) {
    if (!scene) return;
    const QList<QGraphicsItem*> items = scene->items();
    for (QGraphicsItem* it : items) {
        if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) {
            base->updateOverlayLayout();
        }
    }
}
}

ScreenCanvas::~ScreenCanvas() {
    // Prevent any further UI refresh callbacks after this view is destroyed
    ResizableMediaBase::setUploadChangedNotifier(nullptr);
    if (m_scene) {
        disconnect(m_scene, nullptr, this, nullptr);
    }
    if (m_infoBorderRect && scene()) {
        scene()->removeItem(m_infoBorderRect);
        delete m_infoBorderRect;
        m_infoBorderRect = nullptr;
    }
    if (m_infoWidget) {
        m_infoWidget->deleteLater();
        m_infoWidget = nullptr;
    }
}

void ScreenCanvas::maybeRefreshInfoOverlayOnSceneChanged() {
    if (!m_scene) return;
    auto recomputeAndRefreshIfNeeded = [this]() {
        if (!m_scene) return;
        int count = 0;
        const auto itemsNow = m_scene->items();
        for (QGraphicsItem* it : itemsNow) {
            if (dynamic_cast<ResizableMediaBase*>(it)) ++count;
        }
        if (m_lastMediaItemCount == -1) { m_lastMediaItemCount = count; return; }
        if (count != m_lastMediaItemCount) {
            m_lastMediaItemCount = count;
            refreshInfoOverlay();
        }
    };
    // Do an immediate check (covers most cases)
    recomputeAndRefreshIfNeeded();
    // And schedule a microtask; some remove operations complete after the current event
    QTimer::singleShot(0, [recomputeAndRefreshIfNeeded]() { recomputeAndRefreshIfNeeded(); });
}

void ScreenCanvas::initInfoOverlay() {
    if (!viewport()) return;
    if (!m_infoWidget) {
        // Create a clipped container to properly handle border-radius clipping
        m_infoWidget = new ClippedContainer(viewport());
        m_infoWidget->setAttribute(Qt::WA_StyledBackground, true);
        m_infoWidget->setAutoFillBackground(true);
        // Ensure overlay blocks mouse events to canvas behind it
        m_infoWidget->setAttribute(Qt::WA_NoMousePropagation, true);
        // Build stylesheet - transparent background like MediaSettingsPanel (background handled by graphics rect)
        const QString bg = QString("background-color: transparent; border-radius: %1px; color: white; font-size: 16px;")
            .arg(gOverlayCornerRadiusPx);
        m_infoWidget->setStyleSheet(bg);
        // Baseline minimum width to avoid tiny panel before content exists
        m_infoWidget->setMinimumWidth(200);
        // Vertically, the overlay must never stretch; we'll size it explicitly
        m_infoWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        
        m_infoLayout = new QVBoxLayout(m_infoWidget);
        m_infoLayout->setContentsMargins(0, 0, 0, 0); // No margins on main container
        m_infoLayout->setSpacing(0);
        // Let us control the container height explicitly (no automatic min size from layout)
        m_infoLayout->setSizeConstraint(QLayout::SetNoConstraint);
        
        // Create content container for media list items with margins, wrapped in a scroll area
        m_contentScroll = new QScrollArea(m_infoWidget);
        m_contentScroll->setFrameShape(QFrame::NoFrame);
        // Hide native scrollbars; we'll draw a floating overlay scrollbar instead
        m_contentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_contentScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_contentScroll->setWidgetResizable(true);
        // Ensure horizontal scrollbar is completely disabled
        if (QScrollBar* hBar = m_contentScroll->horizontalScrollBar()) {
            hBar->setEnabled(false);
            hBar->hide();
        }
        // Ensure viewport is fully transparent (no gray behind the track)
        if (m_contentScroll->viewport()) {
            m_contentScroll->viewport()->setAutoFillBackground(false);
        }
        // Hide the native vertical scrollbar widget but keep it functional
        if (QScrollBar* nativeV = m_contentScroll->verticalScrollBar()) {
            nativeV->hide();
        }
        m_contentScroll->setStyleSheet(
            // Transparent backgrounds for area and viewport
            "QAbstractScrollArea { background: transparent; border: none; }"
            " QAbstractScrollArea > QWidget#qt_scrollarea_viewport { background: transparent; }"
            " QAbstractScrollArea::corner { background: transparent; }"
            // Ensure any native vertical scrollbar inside the scroll area is 0 width and invisible
            " QScrollArea QScrollBar:vertical { width: 0px; margin: 0; background: transparent; }"
        );

        // Create a floating overlay vertical scrollbar that sits above content
        if (!m_overlayVScroll) {
            m_overlayVScroll = new QScrollBar(Qt::Vertical, m_infoWidget);
            m_overlayVScroll->setObjectName("overlayVScroll");
            m_overlayVScroll->setAutoFillBackground(false);
            m_overlayVScroll->setAttribute(Qt::WA_TranslucentBackground, true);
            m_overlayVScroll->setCursor(Qt::ArrowCursor);
            
            // Create timer for auto-hiding scrollbar after inactivity
            if (!m_scrollbarHideTimer) {
                m_scrollbarHideTimer = new QTimer(this);
                m_scrollbarHideTimer->setSingleShot(true);
                m_scrollbarHideTimer->setInterval(gScrollbarAutoHideDelayMs);
                connect(m_scrollbarHideTimer, &QTimer::timeout, this, [this]() {
                    if (m_overlayVScroll) {
                        m_overlayVScroll->hide();
                    }
                });
            }
            m_overlayVScroll->setStyleSheet(
                "QScrollBar#overlayVScroll { background: transparent; border: none; width: 8px; margin: 0px; }"
                " QScrollBar#overlayVScroll::groove:vertical { background: transparent; border: none; margin: 0px; }"
                " QScrollBar#overlayVScroll::handle:vertical { background: rgba(255,255,255,0.35); min-height: 24px; border-radius: 4px; }"
                " QScrollBar#overlayVScroll::handle:vertical:hover { background: rgba(255,255,255,0.55); }"
                " QScrollBar#overlayVScroll::handle:vertical:pressed { background: rgba(255,255,255,0.7); }"
                " QScrollBar#overlayVScroll::add-line:vertical, QScrollBar#overlayVScroll::sub-line:vertical { height: 0px; width: 0px; background: transparent; border: none; }"
                " QScrollBar#overlayVScroll::add-page:vertical, QScrollBar#overlayVScroll::sub-page:vertical { background: transparent; }"
            );
            // Sync with the hidden scroll area's vertical scrollbar
            QScrollBar* src = m_contentScroll->verticalScrollBar();
            connect(m_overlayVScroll, &QScrollBar::valueChanged, src, &QScrollBar::setValue);
            connect(src, &QScrollBar::rangeChanged, this, [this](int min, int max){
                if (m_overlayVScroll) m_overlayVScroll->setRange(min, max);
                updateOverlayVScrollVisibilityAndGeometry();
            });
            connect(src, &QScrollBar::valueChanged, this, [this](int v){ 
                if (m_overlayVScroll) m_overlayVScroll->setValue(v); 
            });
            
            // Show scrollbar and restart hide timer on any scroll activity
            auto showScrollbarAndRestartTimer = [this]() {
                if (m_overlayVScroll && m_scrollbarHideTimer) {
                    m_overlayVScroll->show();
                    m_scrollbarHideTimer->start(); // restart the 1.5s timer
                }
            };
            
            // Connect to all scroll activity events
            connect(m_overlayVScroll, &QScrollBar::valueChanged, this, showScrollbarAndRestartTimer);
            connect(src, &QScrollBar::valueChanged, this, showScrollbarAndRestartTimer);
            connect(m_overlayVScroll, &QScrollBar::sliderPressed, this, showScrollbarAndRestartTimer);
            connect(m_overlayVScroll, &QScrollBar::sliderMoved, this, showScrollbarAndRestartTimer);
            // Initialize current values immediately
            m_overlayVScroll->setRange(src->minimum(), src->maximum());
            m_overlayVScroll->setPageStep(src->pageStep());
            m_overlayVScroll->setValue(src->value());
        }

        m_contentWidget = new QWidget();
        m_contentWidget->setStyleSheet("background: transparent;");
        m_contentWidget->setAutoFillBackground(false);
        // Prevent unwanted stretching while allowing natural sizing
        m_contentWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        m_contentLayout = new QVBoxLayout(m_contentWidget);
        m_contentLayout->setContentsMargins(20, 16, 20, 16); // Margins only for content
        m_contentLayout->setSpacing(gMediaListItemSpacing);
        m_contentScroll->setWidget(m_contentWidget);
        
        // Add scroll area (containing content) to main layout
        m_infoLayout->addWidget(m_contentScroll);
        
        // Upload button in overlay (no title)
        m_overlayHeaderWidget = new QWidget(m_infoWidget);
        m_overlayHeaderWidget->setStyleSheet("background: transparent;");
        m_overlayHeaderWidget->setAutoFillBackground(false);
        // Prevent header area from expanding vertically; height should follow its children (the button)
        m_overlayHeaderWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* headerLayout = new QHBoxLayout(m_overlayHeaderWidget);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(0);
        
        // Upload button in overlay - integrated style with top border only
        m_uploadButton = new QPushButton("Upload", m_overlayHeaderWidget);
        m_uploadButton->setStyleSheet(
            "QPushButton { "
            "    padding: 8px 0px; "
            "    font-weight: bold; "
            "    font-size: 12px; "
            "    color: " + AppColors::colorToCss(AppColors::gOverlayTextColor) + "; "
            "    background: transparent; "
            "    border: none; "
            "    border-radius: 0px; "
            "} "
            "QPushButton:hover { "
            "    color: white; "
            "    background: rgba(255,255,255,0.05); "
            "} "
            "QPushButton:pressed { "
            "    color: white; "
            "    background: rgba(255,255,255,0.1); "
            "}"
        );
        m_uploadButton->setFixedHeight(40);
        m_uploadButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        
        // Full width button
        headerLayout->addWidget(m_uploadButton);
        // Do not add header here; refreshInfoOverlay() will place it at the bottom of the panel
        m_infoWidget->hide(); // hidden until first layout
    }
    // Initial content and layout
    refreshInfoOverlay();
    layoutInfoOverlay();
}

void ScreenCanvas::scheduleInfoOverlayRefresh() {
    if (m_infoRefreshQueued) return;
    m_infoRefreshQueued = true;
    QTimer::singleShot(0, [this]() {
        m_infoRefreshQueued = false;
        refreshInfoOverlay();
        layoutInfoOverlay();
    });
}

void ScreenCanvas::refreshInfoOverlay() {
    if (!m_infoWidget || !m_infoLayout || !m_contentLayout) return;
    // Avoid intermediate paints while rebuilding
    m_infoWidget->setUpdatesEnabled(false);
    m_infoWidget->hide();
    // Release any previous fixed-height constraints so we can shrink/grow accurately this pass
    m_infoWidget->setMinimumHeight(0);
    m_infoWidget->setMaximumHeight(QWIDGETSIZE_MAX);
    // Reset width constraints to allow shrinking
    m_infoWidget->setMaximumWidth(QWIDGETSIZE_MAX);
    m_infoWidget->setMinimumWidth(0);  // Temporarily remove minimum
    m_infoWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    
    // Force immediate geometry reset to clear any cached sizes
    m_infoWidget->resize(0, 0);
    m_infoWidget->updateGeometry();
    // Clear only the content layout (media items), keep the content widget and header widget
    while (m_contentLayout->count() > 0) {
        QLayoutItem* it = m_contentLayout->takeAt(0);
        if (!it) break;
        if (QWidget* w = it->widget()) {
            w->hide();
            delete w;
        }
        delete it;
    }
    // Collect media items
    QList<ResizableMediaBase*> media;
    if (m_scene) {
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) media.append(base);
        }
    }
    // Sort by z (topmost first)
    std::sort(media.begin(), media.end(), [](ResizableMediaBase* a, ResizableMediaBase* b){ return a->zValue() > b->zValue(); });

    auto humanSize = [](qint64 bytes) -> QString {
        double b = static_cast<double>(bytes);
        const char* units[] = {"B","KB","MB","GB"}; int u=0; while (b>=1024.0 && u<3){ b/=1024.0; ++u;} return QString::number(b, 'f', (u==0?0: (b<10?2:1))) + " " + units[u];
    };

    int measuredContentW = 0; // track widest intrinsic content width (excluding margins)
    for (ResizableMediaBase* m : media) {
        QString name = m->displayName();
        QSize sz = m->baseSizePx();
        QString dim = QString::number(sz.width()) + " x " + QString::number(sz.height()) + " px";
        QString sizeStr = QStringLiteral("n/a");
        QString src = m->sourcePath();
        if (!src.isEmpty()) {
            QFileInfo fi(src);
            if (fi.exists() && fi.isFile()) sizeStr = humanSize(fi.size());
        }
        // Row: name
        auto* nameLbl = new QLabel(name, m_contentWidget);
        nameLbl->setStyleSheet("color: white; background: transparent;");
        nameLbl->setAutoFillBackground(false);
        nameLbl->setAttribute(Qt::WA_TranslucentBackground, true);
        nameLbl->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed); // Allow expanding to push container width
        nameLbl->setWordWrap(false); // Don't wrap - expand container width instead
        nameLbl->setTextInteractionFlags(Qt::NoTextInteraction);
        nameLbl->setFixedHeight(18); // Exact height to prevent any extra spacing
        nameLbl->setContentsMargins(0, 0, 0, 0); // Force zero margins
        nameLbl->setAlignment(Qt::AlignLeft | Qt::AlignTop); // Align text to top
    m_contentLayout->addWidget(nameLbl);
    measuredContentW = std::max(measuredContentW, nameLbl->sizeHint().width());
        // Row: upload status or progress - fixed height container to prevent flickering
        auto* statusContainer = new QWidget(m_contentWidget);
        statusContainer->setStyleSheet("background: transparent;");
        statusContainer->setAutoFillBackground(false);
        statusContainer->setAttribute(Qt::WA_TranslucentBackground, true);
        statusContainer->setFixedHeight(20); // Fixed height to prevent flickering
        auto* statusLayout = new QVBoxLayout(statusContainer);
        statusLayout->setContentsMargins(0, 0, 0, 0);
        statusLayout->setSpacing(0);
        statusLayout->setAlignment(Qt::AlignVCenter);
        
        if (m->uploadState() == ResizableMediaBase::UploadState::Uploading) {
            auto* bar = new QProgressBar(statusContainer);
            bar->setRange(0, 100);
            bar->setValue(m->uploadProgress());
            bar->setTextVisible(false);
            bar->setFixedHeight(10);
            bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); // Preferred width
            // Blue progress bar styling consistent with theme - no border radius
            bar->setStyleSheet("QProgressBar{background: " + AppColors::colorToCss(AppColors::gMediaProgressBg) + ";} QProgressBar::chunk{background: " + AppColors::colorToCss(AppColors::gMediaProgressFill) + ";}");
            statusLayout->addWidget(bar, 0, Qt::AlignVCenter); // Only center vertically, full width horizontally
        } else {
            auto* status = new QLabel(m->uploadState() == ResizableMediaBase::UploadState::Uploaded ? QStringLiteral("Uploaded") : QStringLiteral("Not uploaded"), statusContainer);
            const QString color = (m->uploadState() == ResizableMediaBase::UploadState::Uploaded) ? AppColors::colorToCss(AppColors::gMediaUploadedColor) : AppColors::colorToCss(AppColors::gMediaNotUploadedColor);
            status->setStyleSheet(QString("color: %1; font-size: 14px; background: transparent;").arg(color));
            status->setAutoFillBackground(false);
            status->setAttribute(Qt::WA_TranslucentBackground, true);
            status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            status->setWordWrap(true);
            status->setTextInteractionFlags(Qt::NoTextInteraction);
            status->setFixedHeight(16); // Fixed height to prevent stretching
            statusLayout->addWidget(status, 0, Qt::AlignLeft | Qt::AlignVCenter); // Left-aligned, vertically centered
        }
        
        m_contentLayout->addWidget(statusContainer);
        // Row: details smaller under status
        auto* details = new QLabel(dim + QStringLiteral("  ·  ") + sizeStr, m_contentWidget);
        details->setStyleSheet("color: " + AppColors::colorToCss(AppColors::gTextSecondary) + "; font-size: 14px; background: transparent;");
        details->setAutoFillBackground(false);
        details->setAttribute(Qt::WA_TranslucentBackground, true);
        details->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed); // Allow expanding to push container width
        details->setWordWrap(false); // Keep dimensions and size on single line
        details->setTextInteractionFlags(Qt::NoTextInteraction);
        details->setFixedHeight(18); // Fixed height to prevent stretching
        m_contentLayout->addWidget(details);
        measuredContentW = std::max(measuredContentW, details->sizeHint().width());
    }

    // Finally, place the header (with upload button) at the bottom, full width, no margins
    if (m_overlayHeaderWidget) {
        m_overlayHeaderWidget->show();
        m_infoLayout->addWidget(m_overlayHeaderWidget);
    }

    // Force layout recalculation and resize widget to fit content
    if (m_infoLayout) {
        m_infoLayout->invalidate();
        m_infoLayout->activate();
    }
    
    m_infoWidget->ensurePolished();
    m_infoWidget->updateGeometry();
    // Compute natural preferred size including header
    const QSize contentHint = m_contentLayout ? m_contentLayout->totalSizeHint() : m_contentWidget->sizeHint();
    const QSize headerHint = m_overlayHeaderWidget ? m_overlayHeaderWidget->sizeHint() : QSize(0,0);
    const int naturalHeight = contentHint.height() + headerHint.height();
    
    // Calculate desired width from measured content plus margins
    const int margin = 16;
    const int contentMarginsLR = m_contentLayout ? (m_contentLayout->contentsMargins().left() + m_contentLayout->contentsMargins().right()) : 0;
    int desiredW = std::max(measuredContentW + contentMarginsLR, headerHint.width());
    
    // If no content measured (empty list), use minimum width
    if (measuredContentW == 0 && media.isEmpty()) {
        desiredW = m_infoWidget->minimumWidth();
    } else {
        desiredW = std::max(desiredW, m_infoWidget->minimumWidth());
    }
    if (viewport()) {
        const int capW = static_cast<int>(viewport()->width() * 0.5); // 50% viewport cap
        desiredW = std::min(desiredW, capW);
    }
    // Cap height to viewport height minus margins to avoid overlay exceeding canvas
    const int maxOverlayH = viewport() ? std::max(0, viewport()->height() - margin*2) : naturalHeight;
    int overlayH = naturalHeight;
    if (overlayH > maxOverlayH) {
        // Clamp the scroll viewport height and show overlay scrollbar
        if (m_contentScroll) {
            const int maxContentH = std::max(0, maxOverlayH - headerHint.height());
            m_contentScroll->setMaximumHeight(maxContentH);
            m_contentScroll->setMinimumHeight(0);
            m_contentScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        }
        overlayH = maxOverlayH;
    } else {
        // No scroll: wrap tightly and prepare overlay scrollbar to hide
        if (m_contentScroll) {
            m_contentScroll->setMaximumHeight(contentHint.height());
            m_contentScroll->setMinimumHeight(0);
            m_contentScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        }
    }
    
    // Allow content widgets to expand with their content
    if (m_contentWidget) {
        m_contentWidget->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Maximum);
    }
    if (m_contentScroll) {
        m_contentScroll->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Expanding);
    }
    
    // Apply final widget dimensions
    m_infoWidget->setFixedHeight(overlayH);
    m_infoWidget->setMinimumWidth(200);  // Restore minimum width
    m_infoWidget->setFixedWidth(desiredW);
    m_infoWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    
    // Force layout recalculation
    if (m_infoLayout) {
        m_infoLayout->invalidate();
        m_infoLayout->activate();
    }
    m_infoWidget->updateGeometry();
    updateOverlayVScrollVisibilityAndGeometry();
    
    // Only show overlay if there are media items present
    if (!media.isEmpty()) {
        m_infoWidget->show();
        if (m_infoBorderRect) m_infoBorderRect->setVisible(true);
    } else {
        // Hide overlay when no media is present
        m_infoWidget->hide();
        if (m_infoBorderRect) m_infoBorderRect->setVisible(false);
    }
    
    m_infoWidget->setUpdatesEnabled(true);
    // In case the widget's final metrics settle after this event loop turn (common on first show or font/layout updates),
    // schedule a one-shot re-anchor to avoid a transient displaced position after dropping media.
    QTimer::singleShot(0, [this]() { layoutInfoOverlay(); });
}

void ScreenCanvas::layoutInfoOverlay() {
    if (!m_infoWidget || !viewport()) return;
    const int margin = 16;
    const int w = m_infoWidget->width();
    const int x = viewport()->width() - margin - w;
    const int y = viewport()->height() - margin - m_infoWidget->height();
    m_infoWidget->move(std::max(0, x), std::max(0, y));
    
    // Create or update border rectangle in scene (same approach as MediaSettingsPanel)
    if (m_infoWidget->isVisible() && scene()) {
        if (!m_infoBorderRect) {
            m_infoBorderRect = new MouseBlockingRoundedRectItem();
            m_infoBorderRect->setRadius(gOverlayCornerRadiusPx);
            applyOverlayBorder(m_infoBorderRect);
            m_infoBorderRect->setBrush(QBrush(AppColors::gOverlayBackgroundColor));
            m_infoBorderRect->setZValue(12009.5); // same Z-value as MediaSettingsPanel
            m_infoBorderRect->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
            m_infoBorderRect->setData(0, QStringLiteral("overlay"));
            scene()->addItem(m_infoBorderRect);
        }
        // Position border rect using same method as MediaSettingsPanel - direct viewport transform
        QPointF widgetTopLeftVp(std::max(0, x), std::max(0, y));
        QPointF widgetTopLeftScene = viewportTransform().inverted().map(widgetTopLeftVp);
        m_infoBorderRect->setRect(0, 0, w, m_infoWidget->height());
        m_infoBorderRect->setPos(widgetTopLeftScene);
    } else if (m_infoBorderRect) {
        // Hide border when overlay is hidden
        m_infoBorderRect->setVisible(false);
    }
    
    updateOverlayVScrollVisibilityAndGeometry();
}

void ScreenCanvas::updateInfoOverlayGeometryForViewport() {
    if (!m_infoWidget || !m_infoLayout || !viewport()) return;
    if (!m_infoWidget->isVisible()) return; // nothing to adjust if hidden
    // Compute current natural size based on existing content
    const QSize contentHint = m_contentLayout ? m_contentLayout->totalSizeHint() : (m_contentWidget ? m_contentWidget->sizeHint() : QSize());
    const QSize headerHint = m_overlayHeaderWidget ? m_overlayHeaderWidget->sizeHint() : QSize(0,0);
    const int naturalHeight = contentHint.height() + headerHint.height();
    const int margin = 16;
    const int maxOverlayH = std::max(0, viewport()->height() - margin*2);
    int overlayH = naturalHeight;
    if (overlayH > maxOverlayH) {
        if (m_contentScroll) {
            const int maxContentH = std::max(0, maxOverlayH - headerHint.height());
            m_contentScroll->setMaximumHeight(maxContentH);
            m_contentScroll->setMinimumHeight(0);
            m_contentScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        }
        overlayH = maxOverlayH;
    } else {
        if (m_contentScroll) {
            m_contentScroll->setMaximumHeight(contentHint.height());
            m_contentScroll->setMinimumHeight(0);
            m_contentScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        }
    }
    // Recompute desired width from current content and apply 50% viewport cap
    int desiredW = std::max(contentHint.width(), headerHint.width());
    if (m_contentLayout) {
        const int lr = m_contentLayout->contentsMargins().left() + m_contentLayout->contentsMargins().right();
        desiredW = std::max(desiredW, contentHint.width() + lr);
    }
    desiredW = std::max(desiredW, m_infoWidget->minimumWidth());
    const int capW = static_cast<int>(viewport()->width() * 0.5);
    desiredW = std::min(desiredW, capW);
    
    // Update widget dimensions
    m_infoWidget->setFixedHeight(overlayH);
    m_infoWidget->setFixedWidth(desiredW);
    m_infoWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    
    // Force layout recalculation
    if (m_infoLayout) {
        m_infoLayout->invalidate();
        m_infoLayout->activate();
    }
    m_infoWidget->updateGeometry();
    layoutInfoOverlay();
    updateOverlayVScrollVisibilityAndGeometry();
}

void ScreenCanvas::updateOverlayVScrollVisibilityAndGeometry() {
    if (!m_overlayVScroll || !m_contentScroll || !m_overlayVScroll->parentWidget()) return;
    QScrollBar* src = m_contentScroll->verticalScrollBar();
    if (!src) { m_overlayVScroll->hide(); return; }
    const bool need = src->maximum() > src->minimum();
    if (!need) { m_overlayVScroll->hide(); return; }
    // Position the overlay scrollbar on the right edge of the overlay panel, with margins for aesthetics
    const int sbWidth = 8;
    const int margin = 6; // small inset from edge (same as settings overlay)
    const int topMargin = 6; // top margin (same as settings overlay)
    const int bottomMargin = 6; // bottom margin (same as settings overlay)
    // Match the content scroll viewport geometry within the overlay panel
    const QRect contentGeom = m_contentScroll->geometry();
    const int x = m_infoWidget->width() - sbWidth - margin;
    const int y = contentGeom.top() + topMargin;
    const int h = qMax(0, contentGeom.height() - topMargin - bottomMargin);
    // Sync range/value/page step on every geometry update
    m_overlayVScroll->setRange(src->minimum(), src->maximum());
    m_overlayVScroll->setPageStep(src->pageStep());
    m_overlayVScroll->setValue(src->value());
    m_overlayVScroll->setGeometry(x, y, sbWidth, h);
    
    // Only show if not using auto-hide, or if timer is currently active
    // (this prevents forcing visibility when timer should hide it)
    if (!m_scrollbarHideTimer || m_scrollbarHideTimer->isActive()) {
        m_overlayVScroll->show();
    }
}

ScreenCanvas::ScreenCanvas(QWidget* parent) : QGraphicsView(parent) {
    setAcceptDrops(true);
    setDragMode(QGraphicsView::NoDrag); // manual panning / selection logic
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    // Expand virtual scene rect so user can pan into empty space (design tool feel)
    m_scene->setSceneRect(-50000, -50000, 100000, 100000);
    setRenderHint(QPainter::Antialiasing, true);
    // Figma-like: no scrollbars, we manually pan/zoom via transform.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Remove frame and make background transparent so only content shows.
    setFrameStyle(QFrame::NoFrame);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setTransformationAnchor(QGraphicsView::NoAnchor); // we'll anchor manually
    if (viewport()) {
        viewport()->setAutoFillBackground(false);
        viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    }
    // FullViewportUpdate avoids artifacts when we translate/scale manually.
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    viewport()->setMouseTracking(true);
    grabGesture(Qt::PinchGesture); // for non-macOS platforms
    m_nativePinchGuardTimer = new QTimer(this);
    m_nativePinchGuardTimer->setInterval(180); // short guard after last native pinch
    m_nativePinchGuardTimer->setSingleShot(true);
    connect(m_nativePinchGuardTimer, &QTimer::timeout, this, [this]() { m_nativePinchActive = false; });

    // On scene changes, re-anchor, and refresh the overlay only if media count changed
    connect(m_scene, &QGraphicsScene::changed, this, [this](const QList<QRectF>&){ layoutInfoOverlay(); maybeRefreshInfoOverlayOnSceneChanged(); });
    
    // Set up screen border snapping callbacks for media items
    ResizableMediaBase::setScreenSnapCallback([this](const QPointF& pos, const QRectF& bounds, bool shift) {
        return snapToScreenBorders(pos, bounds, shift);
    });
    ResizableMediaBase::setResizeSnapCallback([this](qreal scale, const QPointF& fixed, const QPointF& moving, const QSize& base, bool shift) {
        return snapResizeToScreenBorders(scale, fixed, moving, base, shift);
    });

    // Initialize global info overlay (top-right)
    initInfoOverlay();

    // Refresh overlay when any media upload state changes (coalesce multiple changes)
    ResizableMediaBase::setUploadChangedNotifier([this]() {
        scheduleInfoOverlayRefresh();
    });
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    createScreenItems();
}

void ScreenCanvas::clearScreens() {
    for (auto* r : m_screenItems) {
        if (r) m_scene->removeItem(r);
        delete r;
    }
    m_screenItems.clear();
    
    // Reset info overlay border rectangle when clearing screens (connection reload)
    if (m_infoBorderRect) {
        if (m_scene) m_scene->removeItem(m_infoBorderRect);
        delete m_infoBorderRect;
        m_infoBorderRect = nullptr;
    }
}

void ScreenCanvas::recenterWithMargin(int marginPx) {
    QRectF bounds = screensBoundingRect();
    if (bounds.isNull() || !bounds.isValid()) return;
    const QSize vp = viewport() ? viewport()->size() : size();
    const qreal availW = static_cast<qreal>(vp.width())  - 2.0 * marginPx;
    const qreal availH = static_cast<qreal>(vp.height()) - 2.0 * marginPx;
    if (availW <= 1 || availH <= 1 || bounds.width() <= 0 || bounds.height() <= 0) {
        fitInView(bounds, Qt::KeepAspectRatio);
        centerOn(bounds.center());
        return;
    }
    const qreal sx = availW / bounds.width();
    const qreal sy = availH / bounds.height();
    const qreal s = std::min(sx, sy);
    QTransform t; t.scale(s, s); setTransform(t); centerOn(bounds.center());
    if (m_scene) {
        const QList<QGraphicsItem*> sel = m_scene->selectedItems();
        for (QGraphicsItem* it : sel) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) v->requestOverlayRelayout();
            if (auto* b = dynamic_cast<ResizableMediaBase*>(it)) b->requestLabelRelayout();
        }
        // Also relayout all media overlays to keep absolute panels pinned
        relayoutAllMediaOverlays(m_scene);
    }
    m_ignorePanMomentum = true; m_momentumPrimed = false; m_lastMomentumMag = 0.0; m_lastMomentumDelta = QPoint(0,0); m_momentumTimer.restart();
}

void ScreenCanvas::updateRemoteCursor(int globalX, int globalY) {
    // Inputs are remote global desktop coordinates relative to remote virtual desktop origin.
    QPointF scenePos = mapRemoteCursorToScene(globalX, globalY);
    if (scenePos.isNull()) return; // outside any screen
    if (!m_remoteCursorDot) {
        recreateRemoteCursorItem();
    }
    if (m_remoteCursorDot) { m_remoteCursorDot->setPos(scenePos); m_remoteCursorDot->show(); }
}

void ScreenCanvas::hideRemoteCursor() {
    if (m_remoteCursorDot) m_remoteCursorDot->hide();
}

void ScreenCanvas::setMediaHandleSelectionSizePx(int px) { m_mediaHandleSelectionSizePx = qMax(1, px); }
void ScreenCanvas::setMediaHandleVisualSizePx(int px) { m_mediaHandleVisualSizePx = qMax(1, px); }
void ScreenCanvas::setMediaHandleSizePx(int px) { setMediaHandleSelectionSizePx(px); setMediaHandleVisualSizePx(px); }

void ScreenCanvas::setScreenBorderWidthPx(int px) {
    m_screenBorderWidthPx = qMax(0, px);
    if (!m_scene) return;
    for (int i = 0; i < m_screenItems.size() && i < m_screens.size(); ++i) {
        QGraphicsRectItem* item = m_screenItems[i]; if (!item) continue;
        int penW = m_screenBorderWidthPx;
        int oldPenW = static_cast<int>(item->pen().widthF());
        QRectF currentInner = item->rect();
        QRectF outer = currentInner.adjusted(-(oldPenW/2.0), -(oldPenW/2.0), (oldPenW/2.0), (oldPenW/2.0));
        QRectF newInner = outer.adjusted(penW/2.0, penW/2.0, -penW/2.0, -penW/2.0);
        item->setRect(newInner); QPen p = item->pen(); p.setWidthF(penW); item->setPen(p);
    }
}

bool ScreenCanvas::event(QEvent* event) {
    // Block gestures that would affect the canvas if the pointer is over the overlay
    if ((event->type() == QEvent::Gesture || event->type() == QEvent::NativeGesture) && m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        QPoint vpPos = viewport()->mapFromGlobal(QCursor::pos());
        if (m_infoWidget->geometry().contains(vpPos)) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::Gesture) {
    return gestureEvent(static_cast<QGestureEvent*>(event));
    }
    if (event->type() == QEvent::NativeGesture) {
        auto* ng = static_cast<QNativeGestureEvent*>(event);
        if (ng->gestureType() == Qt::ZoomNativeGesture) {
            m_nativePinchActive = true; m_nativePinchGuardTimer->start();
            const qreal factor = std::pow(2.0, ng->value());
            QPoint vpPos = viewport()->mapFromGlobal(QCursor::pos());
            if (!viewport()->rect().contains(vpPos)) {
                QPoint viewPos = ng->position().toPoint();
                vpPos = viewport()->mapFrom(this, viewPos);
                if (!viewport()->rect().contains(vpPos)) vpPos = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
            }
            m_lastMousePos = vpPos; // remember anchor
            zoomAroundViewportPos(vpPos, factor);
            relayoutAllMediaOverlays(m_scene);
            layoutInfoOverlay();
            event->accept(); return true;
        }
    }
    return QGraphicsView::event(event);
}

bool ScreenCanvas::viewportEvent(QEvent* event) {
#ifdef Q_OS_MACOS
    // Some trackpad gestures may arrive directly to the viewport; handle zoom similarly
    if (event->type() == QEvent::NativeGesture) {
        if (m_infoWidget && m_infoWidget->isVisible()) {
            QPoint vpPosNow = viewport()->mapFromGlobal(QCursor::pos());
            if (m_infoWidget->geometry().contains(vpPosNow)) { event->accept(); return true; }
        }
        auto* ng = static_cast<QNativeGestureEvent*>(event);
        if (ng->gestureType() == Qt::ZoomNativeGesture) {
            m_nativePinchActive = true; m_nativePinchGuardTimer->start();
            const qreal factor = std::pow(2.0, ng->value());
            QPoint vpPos = viewport()->mapFrom(this, ng->position().toPoint());
            if (!viewport()->rect().contains(vpPos)) vpPos = viewport()->rect().center();
            m_lastMousePos = vpPos; // remember anchor
            zoomAroundViewportPos(vpPos, factor);
            relayoutAllMediaOverlays(m_scene);
            layoutInfoOverlay();
            event->accept();
            return true;
        }
    }
#endif
    return QGraphicsView::viewportEvent(event);
}

bool ScreenCanvas::gestureEvent(QGestureEvent* event) {
    if (QGesture* g = event->gesture(Qt::PinchGesture)) {
        auto* pinch = static_cast<QPinchGesture*>(g);
        // If pinch is over the overlay, don't alter the canvas
        if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
            QPoint vpPosChk = pinch->centerPoint().toPoint();
            if (m_infoWidget->geometry().contains(vpPosChk)) { event->accept(); return true; }
        }
        if (pinch->changeFlags() & QPinchGesture::ScaleFactorChanged) {
            // centerPoint() is already in the receiving widget's coordinate system (our view)
            QPoint vpPos = pinch->centerPoint().toPoint();
            if (!viewport()->rect().contains(vpPos)) {
                // Fallbacks: cursor inside viewport, then last mouse pos, then center
                QPoint cursorVp = viewport()->mapFromGlobal(QCursor::pos());
                if (viewport()->rect().contains(cursorVp)) vpPos = cursorVp;
                else vpPos = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
            }
            m_lastMousePos = vpPos; // keep coherent for subsequent wheel/native zooms
            const qreal factor = pinch->scaleFactor();
            zoomAroundViewportPos(vpPos, factor);
            relayoutAllMediaOverlays(m_scene);
            layoutInfoOverlay();
        }
        event->accept();
        return true;
    }
    return QGraphicsView::event(event);
}

void ScreenCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        // Require Command (macOS) or Control (other platforms) modifier to delete
#ifdef Q_OS_MACOS
        const bool deleteAllowed = event->modifiers().testFlag(Qt::MetaModifier);
#else
        const bool deleteAllowed = event->modifiers().testFlag(Qt::ControlModifier);
#endif
        if (deleteAllowed) {
            if (m_scene) {
                const QList<QGraphicsItem*> sel = m_scene->selectedItems();
                for (QGraphicsItem* it : sel) {
                    if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) {
                        base->prepareForDeletion();
                        // Remove from scene first so it stops receiving events.
                        m_scene->removeItem(base);
                        // Now that prepareForDeletion performs full detachment, we can delete immediately.
                        delete base;
                    }
                }
                refreshInfoOverlay();
            }
            event->accept();
            return;
        }
        // Without the required modifier, do not delete; fall through to base handling
    }
    if (event->key() == Qt::Key_Space) { recenterWithMargin(53); event->accept(); return; }
    // Arrow key handling: 
    // - Shift+Up/Down: change Z-order of selected media
    // - Regular arrows: nudge selected media by 1 screen pixel
    // - No selection: consume arrows to prevent view navigation
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
        event->key() == Qt::Key_Up   || event->key() == Qt::Key_Down) {
        
        // Handle Shift+Up/Down for Z-order changes
        if (event->modifiers().testFlag(Qt::ShiftModifier) && 
            (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
            
            if (m_scene) {
                const QList<QGraphicsItem*> sel = m_scene->selectedItems();
                for (QGraphicsItem* it : sel) {
                    if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) {
                        if (event->key() == Qt::Key_Up) {
                            moveMediaUp(base);
                        } else {
                            moveMediaDown(base);
                        }
                    }
                }
            }
            event->accept();
            return;
        }
        
        // Regular arrow key movement (no Shift modifier)
        if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
            bool moved = false;
            if (m_scene) {
                const QList<QGraphicsItem*> sel = m_scene->selectedItems();
                if (!sel.isEmpty()) {
                    // Move by exactly one scene pixel (grid unit) regardless of zoom level
                    const qreal unit = ResizableMediaBase::sceneGridUnit();
                    const qreal dxScene = unit;
                    const qreal dyScene = unit;
                    qreal dx = 0.0, dy = 0.0;
                    switch (event->key()) {
                        case Qt::Key_Left:  dx = -dxScene; break;
                        case Qt::Key_Right: dx =  dxScene; break;
                        case Qt::Key_Up:    dy = -dyScene; break;
                        case Qt::Key_Down:  dy =  dyScene; break;
                        default: break;
                    }
                    if (dx != 0.0 || dy != 0.0) {
                        for (QGraphicsItem* it : sel) {
                            if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) {
                                base->setPos(base->pos() + QPointF(dx, dy));
                                // Relayout overlays/labels for this item after movement
                                base->requestLabelRelayout();
                                base->updateOverlayLayout();
                                moved = true;
                            }
                        }
                    }
                }
            }
            if (moved) {
                event->accept();
                return;
            }
        }
        
        // Consume arrows to avoid view navigation
        event->accept();
        return;
    }
    // Block page/navigation keys from moving the view
    switch (event->key()) {
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            event->accept();
            return;
        default:
            break;
    }
    QGraphicsView::keyPressEvent(event);
}

void ScreenCanvas::mousePressEvent(QMouseEvent* event) {
    // When pressing over the overlay, forward event to the overlay widget under cursor and stop canvas handling
    if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        const QPoint vpPos = viewport()->mapFrom(this, event->pos());
        if (m_infoWidget->geometry().contains(vpPos)) {
            const QPoint overlayLocal = m_infoWidget->mapFrom(viewport(), vpPos);
            QWidget* dst = m_infoWidget->childAt(overlayLocal);
            if (!dst) dst = m_infoWidget;
            const QPoint dstLocal = dst->mapFrom(m_infoWidget, overlayLocal);
            const QPoint globalP = dst->mapToGlobal(dstLocal);
            const QWidget* win = dst->window();
            const QPoint windowP = win ? win->mapFromGlobal(globalP) : QPoint();
            const QPointF localF = QPointF(dstLocal);
            const QPointF windowF = QPointF(windowP);
            const QPointF screenF = QPointF(globalP);
            QMouseEvent forwarded(event->type(), localF, windowF, screenF, event->button(), event->buttons(), event->modifiers());
            QCoreApplication::sendEvent(dst, &forwarded);
            event->accept();
            return;
        }
    }
    // (Space-to-pan currently disabled pending dedicated key tracking)
    const bool spaceHeld = false;
    if (event->button() == Qt::LeftButton) {
        // If the pointer is over any blocking overlay element (e.g., settings panel), route to base handler
        // immediately to avoid altering selection/panning. Media overlays (filename, buttons) should not block.
        {
            const QList<QGraphicsItem*> hit = items(event->pos());
            for (QGraphicsItem* hi : hit) {
                if (hi->data(0).toString() == QLatin1String("blocking-overlay")) {
                    QGraphicsView::mousePressEvent(event);
                    return;
                }
            }
        }
        if (m_scene) {
            const QPointF scenePosEarly = mapToScene(event->pos());
            const QList<QGraphicsItem*> selEarly = m_scene->selectedItems();
            for (QGraphicsItem* it : selEarly) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) { if (v->handleControlsPressAtItemPos(v->mapFromScene(scenePosEarly))) { m_overlayMouseDown = true; event->accept(); return; } }
        }
        // Space+drag always pans
        if (spaceHeld) { m_panning = true; m_lastPanPoint = event->pos(); event->accept(); return; }
        const QPointF scenePos = mapToScene(event->pos());
        ResizableMediaBase* topHandleItem = nullptr; qreal topZ = -std::numeric_limits<qreal>::infinity();
        const QList<QGraphicsItem*> sel = m_scene ? m_scene->selectedItems() : QList<QGraphicsItem*>();
        for (QGraphicsItem* it : sel) if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) if (rp->isSelected() && rp->isOnHandleAtItemPos(rp->mapFromScene(scenePos))) { if (rp->zValue() > topZ) { topZ = rp->zValue(); topHandleItem = rp; } }
        if (topHandleItem) { if (topHandleItem->beginResizeAtScenePos(scenePos)) { viewport()->setCursor(topHandleItem->cursorForScenePos(scenePos)); event->accept(); return; } }
        const QList<QGraphicsItem*> hitItems = items(event->pos()); bool clickedOverlayOnly = false; bool anyMedia = false;
        for (QGraphicsItem* hi : hitItems) {
            if (hi->data(0).toString() == QLatin1String("overlay")) clickedOverlayOnly = true;
            QGraphicsItem* scan = hi; while (scan) { if (dynamic_cast<ResizableMediaBase*>(scan)) { anyMedia = true; break; } scan = scan->parentItem(); }
        }
        bool hasOverlay = false;
        for (QGraphicsItem* hi : hitItems) {
            if (hi->data(0).toString() == QLatin1String("overlay")) hasOverlay = true;
        }
        if (hasOverlay) {
            // Preserve/ensure selection of the media under the overlay to avoid selection loss
            auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; };
            ResizableMediaBase* mediaUnder = nullptr; for (QGraphicsItem* it : hitItems) { if ((mediaUnder = toMedia(it))) break; }
            if (mediaUnder && !mediaUnder->isSelected()) mediaUnder->setSelected(true);
            QGraphicsView::mousePressEvent(event);
            return;
        }
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; };
        ResizableMediaBase* mediaHit = nullptr; for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (mediaHit) {
            if (m_scene) m_scene->clearSelection(); if (!mediaHit->isSelected()) mediaHit->setSelected(true);
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) { const QPointF itemPos = v->mapFromScene(mapToScene(event->pos())); if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; } }
            QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(), event->button(), event->buttons(), Qt::NoModifier);
            QGraphicsView::mousePressEvent(&synthetic);
            if (m_scene) m_scene->clearSelection(); mediaHit->setSelected(true); return;
        }
        for (QGraphicsItem* it : scene()->selectedItems()) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) { const QPointF itemPos = v->mapFromScene(mapToScene(event->pos())); if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; } }
        if (m_scene) m_scene->clearSelection();
        // Start panning when clicking empty space: capture precise anchor so the scene point under
        // the cursor stays under the cursor during the entire drag.
        m_panning = true;
        m_lastPanPoint = event->pos();
        m_panAnchorView = event->pos();
        m_panAnchorScene = mapToScene(event->pos());
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void ScreenCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    // Forward double-clicks to overlay when over it
    if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        const QPoint vpPos = viewport()->mapFrom(this, event->pos());
        if (m_infoWidget->geometry().contains(vpPos)) {
            const QPoint overlayLocal = m_infoWidget->mapFrom(viewport(), vpPos);
            QWidget* dst = m_infoWidget->childAt(overlayLocal);
            if (!dst) dst = m_infoWidget;
            const QPoint dstLocal = dst->mapFrom(m_infoWidget, overlayLocal);
            const QPoint globalP = dst->mapToGlobal(dstLocal);
            const QWidget* win = dst->window();
            const QPoint windowP = win ? win->mapFromGlobal(globalP) : QPoint();
            const QPointF localF = QPointF(dstLocal);
            const QPointF windowF = QPointF(windowP);
            const QPointF screenF = QPointF(globalP);
            QMouseEvent forwarded(event->type(), localF, windowF, screenF, event->button(), event->buttons(), event->modifiers());
            QCoreApplication::sendEvent(dst, &forwarded);
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::LeftButton) {
        // Do not change selection when double-clicking on overlay elements
        {
            const QList<QGraphicsItem*> hit = items(event->pos());
            for (QGraphicsItem* hi : hit) {
                if (hi->data(0).toString() == QLatin1String("overlay")) {
                    QGraphicsView::mouseDoubleClickEvent(event);
                    return;
                }
            }
        }
        if (m_scene) {
            const QPointF scenePosSel = mapToScene(event->pos());
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->handleControlsPressAtItemPos(v->mapFromScene(scenePosSel))) { m_overlayMouseDown = true; event->accept(); return; }
        }
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; };
        ResizableMediaBase* mediaHit = nullptr; for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (mediaHit) { if (scene()) scene()->clearSelection(); if (!mediaHit->isSelected()) mediaHit->setSelected(true); if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) { const QPointF itemPos = v->mapFromScene(mapToScene(event->pos())); if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; } } QGraphicsView::mouseDoubleClickEvent(event); if (scene()) scene()->clearSelection(); mediaHit->setSelected(true); return; }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ScreenCanvas::mouseMoveEvent(QMouseEvent* event) {
    // While over overlay, forward moves to overlay widget and block canvas panning
    if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        const QPoint vpPos = viewport()->mapFrom(this, event->pos());
        if (m_infoWidget->geometry().contains(vpPos)) {
            const QPoint overlayLocal = m_infoWidget->mapFrom(viewport(), vpPos);
            QWidget* dst = m_infoWidget->childAt(overlayLocal);
            if (!dst) dst = m_infoWidget;
            const QPoint dstLocal = dst->mapFrom(m_infoWidget, overlayLocal);
            const QPoint globalP = dst->mapToGlobal(dstLocal);
            const QWidget* win = dst->window();
            const QPoint windowP = win ? win->mapFromGlobal(globalP) : QPoint();
            const QPointF localF = QPointF(dstLocal);
            const QPointF windowF = QPointF(windowP);
            const QPointF screenF = QPointF(globalP);
            QMouseEvent forwarded(event->type(), localF, windowF, screenF, event->button(), event->buttons(), event->modifiers());
            QCoreApplication::sendEvent(dst, &forwarded);
            event->accept();
            return;
        }
    }
    if (m_overlayMouseDown) {
        if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) { if (v->isDraggingProgress() || v->isDraggingVolume()) { v->updateDragWithScenePos(mapToScene(event->pos())); event->accept(); return; } } }
        event->accept(); return;
    }
    m_lastMousePos = event->pos();
    const QPointF scenePos = mapToScene(event->pos());
    Qt::CursorShape resizeCursor = Qt::ArrowCursor; bool onResizeHandle = false; qreal topZ = -std::numeric_limits<qreal>::infinity();
    const QList<QGraphicsItem*> sel = m_scene ? m_scene->selectedItems() : QList<QGraphicsItem*>();
    for (QGraphicsItem* it : sel) if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) if (rp->isSelected() && rp->zValue() >= topZ) { Qt::CursorShape itemCursor = rp->cursorForScenePos(scenePos); if (itemCursor != Qt::ArrowCursor) { resizeCursor = itemCursor; onResizeHandle = true; topZ = rp->zValue(); } }
    if (onResizeHandle) viewport()->setCursor(resizeCursor); else viewport()->unsetCursor();
    if (event->buttons() & Qt::LeftButton) {
        for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) { v->updateDragWithScenePos(mapToScene(event->pos())); event->accept(); return; }
        const QList<QGraphicsItem*> hitItems = items(event->pos()); auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; }; bool hitMedia = false; for (QGraphicsItem* it : hitItems) if (toMedia(it)) { hitMedia = true; break; } if (hitMedia) { QGraphicsView::mouseMoveEvent(event); return; }
        if (m_panning) {
            // Compute where the original anchor scene point currently appears in view coordinates
            const QPoint currentAnchorView = mapFromScene(m_panAnchorScene);
            const QPoint deltaView = event->pos() - currentAnchorView;
            if (!deltaView.isNull()) {
                QTransform t = transform();
                // Translate by the exact amount needed so that the anchor scene point maps to the
                // current mouse position (keeps the grabbed point perfectly pinned under the cursor)
                t.translate(deltaView.x() / t.m11(), deltaView.y() / t.m22());
                setTransform(t);
                // Keep absolute panels pinned during pan
                relayoutAllMediaOverlays(m_scene);
                layoutInfoOverlay();
            }
            m_lastPanPoint = event->pos();
            event->accept();
            return;
        }
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ScreenCanvas::mouseReleaseEvent(QMouseEvent* event) {
    // Forward release to overlay when over it and block canvas handling
    if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        const QPoint vpPos = viewport()->mapFrom(this, event->pos());
        if (m_infoWidget->geometry().contains(vpPos)) {
            const QPoint overlayLocal = m_infoWidget->mapFrom(viewport(), vpPos);
            QWidget* dst = m_infoWidget->childAt(overlayLocal);
            if (!dst) dst = m_infoWidget;
            const QPoint dstLocal = dst->mapFrom(m_infoWidget, overlayLocal);
            const QPointF localF = QPointF(dstLocal);
            const QPointF globalF = QPointF(dst->mapToGlobal(dstLocal));
            QMouseEvent forwarded(event->type(), localF, QPointF(), globalF, event->button(), event->buttons(), event->modifiers());
            QCoreApplication::sendEvent(dst, &forwarded);
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::LeftButton) {
        // If releasing over any blocking overlay item, deliver the event directly and avoid selection churn
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        bool hasBlockingOverlay = false; for (QGraphicsItem* hi : hitItems) if (hi->data(0).toString() == QLatin1String("blocking-overlay")) { hasBlockingOverlay = true; break; }
        if (hasBlockingOverlay) { QGraphicsView::mouseReleaseEvent(event); return; }
        if (m_overlayMouseDown) {
            if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->isDraggingProgress() || v->isDraggingVolume()) v->endDrag(); }
            m_overlayMouseDown = false; event->accept(); return;
        }
        for (QGraphicsItem* it : m_scene->items()) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) { v->endDrag(); event->accept(); return; }
        if (m_panning) { m_panning = false; event->accept(); return; }
        bool wasResizing = false; for (QGraphicsItem* it : m_scene->items()) if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) if (rp->isActivelyResizing()) { wasResizing = true; break; }
        if (wasResizing) viewport()->unsetCursor();
        QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(), event->button(), event->buttons(), Qt::NoModifier);
        QGraphicsView::mouseReleaseEvent(&synthetic);
        if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); if (!sel.isEmpty()) { const QList<QGraphicsItem*> hitItems = items(event->pos()); auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; }; ResizableMediaBase* keep = nullptr; for (QGraphicsItem* it : hitItems) if ((keep = toMedia(it))) break; if (!keep) for (QGraphicsItem* it : sel) if ((keep = dynamic_cast<ResizableMediaBase*>(it))) break; m_scene->clearSelection(); if (keep) keep->setSelected(true); } }
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ScreenCanvas::wheelEvent(QWheelEvent* event) {
#if 1
    // If the cursor is over the media info overlay, route the scroll to its scroll area
    if (m_infoWidget && m_infoWidget->isVisible() && m_contentScroll) {
        // Map wheel position (relative to this view) to viewport, then to the overlay scroll viewport
        QPointF vpPos = viewport() ? QPointF(viewport()->mapFrom(this, event->position().toPoint())) : event->position();
        if (m_infoWidget->geometry().contains(vpPos.toPoint())) {
            QWidget* dst = m_contentScroll->viewport() ? m_contentScroll->viewport() : static_cast<QWidget*>(m_contentScroll);
            if (dst) {
                const QPoint dstLocal = dst->mapFrom(viewport(), vpPos.toPoint());
                const QPoint globalP = dst->mapToGlobal(dstLocal);
                // Forward the wheel event so QScrollArea handles smooth scrolling
                QWheelEvent forwarded(
                    QPointF(dstLocal),
                    QPointF(globalP),
                    event->pixelDelta(),
                    event->angleDelta(),
                    event->buttons(),
                    event->modifiers(),
                    event->phase(),
                    event->inverted(),
                    event->source()
                );
                QCoreApplication::sendEvent(dst, &forwarded);
                
                // Show scrollbar and restart hide timer on wheel scroll over overlay
                if (m_overlayVScroll && m_scrollbarHideTimer) {
                    m_overlayVScroll->show();
                    m_scrollbarHideTimer->start();
                }
            }
            event->accept();
            return; // Block canvas zoom/pan when wheel is over the overlay
        }
    }
    
    // Check for settings overlay widgets with scroll areas that should block canvas interaction
    const QList<QGraphicsItem*> hitItems = items(event->position().toPoint());
    for (QGraphicsItem* item : hitItems) {
        if (item->data(0).toString() == QLatin1String("blocking-overlay")) {
            // Found an overlay item - check if it's a proxy widget with a scroll area
            if (auto* proxyWidget = dynamic_cast<QGraphicsProxyWidget*>(item)) {
                if (auto* widget = proxyWidget->widget()) {
                    // Look for a QScrollArea within the overlay widget
                    QScrollArea* scrollArea = widget->findChild<QScrollArea*>();
                    if (scrollArea && scrollArea->isVisible()) {
                        // Forward wheel event to the scroll area's viewport
                        QWidget* dst = scrollArea->viewport() ? scrollArea->viewport() : static_cast<QWidget*>(scrollArea);
                        if (dst) {
                            // Map coordinates from canvas to the scroll area viewport
                            const QPointF scenePos = mapToScene(event->position().toPoint());
                            const QPointF itemPos = item->mapFromScene(scenePos);
                            const QPoint widgetPos = widget->mapFromParent(itemPos.toPoint());
                            const QPoint dstLocal = dst->mapFrom(widget, widgetPos);
                            const QPoint globalP = dst->mapToGlobal(dstLocal);
                            
                            // Forward the wheel event
                            QWheelEvent forwarded(
                                QPointF(dstLocal),
                                QPointF(globalP),
                                event->pixelDelta(),
                                event->angleDelta(),
                                event->buttons(),
                                event->modifiers(),
                                event->phase(),
                                event->inverted(),
                                event->source()
                            );
                            QCoreApplication::sendEvent(dst, &forwarded);
                            
                            // Show scrollbar if it exists and has a hide timer
                            QScrollBar* vScrollBar = scrollArea->findChild<QScrollBar*>("overlayScrollBar");
                            QTimer* hideTimer = scrollArea->findChild<QTimer*>("scrollbarHideTimer");
                            if (vScrollBar && hideTimer) {
                                vScrollBar->show();
                                hideTimer->start();
                            }
                        }
                        event->accept();
                        return; // Block canvas zoom/pan when wheel is over settings overlay
                    }
                }
            }
            // If we found an overlay but no scroll area, still block canvas interaction
            event->accept();
            return;
        }
    }
#endif
#ifdef Q_OS_MACOS
    if (m_nativePinchActive) { event->ignore(); return; }
    const bool zoomModifier = event->modifiers().testFlag(Qt::MetaModifier);
#else
    const bool zoomModifier = event->modifiers().testFlag(Qt::ControlModifier);
#endif
    if (zoomModifier) {
        qreal deltaY = 0.0;
        if (!event->pixelDelta().isNull()) deltaY = event->pixelDelta().y();
        else if (!event->angleDelta().isNull()) deltaY = event->angleDelta().y() / 8.0;
        if (deltaY != 0.0) {
            const qreal factor = std::pow(1.0015, deltaY);
            // Convert from view widget coords to viewport coords for correct anchoring
            QPoint vpPos = viewport() ? viewport()->mapFrom(this, event->position().toPoint()) : event->position().toPoint();
            zoomAroundViewportPos(vpPos, factor);
            // After zooming, re-anchor all absolute overlays and the media info overlay
            relayoutAllMediaOverlays(m_scene);
            layoutInfoOverlay();
            event->accept();
            return;
        }
    }
    QPoint delta;
    if (!event->pixelDelta().isNull()) delta = event->pixelDelta();
    else if (!event->angleDelta().isNull()) delta = event->angleDelta() / 8;
    if (!delta.isNull()) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        relayoutAllMediaOverlays(m_scene);
        layoutInfoOverlay();
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void ScreenCanvas::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    // Keep absolute panels pinned during viewport resizes
    relayoutAllMediaOverlays(m_scene);
    // Fast-path update: adjust overlay height cap in real-time on viewport size changes
    updateInfoOverlayGeometryForViewport();
}

void ScreenCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()) { event->ignore(); return; }
    const QMimeData* mime = event->mimeData();
    if (mime->hasUrls()) { event->acceptProposedAction(); ensureDragPreview(mime); }
    else if (mime->hasImage()) { event->acceptProposedAction(); ensureDragPreview(mime); }
    else event->ignore();
}

void ScreenCanvas::dragMoveEvent(QDragMoveEvent* event) {
    const QMimeData* mime = event->mimeData(); if (!mime) { event->ignore(); return; }
    if (!m_dragPreviewItem) ensureDragPreview(mime);
    QPointF scenePos = mapToScene(event->position().toPoint()); m_dragPreviewLastScenePos = scenePos; updateDragPreviewPos(scenePos);
    if (!m_dragCursorHidden) { viewport()->setCursor(Qt::BlankCursor); m_dragCursorHidden = true; }
    event->acceptProposedAction();
}

void ScreenCanvas::dragLeaveEvent(QDragLeaveEvent* event) {
    clearDragPreview(); if (m_dragCursorHidden) { viewport()->unsetCursor(); m_dragCursorHidden = false; } event->accept();
}

void ScreenCanvas::dropEvent(QDropEvent* event) {
    const QMimeData* mime = event->mimeData(); if (!mime) { event->ignore(); return; }
    QPointF scenePos = mapToScene(event->position().toPoint());
    
    // Clear any existing selection before adding new media
    if (m_scene) {
        m_scene->clearSelection();
    }
    if (mime->hasUrls()) {
        const QList<QUrl> urls = mime->urls();
        for (const QUrl& url : urls) {
            if (url.isLocalFile()) {
                QString localPath = url.toLocalFile();
                if (localPath.isEmpty()) continue;
                QFileInfo fi(localPath);
                const QString suffix = fi.suffix().toLower();
                bool isVideo = suffix == "mp4" || suffix == "mov" || suffix == "m4v" || suffix == "avi" || suffix == "mkv" || suffix == "webm";
                if (isVideo) {
                    // Use default handle sizes similar to previous inline defaults (visual 12, selection 30)
                    auto* v = new ResizableVideoItem(localPath, 12, 30, fi.fileName(), m_videoControlsFadeMs);
                    // Record original file path for later upload manifest collection
                    v->setSourcePath(localPath);
                    // Preserve global canvas media scale (so video size matches screens & images 1:1)
                    v->setInitialScaleFactor(m_scaleFactor);
                    // Use current placeholder base size (constructor default 640x360) to center on drop point
                    const qreal phW = 640.0 * m_scaleFactor;
                    const qreal phH = 360.0 * m_scaleFactor;
                    v->setPos(scenePos - QPointF(phW/2.0, phH/2.0));
                    v->setScale(m_scaleFactor); // adoptBaseSize will keep center when real size arrives
                    // If a drag preview frame was captured for this video, use it immediately as a poster to avoid flicker gap
                    if (m_dragPreviewIsVideo && m_dragPreviewGotFrame && !m_dragPreviewPixmap.isNull()) {
                        QImage poster = m_dragPreviewPixmap.toImage();
                        if (!poster.isNull()) v->setExternalPosterImage(poster);
                    }
                    assignNextZValue(v);
                    m_scene->addItem(v);
                    v->setSelected(true);
                    emit mediaItemAdded(v);
                } else {
                    QPixmap pm(localPath);
                    if (!pm.isNull()) {
                        auto* p = new ResizablePixmapItem(pm, 12, 30, QFileInfo(localPath).fileName());
                        p->setSourcePath(localPath);
                        p->setPos(scenePos - QPointF(pm.width()/2.0 * m_scaleFactor, pm.height()/2.0 * m_scaleFactor));
                        p->setScale(m_scaleFactor);
                        assignNextZValue(p);
                        m_scene->addItem(p);
                        p->setSelected(true);
                        emit mediaItemAdded(p);
                    }
                }
            }
        }
    } else if (mime->hasImage()) {
        QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            QPixmap pm = QPixmap::fromImage(img);
            if (!pm.isNull()) {
                auto* p = new ResizablePixmapItem(pm, 12, 30, QString());
                p->setSourcePath(QString()); // Set sourcePath to empty string for images
                p->setPos(scenePos - QPointF(pm.width()/2.0 * m_scaleFactor, pm.height()/2.0 * m_scaleFactor));
                p->setScale(m_scaleFactor);
                assignNextZValue(p);
                m_scene->addItem(p);
                p->setSelected(true);
                emit mediaItemAdded(p);
            }
        }
    }
    clearDragPreview(); if (m_dragCursorHidden) { viewport()->unsetCursor(); m_dragCursorHidden = false; }
    event->acceptProposedAction();
    refreshInfoOverlay();
}

void ScreenCanvas::ensureDragPreview(const QMimeData* mime) {
    if (!mime) return; if (m_dragPreviewItem) return; m_dragPreviewGotFrame = false; m_dragPreviewIsVideo = false;
    if (mime->hasUrls()) {
        QList<QUrl> urls = mime->urls(); if (!urls.isEmpty() && urls.first().isLocalFile()) {
            QFileInfo fi(urls.first().toLocalFile()); QString suffix = fi.suffix().toLower();
            bool isVideo = suffix == "mp4" || suffix == "mov" || suffix == "m4v" || suffix == "avi" || suffix == "mkv" || suffix == "webm";
            if (isVideo) { m_dragPreviewIsVideo = true; startVideoPreviewProbe(fi.absoluteFilePath()); return; }
            QPixmap pm(fi.absoluteFilePath()); if (!pm.isNull()) { m_dragPreviewPixmap = pm; m_dragPreviewBaseSize = pm.size(); }
        }
    } else if (mime->hasImage()) {
        QImage img = qvariant_cast<QImage>(mime->imageData()); if (!img.isNull()) { m_dragPreviewPixmap = QPixmap::fromImage(img); m_dragPreviewBaseSize = m_dragPreviewPixmap.size(); }
    }
    if (!m_dragPreviewPixmap.isNull()) {
        auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); pmItem->setOpacity(0.0); pmItem->setZValue(5000.0); pmItem->setScale(m_scaleFactor); m_scene->addItem(pmItem); m_dragPreviewItem = pmItem; startDragPreviewFadeIn();
    }
}

void ScreenCanvas::updateDragPreviewPos(const QPointF& scenePos) {
    if (!m_dragPreviewItem) return; QSize baseSize = m_dragPreviewBaseSize; if (baseSize.isEmpty()) baseSize = QSize(400, 240);
    QPointF topLeft = scenePos - QPointF(baseSize.width()/2.0 * m_scaleFactor, baseSize.height()/2.0 * m_scaleFactor);
    m_dragPreviewItem->setPos(topLeft);
}

void ScreenCanvas::clearDragPreview() {
    stopVideoPreviewProbe(); stopDragPreviewFade(); if (m_dragPreviewItem) { m_scene->removeItem(m_dragPreviewItem); delete m_dragPreviewItem; m_dragPreviewItem = nullptr; }
    m_dragPreviewPixmap = QPixmap(); m_dragPreviewGotFrame = false; m_dragPreviewIsVideo = false;
}

QPixmap ScreenCanvas::makeVideoPlaceholderPixmap(const QSize& pxSize) {
    QPixmap pm(pxSize); pm.fill(Qt::transparent); QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true); QRect r(0,0,pxSize.width()-1, pxSize.height()-1); p.setBrush(QColor(40,40,40,220)); p.setPen(Qt::NoPen); p.drawRoundedRect(r, 16,16); QPolygon play; play << QPoint(pxSize.width()/2 - 18, pxSize.height()/2 - 24) << QPoint(pxSize.width()/2 - 18, pxSize.height()/2 + 24) << QPoint(pxSize.width()/2 + 26, pxSize.height()/2); p.setBrush(QColor(255,255,255,200)); p.drawPolygon(play); return pm;
}

void ScreenCanvas::startVideoPreviewProbe(const QString& localFilePath) {
#ifdef Q_OS_MACOS
    // Fast macOS thumbnail path (if available) could be wired here later; currently fallback.
#endif
    startVideoPreviewProbeFallback(localFilePath);
}

void ScreenCanvas::startVideoPreviewProbeFallback(const QString& localFilePath) {
    if (m_dragPreviewPlayer) return; m_dragPreviewPlayer = new QMediaPlayer(this); m_dragPreviewAudio = new QAudioOutput(this); m_dragPreviewAudio->setMuted(true); m_dragPreviewPlayer->setAudioOutput(m_dragPreviewAudio); m_dragPreviewSink = new QVideoSink(this); m_dragPreviewPlayer->setVideoSink(m_dragPreviewSink); m_dragPreviewPlayer->setSource(QUrl::fromLocalFile(localFilePath));
    connect(m_dragPreviewSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f){ if (m_dragPreviewGotFrame || !f.isValid()) return; QImage img = f.toImage(); if (img.isNull()) return; m_dragPreviewGotFrame = true; QPixmap newPm = QPixmap::fromImage(img); if (newPm.isNull()) return; m_dragPreviewPixmap = newPm; m_dragPreviewBaseSize = newPm.size(); if (!m_dragPreviewItem) { auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); pmItem->setOpacity(0.0); pmItem->setZValue(5000.0); pmItem->setScale(m_scaleFactor); m_scene->addItem(pmItem); m_dragPreviewItem = pmItem; updateDragPreviewPos(m_dragPreviewLastScenePos); startDragPreviewFadeIn(); } else if (auto* pmItem = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) { pmItem->setPixmap(m_dragPreviewPixmap); updateDragPreviewPos(m_dragPreviewLastScenePos); } if (m_dragPreviewPlayer) m_dragPreviewPlayer->pause(); if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; } });
    m_dragPreviewPlayer->play();
}

void ScreenCanvas::stopVideoPreviewProbe() {
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    if (m_dragPreviewPlayer) { m_dragPreviewPlayer->stop(); m_dragPreviewPlayer->deleteLater(); m_dragPreviewPlayer = nullptr; }
    if (m_dragPreviewSink) { m_dragPreviewSink->deleteLater(); m_dragPreviewSink = nullptr; }
    if (m_dragPreviewAudio) { m_dragPreviewAudio->deleteLater(); m_dragPreviewAudio = nullptr; }
}

void ScreenCanvas::startDragPreviewFadeIn() {
    stopDragPreviewFade(); if (!m_dragPreviewItem) return; const qreal target = m_dragPreviewTargetOpacity; if (m_dragPreviewItem->opacity() >= target - 0.001) return; auto* anim = new QVariantAnimation(this); m_dragPreviewFadeAnim = anim; anim->setStartValue(0.0); anim->setEndValue(target); anim->setDuration(m_dragPreviewFadeMs); anim->setEasingCurve(QEasingCurve::OutCubic); connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ if (m_dragPreviewItem) m_dragPreviewItem->setOpacity(v.toReal()); }); connect(anim, &QVariantAnimation::finished, this, [this](){ m_dragPreviewFadeAnim = nullptr; }); anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void ScreenCanvas::stopDragPreviewFade() { if (m_dragPreviewFadeAnim) { m_dragPreviewFadeAnim->stop(); m_dragPreviewFadeAnim = nullptr; } }

void ScreenCanvas::onFastVideoThumbnailReady(const QImage& img) {
    if (img.isNull()) return; if (m_dragPreviewGotFrame) return; m_dragPreviewGotFrame = true; QPixmap pm = QPixmap::fromImage(img); if (pm.isNull()) return; m_dragPreviewPixmap = pm; m_dragPreviewBaseSize = pm.size(); if (!m_dragPreviewItem) { auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); pmItem->setOpacity(0.0); pmItem->setZValue(5000.0); pmItem->setScale(m_scaleFactor); if (m_scene) m_scene->addItem(pmItem); m_dragPreviewItem = pmItem; updateDragPreviewPos(m_dragPreviewLastScenePos); startDragPreviewFadeIn(); } else if (auto* pix = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) { pix->setPixmap(m_dragPreviewPixmap); updateDragPreviewPos(m_dragPreviewLastScenePos); }
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    if (m_dragPreviewPlayer) { m_dragPreviewPlayer->stop(); m_dragPreviewPlayer->deleteLater(); m_dragPreviewPlayer = nullptr; }
    if (m_dragPreviewSink) { m_dragPreviewSink->deleteLater(); m_dragPreviewSink = nullptr; }
    if (m_dragPreviewAudio) { m_dragPreviewAudio->deleteLater(); m_dragPreviewAudio = nullptr; }
}

void ScreenCanvas::createScreenItems() {
    clearScreens();
    if (!m_scene) return;
    const double spacing = static_cast<double>(m_screenSpacingPx);
    QMap<int, QRectF> compactPositions = calculateCompactPositions(1.0, spacing, spacing);
    m_sceneScreenRects.clear();
    for (int i = 0; i < m_screens.size(); ++i) {
        const ScreenInfo& s = m_screens[i];
        QRectF pos = compactPositions.value(i);
        auto* rect = createScreenItem(s, i, pos);
        rect->setZValue(-1000.0);
        m_scene->addItem(rect);
        m_screenItems << rect;
        m_sceneScreenRects.insert(s.id, pos);
    }
    ensureZOrder();
}

QGraphicsRectItem* ScreenCanvas::createScreenItem(const ScreenInfo& screen, int index, const QRectF& position) {
    const int penWidth = m_screenBorderWidthPx;
    QRectF inner = position.adjusted(penWidth/2.0, penWidth/2.0, -penWidth/2.0, -penWidth/2.0);
    QGraphicsRectItem* item = new QGraphicsRectItem(inner);
    if (screen.primary) { item->setBrush(QBrush(QColor(74,144,226,180))); item->setPen(QPen(QColor(74,144,226), penWidth)); }
    else { item->setBrush(QBrush(QColor(80,80,80,180))); item->setPen(QPen(QColor(160,160,160), penWidth)); }
    item->setData(0, index);
    QGraphicsTextItem* label = new QGraphicsTextItem(QString("Screen %1\n%2×%3").arg(index+1).arg(screen.width).arg(screen.height));
    label->setDefaultTextColor(Qt::white);
    QFont f("Arial", m_screenLabelFontPt, QFont::Bold);
    label->setFont(f);
    QRectF labelRect = label->boundingRect(); QRectF screenRect = item->rect(); label->setPos(screenRect.center() - labelRect.center()); label->setParentItem(item);
    return item;
}

QMap<int, QRectF> ScreenCanvas::calculateCompactPositions(double scaleFactor, double hSpacing, double vSpacing) const {
    QMap<int, QRectF> positions; if (m_screens.isEmpty()) return positions; QList<QPair<int, ScreenInfo>> screenPairs; for (int i=0;i<m_screens.size();++i) screenPairs.append(qMakePair(i, m_screens[i]));
    std::sort(screenPairs.begin(), screenPairs.end(), [](const QPair<int, ScreenInfo>& a, const QPair<int, ScreenInfo>& b){ if (qAbs(a.second.y - b.second.y) < 100) return a.second.x < b.second.x; return a.second.y < b.second.y; });
    double currentX = 0, currentY = 0, rowHeight = 0; int lastY = INT_MIN;
    for (const auto& pair : screenPairs) {
        int index = pair.first; const ScreenInfo& screen = pair.second;
        double screenWidth = screen.width * scaleFactor;
        double screenHeight = screen.height * scaleFactor;
        if (lastY != INT_MIN && qAbs(screen.y - lastY) > 100) {
            currentX = 0; currentY += rowHeight + vSpacing; rowHeight = 0;
        }
        QRectF rect(currentX, currentY, screenWidth, screenHeight);
        positions[index] = rect;
        currentX += screenWidth + hSpacing;
        rowHeight = qMax(rowHeight, screenHeight);
        lastY = screen.y;
    }
    return positions; }

QRectF ScreenCanvas::screensBoundingRect() const { QRectF bounds; bool first = true; for (auto* item : m_screenItems) { if (!item) continue; QRectF r = item->sceneBoundingRect(); if (first) { bounds = r; first = false; } else { bounds = bounds.united(r); } } return bounds; }

QPointF ScreenCanvas::mapRemoteCursorToScene(int remoteX, int remoteY) const {
    if (m_screens.isEmpty() || m_sceneScreenRects.isEmpty()) return QPointF();
    const ScreenInfo* containing = nullptr;
    for (const auto &s : m_screens) {
        QRect r(s.x, s.y, s.width, s.height);
        if (r.contains(remoteX, remoteY)) { containing = &s; break; }
    }
    if (!containing) return QPointF();
    auto it = m_sceneScreenRects.find(containing->id);
    if (it == m_sceneScreenRects.end()) return QPointF();
    QRectF sceneRect = it.value();
    QRect remoteRect(containing->x, containing->y, containing->width, containing->height);
    if (remoteRect.width() <= 0 || remoteRect.height() <= 0) return QPointF();
    qreal relX = (remoteX - remoteRect.x()) / static_cast<qreal>(remoteRect.width());
    qreal relY = (remoteY - remoteRect.y()) / static_cast<qreal>(remoteRect.height());
    // Clamp just in case due to rounding
    relX = std::clamp(relX, 0.0, 1.0);
    relY = std::clamp(relY, 0.0, 1.0);
    return QPointF(sceneRect.x() + relX * sceneRect.width(), sceneRect.y() + relY * sceneRect.height());
}

void ScreenCanvas::zoomAroundViewportPos(const QPointF& vpPosF, qreal factor) {
    QPoint vpPos = vpPosF.toPoint(); if (!viewport()->rect().contains(vpPos)) vpPos = viewport()->rect().center(); const QPointF sceneAnchor = mapToScene(vpPos);
    QTransform t = transform(); t.translate(sceneAnchor.x(), sceneAnchor.y()); t.scale(factor, factor); t.translate(-sceneAnchor.x(), -sceneAnchor.y()); setTransform(t);
    if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); for (QGraphicsItem* it : sel) { if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) v->requestOverlayRelayout(); if (auto* b = dynamic_cast<ResizableMediaBase*>(it)) b->requestLabelRelayout(); } }
}

void ScreenCanvas::ensureZOrder() {
    // Ensures overlays or future interactive layers can sit above screens; currently screens use -1000.
}

void ScreenCanvas::debugLogScreenSizes() const {
    if (m_screenItems.size() != m_screens.size()) {
        qDebug() << "Screen/item count mismatch" << m_screenItems.size() << m_screens.size();
    }
    for (int i = 0; i < m_screenItems.size() && i < m_screens.size(); ++i) {
        auto* item = m_screenItems[i]; if (!item) continue;
        const ScreenInfo& si = m_screens[i];
        QRectF r = item->rect(); // local (inner) rect already adjusted for border
        qDebug() << "Screen" << i << "expected" << si.width << "x" << si.height
                 << "scaleFactor" << m_scaleFactor
                 << "itemRect" << r.width() << "x" << r.height()
                 << "sceneBounding" << item->sceneBoundingRect().size();
    }
}

void ScreenCanvas::recreateRemoteCursorItem() {
    if (!m_scene) return;
    if (m_remoteCursorDot) {
        m_scene->removeItem(m_remoteCursorDot);
        delete m_remoteCursorDot;
        m_remoteCursorDot = nullptr;
    }
    const int d = m_remoteCursorDiameterPx;
    const qreal r = d / 2.0;
    m_remoteCursorDot = new QGraphicsEllipseItem(QRectF(-r, -r, d, d));
    m_remoteCursorDot->setBrush(QBrush(m_remoteCursorFill));
    QPen pen(m_remoteCursorBorder);
    pen.setWidthF(m_remoteCursorBorderWidth);
    if (m_remoteCursorFixedSize) {
        pen.setCosmetic(true); // constant width when fixed-size
    } else {
        pen.setCosmetic(false);
    }
    m_remoteCursorDot->setPen(pen);
    m_remoteCursorDot->setZValue(4000.0);
    if (m_remoteCursorFixedSize) {
        m_remoteCursorDot->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    } else {
        m_remoteCursorDot->setFlag(QGraphicsItem::ItemIgnoresTransformations, false);
    }
    m_scene->addItem(m_remoteCursorDot);
}

QList<QRectF> ScreenCanvas::getScreenBorderRects() const {
    QList<QRectF> rects;
    for (const auto* item : m_screenItems) {
        if (item) {
            rects.append(item->sceneBoundingRect());
        }
    }
    return rects;
}

QPointF ScreenCanvas::snapToScreenBorders(const QPointF& scenePos, const QRectF& mediaBounds, bool shiftPressed) const {
    if (!shiftPressed) return scenePos;
    
    const QList<QRectF> screenRects = getScreenBorderRects();
    if (screenRects.isEmpty()) return scenePos;
    
    // Convert snap distance from pixels to scene units
    const QTransform t = transform();
    const qreal snapDistanceScene = m_snapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);
    
    QPointF snappedPos = scenePos;
    
    // For each screen, test snapping to its borders
    for (const QRectF& screenRect : screenRects) {
        // Calculate media edges at the given position
        const qreal mediaLeft = scenePos.x();
        const qreal mediaRight = scenePos.x() + mediaBounds.width();
        const qreal mediaTop = scenePos.y();
        const qreal mediaBottom = scenePos.y() + mediaBounds.height();
        
        // Horizontal snapping
        // Media left edge to screen left edge
        if (std::abs(mediaLeft - screenRect.left()) < snapDistanceScene) {
            snappedPos.setX(screenRect.left());
        }
        // Media right edge to screen right edge
        else if (std::abs(mediaRight - screenRect.right()) < snapDistanceScene) {
            snappedPos.setX(screenRect.right() - mediaBounds.width());
        }
        // Media left edge to screen right edge (adjacent positioning)
        else if (std::abs(mediaLeft - screenRect.right()) < snapDistanceScene) {
            snappedPos.setX(screenRect.right());
        }
        // Media right edge to screen left edge (adjacent positioning)
        else if (std::abs(mediaRight - screenRect.left()) < snapDistanceScene) {
            snappedPos.setX(screenRect.left() - mediaBounds.width());
        }
        
        // Vertical snapping
        // Media top edge to screen top edge
        if (std::abs(mediaTop - screenRect.top()) < snapDistanceScene) {
            snappedPos.setY(screenRect.top());
        }
        // Media bottom edge to screen bottom edge
        else if (std::abs(mediaBottom - screenRect.bottom()) < snapDistanceScene) {
            snappedPos.setY(screenRect.bottom() - mediaBounds.height());
        }
        // Media top edge to screen bottom edge (adjacent positioning)
        else if (std::abs(mediaTop - screenRect.bottom()) < snapDistanceScene) {
            snappedPos.setY(screenRect.bottom());
        }
        // Media bottom edge to screen top edge (adjacent positioning)
        else if (std::abs(mediaBottom - screenRect.top()) < snapDistanceScene) {
            snappedPos.setY(screenRect.top() - mediaBounds.height());
        }
    }
    
    return snappedPos;
}

qreal ScreenCanvas::snapResizeToScreenBorders(qreal currentScale, const QPointF& fixedScenePoint, const QPointF& fixedItemPoint, const QSize& baseSize, bool shiftPressed) const {
    if (!shiftPressed) return currentScale;
    
    const QList<QRectF> screenRects = getScreenBorderRects();
    if (screenRects.isEmpty()) return currentScale;
    
    // Convert snap distance from pixels to scene units
    const QTransform t = transform();
    const qreal snapDistanceScene = m_snapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);
    
    // Calculate current media position based on the fixed corner
    // fixedScenePoint is where the fixed corner is, fixedItemPoint is its item coordinates
    const QPointF mediaTopLeft = fixedScenePoint - currentScale * fixedItemPoint;
    const qreal mediaWidth = currentScale * baseSize.width();
    const qreal mediaHeight = currentScale * baseSize.height();
    
    // Determine which corner is moving based on fixedItemPoint
    const bool fixedIsTopLeft = (fixedItemPoint.x() < baseSize.width() * 0.5 && fixedItemPoint.y() < baseSize.height() * 0.5);
    const bool fixedIsTopRight = (fixedItemPoint.x() > baseSize.width() * 0.5 && fixedItemPoint.y() < baseSize.height() * 0.5);
    const bool fixedIsBottomLeft = (fixedItemPoint.x() < baseSize.width() * 0.5 && fixedItemPoint.y() > baseSize.height() * 0.5);
    const bool fixedIsBottomRight = (fixedItemPoint.x() > baseSize.width() * 0.5 && fixedItemPoint.y() > baseSize.height() * 0.5);
    
    // Determine which edges are moving
    const bool movingRight = fixedIsTopLeft || fixedIsBottomLeft;
    const bool movingDown = fixedIsTopLeft || fixedIsTopRight;
    const bool movingLeft = fixedIsTopRight || fixedIsBottomRight;
    const bool movingUp = fixedIsBottomLeft || fixedIsBottomRight;
    
    qreal bestScale = currentScale;
    qreal minDistance = snapDistanceScene;
    
    for (const QRectF& screenRect : screenRects) {
        // Calculate media edges at current scale
        const qreal mediaLeft = mediaTopLeft.x();
        const qreal mediaRight = mediaTopLeft.x() + mediaWidth;
        const qreal mediaTop = mediaTopLeft.y();
        const qreal mediaBottom = mediaTopLeft.y() + mediaHeight;
        
        // Test snapping based on which corner is moving
        if (movingRight) {
            // Right edge can snap to screen borders
            const qreal distToScreenRight = std::abs(mediaRight - screenRect.right());
            if (distToScreenRight < snapDistanceScene) {
                // Snap media right edge to screen right edge
                const qreal targetWidth = screenRect.right() - mediaLeft;
                const qreal targetScale = targetWidth / baseSize.width();
                if (targetScale > 0.05 && targetScale < 100.0) {
                    return std::clamp<qreal>(targetScale, 0.05, 100.0);
                }
            }
        }
        
        if (movingDown) {
            // Bottom edge can snap to screen borders
            const qreal distToScreenBottom = std::abs(mediaBottom - screenRect.bottom());
            if (distToScreenBottom < snapDistanceScene) {
                // Snap media bottom edge to screen bottom edge
                const qreal targetHeight = screenRect.bottom() - mediaTop;
                const qreal targetScale = targetHeight / baseSize.height();
                if (targetScale > 0.05 && targetScale < 100.0) {
                    return std::clamp<qreal>(targetScale, 0.05, 100.0);
                }
            }
        }
        
        if (movingLeft) {
            // Left edge can snap to screen borders
            const qreal distToScreenLeft = std::abs(mediaLeft - screenRect.left());
            if (distToScreenLeft < snapDistanceScene) {
                // Snap media left edge to screen left edge
                const qreal targetWidth = (mediaLeft + mediaWidth) - screenRect.left();
                const qreal targetScale = targetWidth / baseSize.width();
                if (targetScale > 0.05 && targetScale < 100.0) {
                    return std::clamp<qreal>(targetScale, 0.05, 100.0);
                }
            }
        }
        
        if (movingUp) {
            // Top edge can snap to screen borders
            const qreal distToScreenTop = std::abs(mediaTop - screenRect.top());
            if (distToScreenTop < snapDistanceScene) {
                // Snap media top edge to screen top edge
                const qreal targetHeight = (mediaTop + mediaHeight) - screenRect.top();
                const qreal targetScale = targetHeight / baseSize.height();
                if (targetScale > 0.05 && targetScale < 100.0) {
                    return std::clamp<qreal>(targetScale, 0.05, 100.0);
                }
            }
        }
    }
    
    return std::clamp<qreal>(bestScale, 0.05, 100.0);
}

// Z-order management methods
void ScreenCanvas::assignNextZValue(QGraphicsItem* item) {
    if (!item) return;
    item->setZValue(m_nextMediaZValue);
    m_nextMediaZValue += 1.0;
}

void ScreenCanvas::moveMediaUp(QGraphicsItem* item) {
    if (!item) return;
    
    QList<QGraphicsItem*> mediaItems = getMediaItemsSortedByZ();
    int currentIndex = mediaItems.indexOf(item);
    
    if (currentIndex >= 0 && currentIndex < mediaItems.size() - 1) {
        // Swap Z values with the item above
        QGraphicsItem* itemAbove = mediaItems[currentIndex + 1];
        qreal tempZ = item->zValue();
        item->setZValue(itemAbove->zValue());
        itemAbove->setZValue(tempZ);
    }
}

void ScreenCanvas::moveMediaDown(QGraphicsItem* item) {
    if (!item) return;
    
    QList<QGraphicsItem*> mediaItems = getMediaItemsSortedByZ();
    int currentIndex = mediaItems.indexOf(item);
    
    if (currentIndex > 0) {
        // Swap Z values with the item below
        QGraphicsItem* itemBelow = mediaItems[currentIndex - 1];
        qreal tempZ = item->zValue();
        item->setZValue(itemBelow->zValue());
        itemBelow->setZValue(tempZ);
    }
}

QList<QGraphicsItem*> ScreenCanvas::getMediaItemsSortedByZ() const {
    QList<QGraphicsItem*> mediaItems;
    
    if (!m_scene) return mediaItems;
    
    // Collect all media items (ResizableMediaBase derived items)
    for (QGraphicsItem* item : m_scene->items()) {
        // Check if it's a media item by checking if it has a non-negative Z value in media range
        if (item->zValue() >= 1.0 && item->zValue() < 10000.0) {
            // Additional check: ensure it's actually a media item, not an overlay
            QString dataType = item->data(0).toString();
            if (dataType != "overlay") {
                mediaItems.append(item);
            }
        }
    }
    
    // Sort by Z value (lowest to highest)
    std::sort(mediaItems.begin(), mediaItems.end(), [](QGraphicsItem* a, QGraphicsItem* b) {
        return a->zValue() < b->zValue();
    });
    
    return mediaItems;
}
