// ScreenCanvas implementation (snap guides rendered via dedicated SnapGuideItem)
#include "ScreenCanvas.h"
#include "WebSocketClient.h" // for remote scene start/stop messaging
#include <QTimer>
#include "AppColors.h"
#include "MediaItems.h"
#include "MediaSettingsPanel.h" // for settingsPanel() accessor usage
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsPixmapItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsTextItem>
#include <QApplication>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QProgressBar>
#include <QVariantAnimation>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFileInfo>
#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QStyleHints>
#include <QCursor>
#include <QRandomGenerator>
#include <QSizePolicy>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QRegion>
#include <QJsonObject>
#include <QJsonArray>
#include <cmath>
#include <algorithm>
#include <limits>

// (Trimmed includes to essentials; further pruning can be done if desired.)

// Forward declare SnapGuideItem (full definition just below) and provide a lightweight local ClippedContainer
class SnapGuideItem;
class ClippedContainer : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        setMask(QRegion(rect()));
    }
};

void ScreenCanvas::drawBackground(QPainter* painter, const QRectF& rect) {
    QGraphicsView::drawBackground(painter, rect);
}

void ScreenCanvas::drawForeground(QPainter* painter, const QRectF& rect) {
    QGraphicsView::drawForeground(painter, rect);
}

// Local lightweight ClippedContainer for viewport overlay clipping.

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

// Helper: convert a pixel length (in screen/view px) to item-space length for a given media item
static qreal itemLengthFromPixels(const ResizableMediaBase* item, int px) {
    if (!item || !item->scene() || item->scene()->views().isEmpty()) return px;
    const QGraphicsView* v = item->scene()->views().first();
    QTransform itemToViewport = v->viewportTransform() * item->sceneTransform();
    qreal sx = std::hypot(itemToViewport.m11(), itemToViewport.m21());
    if (sx <= 1e-6) return px;
    return px / sx;
}

// Helper: climb parent chain to find the ResizableMediaBase ancestor for any graphics item
static ResizableMediaBase* toMedia(QGraphicsItem* x) {
    while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
    return nullptr;
}
}

// Dedicated item to render snap guides between scene content and overlays.
class SnapGuideItem : public QGraphicsItem {
public:
    explicit SnapGuideItem(ScreenCanvas* view) : m_view(view) {
        setZValue(11999.0); // overlays at 12000+
        setAcceptedMouseButtons(Qt::NoButton);
        setFlag(QGraphicsItem::ItemIsSelectable, false);
        setFlag(QGraphicsItem::ItemIsMovable, false);
    }
    QRectF boundingRect() const override {
        if (m_lines.isEmpty()) return QRectF();
        qreal minX=std::numeric_limits<qreal>::max();
        qreal minY=std::numeric_limits<qreal>::max();
        qreal maxX=std::numeric_limits<qreal>::lowest();
        qreal maxY=std::numeric_limits<qreal>::lowest();
        for (const QLineF& l : m_lines) {
            minX = std::min({minX, l.x1(), l.x2()});
            minY = std::min({minY, l.y1(), l.y2()});
            maxX = std::max({maxX, l.x1(), l.x2()});
            maxY = std::max({maxY, l.y1(), l.y2()});
        }
        return QRectF(QPointF(minX, minY), QPointF(maxX, maxY)).adjusted(-2,-2,2,2);
    }
    void setLines(const QVector<QLineF>& lines) { prepareGeometryChange(); m_lines = lines; }
    void clearLines() { if (m_lines.isEmpty()) return; prepareGeometryChange(); m_lines.clear(); }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        if (!m_view || m_lines.isEmpty()) return;
        painter->save();
        // Pixel invariant: draw in viewport pixels
        QGraphicsView* gv = m_view;
        const QTransform sceneToViewport = gv->viewportTransform();
        painter->resetTransform();
        qreal thickness = std::max<qreal>(0.1, AppColors::gSnapIndicatorLineThickness);
        qreal gapPx = std::max<qreal>(1.0, AppColors::gSnapIndicatorDashGap);
        qreal dashLenPx = std::clamp(gapPx * 0.9, thickness * 2.0, gapPx * 2.5);
        qreal period = dashLenPx + gapPx;
        QPen pen(AppColors::gSnapIndicatorColor);
        pen.setWidthF(thickness);
        pen.setCapStyle(Qt::FlatCap);
        painter->setPen(pen);
        auto alignCoord = [&](qreal v){
            if (std::fabs(std::round(thickness) - thickness) < 0.01 && (static_cast<int>(std::round(thickness)) % 2 == 1)) {
                return std::floor(v) + 0.5;
            }
            return v;
        };
        for (const QLineF& sl : m_lines) {
            QPointF v1 = sceneToViewport.map(sl.p1());
            QPointF v2 = sceneToViewport.map(sl.p2());
            qreal dx = v2.x() - v1.x();
            qreal dy = v2.y() - v1.y();
            qreal length = std::hypot(dx, dy);
            if (length < 0.5) continue;
            bool vertical = std::fabs(dx) < std::fabs(dy);
            if (vertical) {
                if (v2.y() < v1.y()) std::swap(v1, v2);
                qreal x = alignCoord(v1.x());
                qreal yStart = v1.y();
                qreal yEnd   = v2.y();
                qreal phaseBase = std::floor(yStart / period) * period;
                for (qreal y = phaseBase; y < yEnd; y += period) {
                    qreal segA = std::max(y, yStart);
                    qreal segB = std::min(y + dashLenPx, yEnd);
                    if (segB - segA > 0.2) painter->drawLine(QPointF(x, segA), QPointF(x, segB));
                }
            } else {
                if (v2.x() < v1.x()) std::swap(v1, v2);
                qreal y = alignCoord(v1.y());
                qreal xStart = v1.x();
                qreal xEnd   = v2.x();
                qreal phaseBase = std::floor(xStart / period) * period;
                for (qreal x = phaseBase; x < xEnd; x += period) {
                    qreal segA = std::max(x, xStart);
                    qreal segB = std::min(x + dashLenPx, xEnd);
                    if (segB - segA > 0.2) painter->drawLine(QPointF(segA, y), QPointF(segB, y));
                }
            }
        }
        painter->restore();
    }
private:
    ScreenCanvas* m_view = nullptr;
    QVector<QLineF> m_lines;
};

// --- Snap indicator API (now that SnapGuideItem is defined) ---
void ScreenCanvas::clearSnapIndicators() {
    if (m_snapGuides) { m_snapGuides->clearLines(); m_snapGuides->update(); }
}

void ScreenCanvas::updateSnapIndicators(const QVector<QLineF>& lines) {
    if (!m_scene) return;
    if (m_snapGuides) { m_snapGuides->setLines(lines); m_snapGuides->update(); }
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

QJsonObject ScreenCanvas::serializeSceneState() const {
    QJsonObject root;
    // Screens
    QJsonArray screensArr;
    for (const ScreenInfo& si : m_screens) {
        screensArr.append(si.toJson());
    }
    root["screens"] = screensArr;
    // Media items
    QJsonArray mediaArr;
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            auto* media = dynamic_cast<ResizableMediaBase*>(gi);
            if (!media) continue;
            QJsonObject m;
            m["mediaId"] = media->mediaId();
            m["fileId"] = media->fileId();
            // Include original filename (no path) to help remote resolve or display placeholder
            if (!media->fileId().isEmpty()) {
                QString p = FileManager::instance().getFilePathForId(media->fileId());
                if (!p.isEmpty()) {
                    QFileInfo fi(p); m["fileName"] = fi.fileName();
                }
            }
            m["type"] = media->isVideoMedia() ? "video" : "image";
            QRectF br = media->sceneBoundingRect();
            m["x"] = br.x();
            m["y"] = br.y();
            m["width"] = br.width();
            m["height"] = br.height();
            m["baseWidth"] = media->baseSizePx().width();
            m["baseHeight"] = media->baseSizePx().height();
            m["visible"] = media->isContentVisible();
            // Compute per-screen spans: for every screen intersecting this media, include a span with normalized geometry
            // relative to that screen. Do NOT clamp; negatives/overflow allow the remote to position and rely on parent clipping.
            int legacyBestScreenId = -1; QRectF legacyBestScreenRect; qreal legacyBestArea = 0.0;
            QJsonArray spans;
            for (auto it = m_sceneScreenRects.constBegin(); it != m_sceneScreenRects.constEnd(); ++it) {
                const int sid = it.key();
                const QRectF srect = it.value();
                const QRectF inter = srect.intersected(br);
                const qreal area = std::max<qreal>(0.0, inter.width() * inter.height());
                if (area <= 0.0 || srect.width() <= 0.0 || srect.height() <= 0.0) continue;
                // Record as a span
                QJsonObject span;
                span["screenId"] = sid;
                span["normX"] = (br.x() - srect.x()) / srect.width();
                span["normY"] = (br.y() - srect.y()) / srect.height();
                span["normW"] = br.width() / srect.width();
                span["normH"] = br.height() / srect.height();
                spans.append(span);
                // Track legacy "best" screen by max intersection area for backward compatibility fields
                if (area > legacyBestArea) { legacyBestArea = area; legacyBestScreenId = sid; legacyBestScreenRect = srect; }
            }
            if (!spans.isEmpty()) {
                m["spans"] = spans;
            }
            // Backward-compatible single-screen fields (used by older remotes); keep best-overlap mapping
            if (legacyBestScreenId != -1 && legacyBestScreenRect.width() > 0.0 && legacyBestScreenRect.height() > 0.0) {
                m["screenId"] = legacyBestScreenId;
                m["normX"] = (br.x() - legacyBestScreenRect.x()) / legacyBestScreenRect.width();
                m["normY"] = (br.y() - legacyBestScreenRect.y()) / legacyBestScreenRect.height();
                m["normW"] = br.width() / legacyBestScreenRect.width();
                m["normH"] = br.height() / legacyBestScreenRect.height();
            }
            if (auto* panel = media->settingsPanel()) {
                m["autoDisplay"] = panel->displayAutomaticallyEnabled();
                m["autoDisplayDelayMs"] = panel->displayDelayMillis();
                if (media->isVideoMedia()) {
                    m["autoPlay"] = panel->playAutomaticallyEnabled();
                    m["autoPlayDelayMs"] = panel->playDelayMillis();
                    if (auto* v = dynamic_cast<ResizableVideoItem*>(media)) {
                        m["muted"] = v->isMuted();
                        m["volume"] = v->volume();
                    }
                }
                m["fadeInSeconds"] = panel->fadeInSeconds();
                m["fadeOutSeconds"] = panel->fadeOutSeconds();
            }
            // Always include base content opacity (user configured). Animation multiplier not serialized.
            m["contentOpacity"] = media->contentOpacity();
            mediaArr.append(m);
        }
    }
    root["media"] = mediaArr;
    return root;
}

void ScreenCanvas::maybeRefreshInfoOverlayOnSceneChanged() {
    if (!m_scene) return;
    
    // Count current media items
    int count = 0;
    for (QGraphicsItem* it : m_scene->items()) {
        if (dynamic_cast<ResizableMediaBase*>(it)) ++count;
    }
    
    // Initialize or update count
    if (m_lastMediaItemCount == -1) { 
        m_lastMediaItemCount = count;
    } else if (count != m_lastMediaItemCount) {
        m_lastMediaItemCount = count;
        refreshInfoOverlay();
        layoutInfoOverlay();
    }
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
        // Remove corner rounding (was using gOverlayCornerRadiusPx) to have sharp edges per latest design
        const QString bg = QString("background-color: transparent; border-radius: 0px; color: white; font-size: 16px;");
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
        m_contentLayout->setContentsMargins(0, 0, 0, 0); // No margins - individual items will have margins
        m_contentLayout->setSpacing(0); // No spacing - individual items will control spacing
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
        
        // Switch header to a vertical stack to host Launch Remote Scene above Upload with separators
        delete headerLayout;
        auto* vHeaderLayout = new QVBoxLayout(m_overlayHeaderWidget);
        vHeaderLayout->setContentsMargins(0, 0, 0, 0);
        vHeaderLayout->setSpacing(0);

        auto createSeparator = [this]() {
            auto* sep = new QLabel(m_overlayHeaderWidget);
            sep->setStyleSheet(QString(
                "QLabel { background-color: %1; border: none; }"
            ).arg(AppColors::colorToCss(AppColors::gOverlayBorderColor)));
            sep->setAutoFillBackground(true);
            sep->setFixedHeight(1);
            sep->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            return sep;
        };

        // Top separator
        vHeaderLayout->addWidget(createSeparator());

        // Launch Remote Scene toggle button
        m_launchSceneButton = new QPushButton("Launch Remote Scene", m_overlayHeaderWidget);
        m_launchSceneButton->setCheckable(true);
        m_launchSceneButton->setStyleSheet(
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
        m_launchSceneButton->setFixedHeight(40);
        m_launchSceneButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        vHeaderLayout->addWidget(m_launchSceneButton);

        // Separator between Launch Remote Scene and Launch Test Scene
        vHeaderLayout->addWidget(createSeparator());

        // Launch Test Scene toggle button
        m_launchTestSceneButton = new QPushButton("Launch Test Scene", m_overlayHeaderWidget);
        m_launchTestSceneButton->setCheckable(true);
        m_launchTestSceneButton->setStyleSheet(
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
        m_launchTestSceneButton->setFixedHeight(40);
        m_launchTestSceneButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        vHeaderLayout->addWidget(m_launchTestSceneButton);

        // Separator between Launch Test Scene and Upload
        vHeaderLayout->addWidget(createSeparator());

        // Upload button (kept as before, with no top border)
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
        vHeaderLayout->addWidget(m_uploadButton);

        // Wire Launch Remote Scene toggle behavior (UI only)
        connect(m_launchSceneButton, &QPushButton::clicked, this, [this]() {
            bool newState = !m_sceneLaunched;
            // If enabling remote scene, disable test scene
            if (newState) {
                if (m_testSceneLaunched) {
                    m_testSceneLaunched = false;
                    if (m_launchTestSceneButton && m_launchTestSceneButton->isCheckable()) {
                        m_launchTestSceneButton->setChecked(false);
                    }
                }
                if (m_launchTestSceneButton) m_launchTestSceneButton->setEnabled(false);
                m_sceneLaunched = true; // set before starting so remote start condition (if reintroduced) sees true
                startHostSceneState();
            } else {
                // Re-enable test scene button when stopping remote scene
                if (m_launchTestSceneButton) m_launchTestSceneButton->setEnabled(true);
                stopHostSceneState();
            }
            m_sceneLaunched = newState; // final authoritative state
            if (m_launchSceneButton->isCheckable()) {
                m_launchSceneButton->setChecked(m_sceneLaunched);
            }
            updateLaunchSceneButtonStyle();
            updateLaunchTestSceneButtonStyle();
        });

        // Wire Launch Test Scene toggle behavior (UI only)
        connect(m_launchTestSceneButton, &QPushButton::clicked, this, [this]() {
            bool newState = !m_testSceneLaunched;
            if (newState) {
                if (m_sceneLaunched) {
                    m_sceneLaunched = false;
                    if (m_launchSceneButton && m_launchSceneButton->isCheckable()) {
                        m_launchSceneButton->setChecked(false);
                    }
                }
                if (m_launchSceneButton) m_launchSceneButton->setEnabled(false);
                startHostSceneState();
            } else {
                if (m_launchSceneButton) m_launchSceneButton->setEnabled(true);
                stopHostSceneState();
            }
            m_testSceneLaunched = newState;
            if (m_launchTestSceneButton->isCheckable()) {
                m_launchTestSceneButton->setChecked(m_testSceneLaunched);
            }
            updateLaunchTestSceneButtonStyle();
            updateLaunchSceneButtonStyle();
        });

        // Initialize Launch Remote Scene style
        updateLaunchSceneButtonStyle();
        
        // Initialize Launch Test Scene style
        updateLaunchTestSceneButtonStyle();

        // Do not add header here; refreshInfoOverlay() will place it at the bottom of the panel
        m_infoWidget->hide(); // hidden until first layout
    }
    
    // Create background rectangle early to prevent visibility issues during window state changes
    if (!m_infoBorderRect && scene()) {
        m_infoBorderRect = new MouseBlockingRoundedRectItem();
        m_infoBorderRect->setRadius(gOverlayCornerRadiusPx);
        applyOverlayBorder(m_infoBorderRect);
        m_infoBorderRect->setBrush(QBrush(AppColors::gOverlayBackgroundColor));
        m_infoBorderRect->setZValue(12009.5);
        m_infoBorderRect->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_infoBorderRect->setData(0, QStringLiteral("overlay"));
        scene()->addItem(m_infoBorderRect);
        m_infoBorderRect->setVisible(false);
    }
    
    // Initial content and layout
    refreshInfoOverlay();
    layoutInfoOverlay();
}

void ScreenCanvas::scheduleInfoOverlayRefresh() {
    if (m_infoRefreshQueued) return;
    m_infoRefreshQueued = true;
    // Perform refresh immediately to prevent flicker
    refreshInfoOverlay();
    layoutInfoOverlay();
    m_infoRefreshQueued = false;
}

void ScreenCanvas::refreshInfoOverlay() {
    if (!m_infoWidget || !m_infoLayout || !m_contentLayout) return;
    // Avoid intermediate paints while rebuilding
    m_infoWidget->setUpdatesEnabled(false);
    m_infoWidget->hide();

    // Clear mapping (sera reconstruit)
    m_mediaContainerByItem.clear();
    m_mediaItemByContainer.clear();
    
    // Reset widget constraints for rebuilding
    m_infoWidget->setMinimumHeight(0);
    m_infoWidget->setMaximumHeight(QWIDGETSIZE_MAX);
    m_infoWidget->setMaximumWidth(QWIDGETSIZE_MAX);
    m_infoWidget->setMinimumWidth(0);
    
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
    
    // Create media item containers to be added to main layout with separators
    QList<QWidget*> mediaContainers;
    
    for (int i = 0; i < media.size(); ++i) {
        ResizableMediaBase* m = media[i];
        QString name = m->displayName();
        QSize sz = m->baseSizePx();
        QString dim = QString::number(sz.width()) + " x " + QString::number(sz.height()) + " px";
        QString sizeStr = QStringLiteral("n/a");
        QString src = m->sourcePath();
        if (!src.isEmpty()) {
            QFileInfo fi(src);
            if (fi.exists() && fi.isFile()) sizeStr = humanSize(fi.size());
        }
        
        // Create a container widget for this media item with content margins
               auto* mediaContainer = new QWidget(m_contentWidget);
               bool isSelected = m && m->isSelected();
               const QString selectedBg = "rgba(255,255,255,0.10)"; // sélection gris clair
               mediaContainer->setAutoFillBackground(true);
               mediaContainer->setAttribute(Qt::WA_TranslucentBackground, false);
               mediaContainer->setStyleSheet(QString("QWidget { background-color: %1; }")
                                       .arg(isSelected ? selectedBg : "transparent"));
               auto* mediaLayoutOuter = new QVBoxLayout(mediaContainer);
               mediaLayoutOuter->setContentsMargins(0,0,0,0); // aucune marge pour que le fond aille jusqu'aux séparateurs
               mediaLayoutOuter->setSpacing(0);
               // Inner content widget that provides horizontal padding
               auto* mediaInner = new QWidget(mediaContainer);
               mediaInner->setAutoFillBackground(false);
               mediaInner->setAttribute(Qt::WA_TranslucentBackground, true);
        auto* mediaLayout = new QVBoxLayout(mediaInner);
        // Ajouter un léger padding vertical interne (ex: 8 px en haut et bas) pour aérer sans créer de "gap" externe
        mediaLayout->setContentsMargins(20,8,20,8); // left, top, right, bottom
        mediaLayout->setSpacing(gMediaListItemSpacing);
        
        // Row: name
        auto* nameLbl = new QLabel(name, mediaContainer);
        nameLbl->setStyleSheet("color: white; background: transparent;");
        nameLbl->setAutoFillBackground(false);
        nameLbl->setAttribute(Qt::WA_TranslucentBackground, true);
        nameLbl->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed); // Allow expanding to show full text
        nameLbl->setWordWrap(false);
        nameLbl->setTextInteractionFlags(Qt::NoTextInteraction);
        nameLbl->setFixedHeight(18); // Exact height to prevent any extra spacing
        nameLbl->setContentsMargins(0, 0, 0, 0); // Force zero margins
        nameLbl->setAlignment(Qt::AlignLeft | Qt::AlignTop); // Align text to top
        nameLbl->setProperty("originalText", name); // Store original text for ellipsis
        mediaLayout->addWidget(nameLbl);
        
        // Row: upload status or progress - fixed height container to prevent flickering
        auto* statusContainer = new QWidget(mediaContainer);
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
        
    mediaLayout->addWidget(statusContainer);
        
        // Row: details smaller under status
        auto* details = new QLabel(dim + QStringLiteral("  ·  ") + sizeStr, mediaContainer);
        details->setStyleSheet("color: " + AppColors::colorToCss(AppColors::gTextSecondary) + "; font-size: 14px; background: transparent;");
        details->setAutoFillBackground(false);
        details->setAttribute(Qt::WA_TranslucentBackground, true);
        details->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed); // Allow expanding to show full text
        details->setWordWrap(false); // Keep dimensions and size on single line
        details->setTextInteractionFlags(Qt::NoTextInteraction);
        details->setFixedHeight(18); // Fixed height to prevent stretching
        details->setProperty("originalText", dim + QStringLiteral("  ·  ") + sizeStr); // Store original text for ellipsis
    mediaLayout->addWidget(details);

    // Add inner content to outer layout
    mediaLayoutOuter->addWidget(mediaInner);
        
        mediaContainers.append(mediaContainer);
    m_mediaContainerByItem.insert(m, mediaContainer);
    m_mediaItemByContainer.insert(mediaContainer, m);
    // Enable mouse interaction on the container
    mediaContainer->setAttribute(Qt::WA_Hover, true); // no custom cursor, keep default arrow
    mediaContainer->installEventFilter(this);
    // Make inner child widgets transparent for mouse so container gets the click
    const auto children = mediaContainer->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* c : children) c->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    
    // Revised layout: no external vertical gaps so selection background touches separator lines.
    // Add separator BEFORE each item except the first; no extra spacings.
    for (int i = 0; i < mediaContainers.size(); ++i) {
        if (i > 0) {
            auto* separator = new QLabel(m_contentWidget);
            separator->setStyleSheet(QString("QLabel { background-color: %1; border: none; }")
                                     .arg(AppColors::colorToCss(AppColors::gOverlayBorderColor)));
            separator->setAutoFillBackground(true);
            separator->setFixedHeight(1);
            separator->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            separator->setTextInteractionFlags(Qt::NoTextInteraction);
            m_contentLayout->addWidget(separator);
        }
        // Ensure internal vertical padding inside the media item instead of external spacers
        if (auto* innerLayout = mediaContainers[i]->findChild<QVBoxLayout*>()) {
            // innerLayout corresponds to mediaLayoutOuter or its child; adjust child that has content margins 20,0,20,0
            // We prefer to locate the inner content widget (mediaInner) and modify its layout
            // but to keep it simple we set contentsMargins on the second (content) layout when found.
        }
        // Add the media container full width
        m_contentLayout->addWidget(mediaContainers[i]);
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
    
    // Compute natural preferred size including header
    const QSize contentHint = m_contentLayout ? m_contentLayout->totalSizeHint() : m_contentWidget->sizeHint();
    const QSize headerHint = m_overlayHeaderWidget ? m_overlayHeaderWidget->sizeHint() : QSize(0,0);
    const int naturalHeight = contentHint.height() + headerHint.height();
    
    // Use consolidated width calculation for consistency
    auto [desiredW, isWidthConstrained] = calculateDesiredWidthAndConstraint();
    const int margin = 16;
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
    
    // Apply final widget dimensions
    m_infoWidget->setFixedSize(desiredW, overlayH);
    m_infoWidget->setMinimumWidth(200);
    
    // Force layout recalculation
    if (m_infoLayout) {
        m_infoLayout->invalidate();
        m_infoLayout->activate();
    }
    
    // Apply ellipsis BEFORE updateGeometry to prevent Qt from rendering unconstrained text
    applyTextEllipsisIfConstrained(isWidthConstrained);
    
    m_infoWidget->updateGeometry();
    
    updateOverlayVScrollVisibilityAndGeometry();
    
    // Only show overlay if there are media items present
    if (!media.isEmpty()) {
        m_infoWidget->show();
        if (m_infoBorderRect) m_infoBorderRect->setVisible(true);
    } else {
        // Hide overlay when no media is present
        m_infoWidget->hide();
        if (m_infoBorderRect) {
            m_infoBorderRect->setVisible(false);
            // Guard against any deferred layout that might resurrect visibility
            QTimer::singleShot(0, this, [this]() {
                if (m_infoBorderRect && (!m_infoWidget || !m_infoWidget->isVisible())) {
                    m_infoBorderRect->setVisible(false);
                }
            });
        }
    }
    
    // Perform final layout and positioning synchronously to prevent flicker
    layoutInfoOverlay();
    
    m_infoWidget->setUpdatesEnabled(true);
}

void ScreenCanvas::layoutInfoOverlay() {
    if (!m_infoWidget || !viewport()) return;
    const int margin = 16;
    const int w = m_infoWidget->width();
    const int x = viewport()->width() - margin - w;
    const int y = viewport()->height() - margin - m_infoWidget->height();
    m_infoWidget->move(std::max(0, x), std::max(0, y));
    
    // Update border rect position and visibility based on widget state
    if (m_infoWidget->isVisible() && m_infoBorderRect) {
        // Immediate positioning to avoid jitter caused by queued updates while panning/zooming
        const int widthNow = w;
        const int heightNow = m_infoWidget->height();
        const QPoint vpPosNow(std::max(0, x), std::max(0, y));
        // map viewport pixel position directly to scene
        const QPointF widgetTopLeftScene = mapToScene(vpPosNow);
        m_infoBorderRect->setRect(0, 0, widthNow, heightNow);
        m_infoBorderRect->setPos(widgetTopLeftScene);
        m_infoBorderRect->setVisible(true);
    } else if (m_infoBorderRect) {
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
    // Use consolidated width calculation to ensure consistency with refreshInfoOverlay
    auto [desiredW, isWidthConstrained] = calculateDesiredWidthAndConstraint();
    
    // Update widget dimensions
    m_infoWidget->setFixedHeight(overlayH);
    m_infoWidget->setFixedWidth(desiredW);
    m_infoWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    
    // Force layout recalculation
    if (m_infoLayout) {
        m_infoLayout->invalidate();
        m_infoLayout->activate();
    }
    
    // Apply ellipsis BEFORE updateGeometry to prevent Qt from rendering unconstrained text
    applyTextEllipsisIfConstrained(isWidthConstrained);
    
    m_infoWidget->updateGeometry();
    
    layoutInfoOverlay();
    updateOverlayVScrollVisibilityAndGeometry();
    
    // Apply ellipsis again immediately to handle any layout-induced changes
    applyTextEllipsisIfConstrained(isWidthConstrained);
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

void ScreenCanvas::applyTextEllipsisIfConstrained(bool isWidthConstrained) {
    if (!m_contentWidget || !m_infoWidget) return;
    
    const int availableTextWidth = m_infoWidget->width() - 40; // 20px margins on each side
    
    for (QLabel* label : m_contentWidget->findChildren<QLabel*>()) {
        const QVariant originalTextProp = label->property("originalText");
        if (!originalTextProp.isValid()) continue;
        
        const QString originalText = originalTextProp.toString();
        
        if (isWidthConstrained && QFontMetrics(label->font()).horizontalAdvance(originalText) > availableTextWidth) {
            label->setText(QFontMetrics(label->font()).elidedText(originalText, Qt::ElideRight, availableTextWidth));
        } else {
            label->setText(originalText);
        }
    }
}

std::pair<int, bool> ScreenCanvas::calculateDesiredWidthAndConstraint() {
    if (!m_infoWidget || !viewport()) return {200, false};
    
    // Measure content width from original text
    int measuredContentW = 0;
    if (m_contentWidget) {
        for (const QLabel* label : m_contentWidget->findChildren<QLabel*>()) {
            if (label->property("originalText").isValid()) {
                const QString originalText = label->property("originalText").toString();
                const QFontMetrics metrics(label->font());
                measuredContentW = std::max(measuredContentW, metrics.horizontalAdvance(originalText));
            }
        }
    }
    
    // Calculate desired width
    const QSize headerHint = m_overlayHeaderWidget ? m_overlayHeaderWidget->sizeHint() : QSize(0,0);
    const int contentWithMargins = measuredContentW + 40; // 20px margins on each side
    int desiredW = std::max({contentWithMargins, headerHint.width(), m_infoWidget->minimumWidth()});
    
    // Apply 50% viewport constraint
    const int capW = static_cast<int>(viewport()->width() * 0.5);
    const bool isWidthConstrained = (desiredW > capW);
    
    return {std::min(desiredW, capW), isWidthConstrained};
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

    // Create snap guide item (between media and overlays)
    m_snapGuides = new SnapGuideItem(this);
    m_scene->addItem(m_snapGuides);

    // On scene changes, re-anchor, refresh overlay on media count change, and keep selection chrome in sync
    connect(m_scene, &QGraphicsScene::changed, this, [this](const QList<QRectF>&){ layoutInfoOverlay(); maybeRefreshInfoOverlayOnSceneChanged(); updateSelectionChrome(); });
    connect(m_scene, &QGraphicsScene::selectionChanged, this, [this](){ updateSelectionChrome(); });
    
    // Set up screen border snapping callbacks for media items
    ResizableMediaBase::setScreenSnapCallback([this](const QPointF& pos, const QRectF& bounds, bool shift, ResizableMediaBase* item) {
        return snapToMediaAndScreenTargets(pos, bounds, shift, item);
    });

    // Unified resize snap: screens + other media (corner precedence)
    ResizableMediaBase::setResizeSnapCallback([this](qreal scale,
                                                     const QPointF& fixed,
                                                     const QPointF& movingItemPoint,
                                                     const QSize& base,
                                                     bool shift,
                                                     ResizableMediaBase* item) {
        auto r = snapResizeToScreenBorders(scale, fixed, movingItemPoint, base, shift, item);
        return ResizableMediaBase::ResizeSnapFeedback{ r.scale, r.cornerSnapped, r.snappedMovingCornerScene };
    });

    // Initialize global info overlay (top-right)
    initInfoOverlay();
    m_lastOverlayLayoutTimer.start();

    // Refresh overlay when any media upload state changes (coalesce multiple changes)
    ResizableMediaBase::setUploadChangedNotifier([this]() {
        scheduleInfoOverlayRefresh();
    });
}

QPointF ScreenCanvas::snapToMediaAndScreenTargets(const QPointF& scenePos, const QRectF& mediaBounds, bool shiftPressed, ResizableMediaBase* movingItem) const {
    if (!shiftPressed) {
        // Leaving snap mode – clear any existing indicators
        const_cast<ScreenCanvas*>(this)->clearSnapIndicators();
        return scenePos;
    }
    QPointF snapped = scenePos;
    // First apply screen snapping (reuse existing logic)
    snapped = snapToScreenBorders(snapped, mediaBounds, true);

    // Convert pixel snap distances to scene units
    const QTransform t = transform();
    const qreal snapDistanceScene = m_snapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);
    const qreal cornerSnapDistanceScene = m_cornerSnapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);

    // Collect other media items
    QList<QGraphicsItem*> items = getMediaItemsSortedByZ();
    // Represent the prospective moved rect
    QRectF movingRect(snapped, QSizeF(mediaBounds.width(), mediaBounds.height()));

    // Track best edge snap adjustments (prefer smallest delta)
    bool cornerCaptured = false;
    QPointF bestPos = snapped;
    qreal bestCornerErr = std::numeric_limits<qreal>::max();
    bool edgeAdjusted = false;
    // For visual indicators we want to remember which edge(s) aligned and their scene coordinates.
    bool xSnapped = false, ySnapped = false;
    qreal snappedVerticalLineX = 0.0;
    qreal snappedHorizontalLineY = 0.0;

    // Derive moving rect corners AFTER initial screen snap
    auto rectCorners = [](const QRectF& r){ return QVector<QPointF>{ r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() }; };
    auto updateCorners = [&](const QPointF& pos){ return rectCorners(QRectF(pos, movingRect.size())); };

    QVector<QPointF> currentCorners = updateCorners(snapped);

    // --- NEW: consider screen corners for corner snapping (previously only media corners) ---
    const QList<QRectF> screenRects = getScreenBorderRects();
    for (const QRectF& sr : screenRects) {
        QVector<QPointF> screenCorners { sr.topLeft(), sr.topRight(), sr.bottomLeft(), sr.bottomRight() };
        for (const QPointF& sc : screenCorners) {
            for (int i=0; i<currentCorners.size(); ++i) {
                const QPointF& mc = currentCorners[i];
                qreal dx = std::abs(mc.x() - sc.x());
                qreal dy = std::abs(mc.y() - sc.y());
                if (dx < cornerSnapDistanceScene && dy < cornerSnapDistanceScene) {
                    qreal err = std::hypot(dx, dy);
                    if (err < bestCornerErr) {
                        bestCornerErr = err;
                        cornerCaptured = true;
                        QPointF delta = sc - mc;
                        bestPos = snapped + delta;
                        snappedVerticalLineX = sc.x();
                        snappedHorizontalLineY = sc.y();
                    }
                }
            }
        }
    }

    for (QGraphicsItem* gi : items) {
        auto* other = dynamic_cast<ResizableMediaBase*>(gi);
        if (!other || other == movingItem) continue;
        QRectF otherR = other->sceneBoundingRect();

        // Corner snapping between media (priority over edges)
        QVector<QPointF> otherCorners = rectCorners(otherR);
        for (const QPointF& oc : otherCorners) {
            for (int i=0;i<currentCorners.size();++i) {
                const QPointF& mc = currentCorners[i];
                qreal dx = std::abs(mc.x() - oc.x());
                qreal dy = std::abs(mc.y() - oc.y());
                if (dx < cornerSnapDistanceScene && dy < cornerSnapDistanceScene) {
                    qreal err = std::hypot(dx, dy);
                    if (err < bestCornerErr) {
                        bestCornerErr = err;
                        cornerCaptured = true;
                        // Compute translation so mc aligns with oc
                        QPointF delta = oc - mc;
                        bestPos = snapped + delta;
                        snappedVerticalLineX = oc.x();
                        snappedHorizontalLineY = oc.y();
                    }
                }
            }
        }
    }

    if (cornerCaptured) {
        // Re-evaluate after applying the corner snap to detect additional aligned corners (e.g. both top & bottom).
        QPointF finalPos = bestPos;
        QRectF finalRect(finalPos, movingRect.size());
        auto finalCorners = rectCorners(finalRect);

        // Detect full overlap with another media item (identical rect) – in that case show all four borders.
        bool fullOverlap = false;
        QRectF overlapSourceRect; // rect we overlapped (for potential future styling)
    // Full overlap now requires a much stricter tolerance than corner capture to avoid
    // showing inner-snap indicators when the inner item is merely close in size.
    const qreal fullTol = std::min<qreal>(0.75, cornerSnapDistanceScene * 0.15);
        for (QGraphicsItem* gi : items) {
            auto* other = dynamic_cast<ResizableMediaBase*>(gi);
            if (!other || other == movingItem) continue;
            QRectF o = other->sceneBoundingRect();
            if (std::abs(o.left()   - finalRect.left())   < fullTol &&
                std::abs(o.right()  - finalRect.right())  < fullTol &&
                std::abs(o.top()    - finalRect.top())    < fullTol &&
                std::abs(o.bottom() - finalRect.bottom()) < fullTol) {
                fullOverlap = true; overlapSourceRect = o; break;
            }
        }
        if (!fullOverlap) {
            // Also consider screen rects: if we perfectly cover a screen, show all borders too.
            for (const QRectF& sr : screenRects) {
                if (std::abs(sr.left()   - finalRect.left())   < fullTol &&
                    std::abs(sr.right()  - finalRect.right())  < fullTol &&
                    std::abs(sr.top()    - finalRect.top())    < fullTol &&
                    std::abs(sr.bottom() - finalRect.bottom()) < fullTol) { fullOverlap = true; overlapSourceRect = sr; break; }
            }
        }
        if (fullOverlap) {
            QVector<QLineF> fullLines;
            fullLines.append(QLineF(finalRect.left(),  finalRect.top(),    finalRect.right(), finalRect.top()));    // top
            fullLines.append(QLineF(finalRect.left(),  finalRect.bottom(), finalRect.right(), finalRect.bottom())); // bottom
            fullLines.append(QLineF(finalRect.left(),  finalRect.top(),    finalRect.left(),  finalRect.bottom())); // left
            fullLines.append(QLineF(finalRect.right(), finalRect.top(),    finalRect.right(), finalRect.bottom())); // right
            const_cast<ScreenCanvas*>(this)->updateSnapIndicators(fullLines);
            return finalPos;
        }

        // Multi-corner simultaneous snapping:
        // Determine which of the moving item's four corners are snapped (within strict display tolerance)
        // to ANY target corner (screens or other media). Show all unique vertical/horizontal lines for those corners.
        const qreal cornerDisplayTol = std::min<qreal>(0.8, cornerSnapDistanceScene * 0.35);
        bool snappedTL = false, snappedTR = false, snappedBL = false, snappedBR = false;
        // Moving item corner positions (after final placement)
        QPointF tl(finalRect.left(), finalRect.top());
        QPointF tr(finalRect.right(), finalRect.top());
        QPointF bl(finalRect.left(), finalRect.bottom());
        QPointF br(finalRect.right(), finalRect.bottom());
        auto testCornerSet = [&](const QVector<QPointF>& targets){
            for (const QPointF& tc : targets) {
                if (!snappedTL && std::abs(tc.x()-tl.x()) < cornerDisplayTol && std::abs(tc.y()-tl.y()) < cornerDisplayTol) snappedTL = true;
                if (!snappedTR && std::abs(tc.x()-tr.x()) < cornerDisplayTol && std::abs(tc.y()-tr.y()) < cornerDisplayTol) snappedTR = true;
                if (!snappedBL && std::abs(tc.x()-bl.x()) < cornerDisplayTol && std::abs(tc.y()-bl.y()) < cornerDisplayTol) snappedBL = true;
                if (!snappedBR && std::abs(tc.x()-br.x()) < cornerDisplayTol && std::abs(tc.y()-br.y()) < cornerDisplayTol) snappedBR = true;
            }
        };
        // Screen corners
        for (const QRectF& sr : screenRects) {
            testCornerSet({ sr.topLeft(), sr.topRight(), sr.bottomLeft(), sr.bottomRight() });
        }
        // Media corners
        for (QGraphicsItem* gi : items) {
            auto* other = dynamic_cast<ResizableMediaBase*>(gi); if (!other || other == movingItem) continue; QRectF r = other->sceneBoundingRect();
            testCornerSet({ r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight() });
        }
        QVector<qreal> verticalXs; QVector<qreal> horizontalYs;
        if (snappedTL || snappedBL) verticalXs.append(finalRect.left());
        if (snappedTR || snappedBR) verticalXs.append(finalRect.right());
        if (snappedTL || snappedTR) horizontalYs.append(finalRect.top());
        if (snappedBL || snappedBR) horizontalYs.append(finalRect.bottom());
        // Deduplicate
        auto dedupVals = [](QVector<qreal>& v){ std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end(), [](qreal a, qreal b){ return std::abs(a-b) < 0.5; }), v.end()); };
        dedupVals(verticalXs); dedupVals(horizontalYs);
        QVector<QLineF> lines;
        for (qreal x : verticalXs) lines.append(QLineF(x, -1e6, x, 1e6));
        for (qreal y : horizontalYs) lines.append(QLineF(-1e6, y, 1e6, y));
        if (!lines.isEmpty()) const_cast<ScreenCanvas*>(this)->updateSnapIndicators(lines); else const_cast<ScreenCanvas*>(this)->clearSnapIndicators();
        return finalPos; // corner precedence
    }

    // Edge (line) snapping against other media
    // We recompute moving rect each candidate; accumulate smallest adjustment separately on X and Y.
    qreal bestDx = 0.0; qreal bestDxAbs = snapDistanceScene + 1.0;
    qreal bestDy = 0.0; qreal bestDyAbs = snapDistanceScene + 1.0;
    qreal candidateVerticalLineX = 0.0; // line to draw for X snap
    qreal candidateHorizontalLineY = 0.0; // line to draw for Y snap

    // First consider screen edges for edge snapping indicators (the initial screen snap may already have aligned them)
    for (const QRectF& sr : screenRects) {
        QRectF m = QRectF(snapped, movingRect.size());
        auto considerDx = [&](qreal fromEdge, qreal toEdge, qreal indicatorX){ qreal delta = toEdge - fromEdge; qreal absd = std::abs(delta); if (absd < bestDxAbs && absd < snapDistanceScene) { bestDxAbs = absd; bestDx = delta; edgeAdjusted = true; candidateVerticalLineX = indicatorX; } };
        auto considerDy = [&](qreal fromEdge, qreal toEdge, qreal indicatorY){ qreal delta = toEdge - fromEdge; qreal absd = std::abs(delta); if (absd < bestDyAbs && absd < snapDistanceScene) { bestDyAbs = absd; bestDy = delta; edgeAdjusted = true; candidateHorizontalLineY = indicatorY; } };
        considerDx(m.left(),  sr.left(),  sr.left());   // left-left
        considerDx(m.left(),  sr.right(), sr.right());  // left-right adjacency
        considerDx(m.right(), sr.right(), sr.right());  // right-right
        considerDx(m.right(), sr.left(),  sr.left());   // right-left adjacency
        considerDy(m.top(),    sr.top(),    sr.top());    // top-top
        considerDy(m.top(),    sr.bottom(), sr.bottom()); // top-bottom adjacency
        considerDy(m.bottom(), sr.bottom(), sr.bottom()); // bottom-bottom
        considerDy(m.bottom(), sr.top(),    sr.top());    // bottom-top adjacency
    }

    for (QGraphicsItem* gi : items) {
        auto* other = dynamic_cast<ResizableMediaBase*>(gi);
        if (!other || other == movingItem) continue;
        QRectF o = other->sceneBoundingRect();
        QRectF m = QRectF(snapped, movingRect.size());
        // Candidate deltas for X alignment
        auto considerDx = [&](qreal fromEdge, qreal toEdge, qreal indicatorX){ qreal delta = toEdge - fromEdge; qreal absd = std::abs(delta); if (absd < bestDxAbs && absd < snapDistanceScene) { bestDxAbs = absd; bestDx = delta; edgeAdjusted = true; candidateVerticalLineX = indicatorX; } };
        auto considerDy = [&](qreal fromEdge, qreal toEdge, qreal indicatorY){ qreal delta = toEdge - fromEdge; qreal absd = std::abs(delta); if (absd < bestDyAbs && absd < snapDistanceScene) { bestDyAbs = absd; bestDy = delta; edgeAdjusted = true; candidateHorizontalLineY = indicatorY; } };

        // Horizontal possibilities
    considerDx(m.left(),  o.left(),  o.left());   // left-left
    considerDx(m.left(),  o.right(), o.right());  // left-right adjacency
    considerDx(m.right(), o.right(), o.right());  // right-right
    considerDx(m.right(), o.left(),  o.left());   // right-left adjacency

        // Vertical possibilities
        considerDy(m.top(),    o.top(),    o.top());    // top-top
        considerDy(m.top(),    o.bottom(), o.bottom()); // top-bottom adjacency
        considerDy(m.bottom(), o.bottom(), o.bottom()); // bottom-bottom
        considerDy(m.bottom(), o.top(),    o.top());    // bottom-top adjacency
    }

    if (edgeAdjusted) {
        bestPos = QPointF(snapped.x() + bestDx, snapped.y() + bestDy);

        // Re-evaluate final rect to find ALL aligned edges (not just the one used to compute translation)
        QRectF finalRect(bestPos, movingRect.size());
        // Full overlap detection in edge-alignment path (identical rect case where edge logic, not corner, resolved last)
    bool fullOverlap = false; QRectF overlapSourceRect; const qreal fullTol = std::min<qreal>(0.75, snapDistanceScene * 0.15);
        for (QGraphicsItem* gi : items) {
            auto* other = dynamic_cast<ResizableMediaBase*>(gi); if (!other || other == movingItem) continue;
            QRectF o = other->sceneBoundingRect();
            if (std::abs(o.left()   - finalRect.left())   < fullTol &&
                std::abs(o.right()  - finalRect.right())  < fullTol &&
                std::abs(o.top()    - finalRect.top())    < fullTol &&
                std::abs(o.bottom() - finalRect.bottom()) < fullTol) { fullOverlap = true; overlapSourceRect = o; break; }
        }
        if (!fullOverlap) {
            for (const QRectF& sr : screenRects) {
                if (std::abs(sr.left()   - finalRect.left())   < fullTol &&
                    std::abs(sr.right()  - finalRect.right())  < fullTol &&
                    std::abs(sr.top()    - finalRect.top())    < fullTol &&
                    std::abs(sr.bottom() - finalRect.bottom()) < fullTol) { fullOverlap = true; overlapSourceRect = sr; break; }
            }
        }
        if (fullOverlap) {
            QVector<QLineF> fullLines;
            fullLines.append(QLineF(finalRect.left(),  finalRect.top(),    finalRect.right(), finalRect.top()));    // top
            fullLines.append(QLineF(finalRect.left(),  finalRect.bottom(), finalRect.right(), finalRect.bottom())); // bottom
            fullLines.append(QLineF(finalRect.left(),  finalRect.top(),    finalRect.left(),  finalRect.bottom())); // left
            fullLines.append(QLineF(finalRect.right(), finalRect.top(),    finalRect.right(), finalRect.bottom())); // right
            const_cast<ScreenCanvas*>(this)->updateSnapIndicators(fullLines);
            return bestPos;
        }
        const qreal tol = snapDistanceScene * 0.5; // tighter tolerance for displaying multiple guides

        // Helper to accumulate unique coordinates (avoid near-duplicates)
        auto addUnique = [](QVector<qreal>& vec, qreal v){
            for (qreal existing : vec) if (std::abs(existing - v) < 0.5) return; // merge close
            vec.append(v);
        };
        QVector<qreal> verticalXs;   // lines x = const
        QVector<qreal> horizontalYs; // lines y = const

        // Consider screens
        for (const QRectF& sr : screenRects) {
            // Overlapping (alignment) edges
            if (std::abs(finalRect.left() - sr.left()) < tol) addUnique(verticalXs, sr.left());
            if (std::abs(finalRect.left() - sr.right()) < tol) addUnique(verticalXs, sr.right());
            if (std::abs(finalRect.right() - sr.right()) < tol) addUnique(verticalXs, sr.right());
            if (std::abs(finalRect.right() - sr.left()) < tol) addUnique(verticalXs, sr.left());

            if (std::abs(finalRect.top() - sr.top()) < tol) addUnique(horizontalYs, sr.top());
            if (std::abs(finalRect.top() - sr.bottom()) < tol) addUnique(horizontalYs, sr.bottom());
            if (std::abs(finalRect.bottom() - sr.bottom()) < tol) addUnique(horizontalYs, sr.bottom());
            if (std::abs(finalRect.bottom() - sr.top()) < tol) addUnique(horizontalYs, sr.top());
        }
        // Consider other media
        for (QGraphicsItem* gi : items) {
            auto* other = dynamic_cast<ResizableMediaBase*>(gi);
            if (!other || other == movingItem) continue;
            QRectF o = other->sceneBoundingRect();
            if (std::abs(finalRect.left() - o.left()) < tol) addUnique(verticalXs, o.left());
            if (std::abs(finalRect.left() - o.right()) < tol) addUnique(verticalXs, o.right());
            if (std::abs(finalRect.right() - o.right()) < tol) addUnique(verticalXs, o.right());
            if (std::abs(finalRect.right() - o.left()) < tol) addUnique(verticalXs, o.left());

            if (std::abs(finalRect.top() - o.top()) < tol) addUnique(horizontalYs, o.top());
            if (std::abs(finalRect.top() - o.bottom()) < tol) addUnique(horizontalYs, o.bottom());
            if (std::abs(finalRect.bottom() - o.bottom()) < tol) addUnique(horizontalYs, o.bottom());
            if (std::abs(finalRect.bottom() - o.top()) < tol) addUnique(horizontalYs, o.top());
        }

        // Improved clustering: keep ability to switch between very close lines by choosing the one nearer the active candidate.
        auto buildClusters = [&](QVector<qreal>& vals, qreal& lastPreferred, qreal candidate) {
            QVector<qreal> out; if (vals.isEmpty()) return out;
            std::sort(vals.begin(), vals.end());
            qreal clusterTol = snapDistanceScene * 0.6; // grouping tolerance
            qreal switchTol  = clusterTol * 0.35;       // distance needed to switch to another value in same cluster
            QVector<qreal> bucket;
            auto flushBucket = [&](){
                if (bucket.isEmpty()) return;
                // Pick representative:
                qreal chosen = bucket.first();
                // If previous preferred is inside bucket range, consider keeping it unless user moved towards another enough.
                qreal minVal = bucket.first();
                qreal maxVal = bucket.last();
                auto nearestToCandidate = [&](){
                    qreal best = bucket.first(); qreal bestAbs = std::abs(best - candidate);
                    for (qreal v : bucket) { qreal d = std::abs(v - candidate); if (d < bestAbs) { bestAbs = d; best = v; } }
                    return best;
                };
                if (bucket.size() == 1) {
                    chosen = bucket.first();
                } else {
                    bool lastInside = !std::isnan(lastPreferred) && lastPreferred >= minVal - 1e-6 && lastPreferred <= maxVal + 1e-6;
                    qreal nearest = nearestToCandidate();
                    if (!lastInside) {
                        chosen = nearest;
                    } else {
                        // If candidate pulls far enough from lastPreferred choose nearest, else keep lastPreferred for stability.
                        if (std::abs(nearest - lastPreferred) > switchTol) chosen = nearest; else chosen = lastPreferred;
                    }
                }
                out.append(chosen);
                lastPreferred = chosen; // persist choice
                bucket.clear();
            };
            for (qreal v : vals) {
                if (bucket.isEmpty()) { bucket.append(v); continue; }
                if (std::abs(v - bucket.last()) <= clusterTol) bucket.append(v); else { flushBucket(); bucket.append(v); }
            }
            flushBucket();
            return out;
        };

        QVector<qreal> displayVertical = buildClusters(verticalXs, m_lastSnapVerticalX, candidateVerticalLineX);
        QVector<qreal> displayHorizontal = buildClusters(horizontalYs, m_lastSnapHorizontalY, candidateHorizontalLineY);

        // Enforce that the primary candidate line (actual snapped edge) is reflected in the display set.
        auto enforcePrimary = [](QVector<qreal>& lines, qreal primary){
            if (std::isnan(primary)) return;
            int idx = -1;
            for (int i=0;i<lines.size();++i) {
                if (std::abs(lines[i] - primary) < 0.5) { idx = i; break; }
            }
            if (idx == -1) {
                // Insert primary at front for determinism
                lines.prepend(primary);
            } else if (idx != 0) {
                // Move to front to avoid showing stale previous representative
                qreal v = lines[idx];
                lines.removeAt(idx);
                lines.prepend(v);
            }
        };
        enforcePrimary(displayVertical, candidateVerticalLineX);
        enforcePrimary(displayHorizontal, candidateHorizontalLineY);
        // Update hysteresis anchors so subsequent clustering remains stable around the newly enforced primary.
        if (!displayVertical.isEmpty()) m_lastSnapVerticalX = displayVertical.first();
        if (!displayHorizontal.isEmpty()) m_lastSnapHorizontalY = displayHorizontal.first();

        // Recompute which edges of the moving rect are actually aligned now; discard stale cluster representatives.
        // Only display an indicator if the edge is virtually perfectly aligned (not just within snap capture tolerance).
        // We use a strict display tolerance (<= 0.8 scene units and a fraction of the snap tolerance) to avoid
        // showing lines for near-but-not-equal heights.
        const qreal edgeTol = snapDistanceScene * 0.45; // broader acceptance for switching logic
        const qreal displayTol = std::min<qreal>(0.8, edgeTol * 0.3); // strict visual requirement
        auto alignedEdge = [&](qreal edgeCoord, const QVector<qreal>& raw){
            qreal best = std::numeric_limits<qreal>::max();
            for (qreal v : raw) {
                qreal d = std::abs(v - edgeCoord);
                if (d < best) best = d;
            }
            return best < displayTol; };

        bool topAligned    = alignedEdge(finalRect.top(),    horizontalYs);
        bool bottomAligned = alignedEdge(finalRect.bottom(), horizontalYs);
        bool leftAligned   = alignedEdge(finalRect.left(),   verticalXs);
        bool rightAligned  = alignedEdge(finalRect.right(),  verticalXs);

        QVector<qreal> prunedHorizontal;
        if (topAligned)    prunedHorizontal.append(finalRect.top());
        if (bottomAligned && (!topAligned || std::abs(finalRect.bottom() - finalRect.top()) > 0.5)) prunedHorizontal.append(finalRect.bottom());

        QVector<qreal> prunedVertical;
        if (leftAligned)   prunedVertical.append(finalRect.left());
        if (rightAligned && (!leftAligned || std::abs(finalRect.right() - finalRect.left()) > 0.5)) prunedVertical.append(finalRect.right());

        if (!prunedHorizontal.isEmpty()) displayHorizontal = prunedHorizontal;
        else displayHorizontal.clear();
        if (!prunedVertical.isEmpty()) displayVertical = prunedVertical;
        else displayVertical.clear();

        std::sort(displayVertical.begin(), displayVertical.end());
        std::sort(displayHorizontal.begin(), displayHorizontal.end());

        QVector<QLineF> lines;
        for (qreal x : displayVertical) lines.append(QLineF(x, -1e6, x, 1e6));
        for (qreal y : displayHorizontal) lines.append(QLineF(-1e6, y, 1e6, y));

        if (!lines.isEmpty()) const_cast<ScreenCanvas*>(this)->updateSnapIndicators(lines);
        else const_cast<ScreenCanvas*>(this)->clearSnapIndicators();

        return bestPos;
    }
    // No snap – clear indicators
    const_cast<ScreenCanvas*>(this)->clearSnapIndicators();
    return bestPos;
}

void ScreenCanvas::showEvent(QShowEvent* event) {
    QGraphicsView::showEvent(event);
    // Restore overlay background when window becomes visible (fixes minimize/restore issue)
    if (m_infoBorderRect && m_infoWidget && m_infoWidget->isVisible()) {
        m_infoBorderRect->setVisible(true);
        m_infoBorderRect->setBrush(QBrush(AppColors::gOverlayBackgroundColor));
        QTimer::singleShot(0, [this]() { layoutInfoOverlay(); });
    }
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    createScreenItems();
    // If screens just became non-empty, ensure any previously requested deferred recenter triggers
    if (m_pendingInitialRecenter && !m_screens.isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_pendingInitialRecenter) return; // might have been cleared
            m_pendingInitialRecenter = false;
            // Only recenter if transform is still identity (i.e., user hasn't interacted yet)
            if (qFuzzyCompare(transform().m11(), 1.0) && qFuzzyCompare(transform().m22(), 1.0)) {
                recenterWithMargin(m_pendingInitialRecenterMargin);
            } else {
            }
        });
    }
}

void ScreenCanvas::clearScreens() {
    for (auto* r : m_screenItems) {
        if (r) m_scene->removeItem(r);
        delete r;
    }
    m_screenItems.clear();
    
    // Note: Overlay background persists across screen updates
}

void ScreenCanvas::hideContentPreservingState() {
    if (!m_scene) return;
    // Hide screen items and remote cursor without deleting them
    // This preserves the viewport state (zoom/pan position)
    for (auto* r : m_screenItems) {
        if (r) r->setVisible(false);
    }
    hideRemoteCursor();
    
    // Hide overlays but don't clear them
    if (m_infoWidget) {
        m_infoWidget->setVisible(false);
    }
}

void ScreenCanvas::showContentAfterReconnect() {
    if (!m_scene) return;
    // Show screen items and overlays again
    // Viewport state (zoom/pan) is automatically preserved
    for (auto* r : m_screenItems) {
        if (r) r->setVisible(true);
    }
    
    // Show overlays again
    if (m_infoWidget) {
        m_infoWidget->setVisible(true);
    }
    
    // Refresh overlay content in case it changed during disconnection
    refreshInfoOverlay();
}

void ScreenCanvas::requestDeferredInitialRecenter(int marginPx) {
    m_pendingInitialRecenter = true;
    m_pendingInitialRecenterMargin = marginPx;
    // If screens already exist, trigger immediately next tick.
    if (!m_screens.isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_pendingInitialRecenter) return;
            m_pendingInitialRecenter = false;
            if (qFuzzyCompare(transform().m11(), 1.0) && qFuzzyCompare(transform().m22(), 1.0)) {
                recenterWithMargin(m_pendingInitialRecenterMargin);
            } else {
            }
        });
    } else {
    }
}

void ScreenCanvas::recenterWithMargin(int marginPx) {
    QRectF bounds = screensBoundingRect();
    if (bounds.isNull() || !bounds.isValid()) {
        return;
    }
    const QSize vp = viewport() ? viewport()->size() : size();
    const qreal availW = static_cast<qreal>(vp.width())  - 2.0 * marginPx;
    const qreal availH = static_cast<qreal>(vp.height()) - 2.0 * marginPx;
    if (availW <= 1 || availH <= 1 || bounds.width() <= 0 || bounds.height() <= 0) {
        fitInView(bounds, Qt::KeepAspectRatio);
        centerOn(bounds.center());
        // Ensure overlays are repositioned immediately in this early-exit path as well
        relayoutAllMediaOverlays(m_scene);
        layoutInfoOverlay();
        updateSelectionChrome();
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
    updateSelectionChrome();
    // Also immediately re-layout the media list overlay to avoid a brief misalignment
    // between the QWidget overlay (in viewport coords) and its scene-backed background rect
    // when the view's transform/center changes.
    layoutInfoOverlay();
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

// Create/update high-z selection chrome so borders/handles are always visible above media
void ScreenCanvas::updateSelectionChrome() {
    if (!m_scene) return;
    QSet<ResizableMediaBase*> stillSelected;
    for (QGraphicsItem* it : m_scene->selectedItems()) {
        if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
            stillSelected.insert(media);
            SelectionChrome sc = m_selectionChromeMap.value(media);
            const qreal zBorderWhite = 11998.0; // below overlay (>=12000), above any media (<10000 used)
            const qreal zBorderBlue  = 11999.0;
            const qreal zHandle      = 11999.5;
            auto ensurePath = [&](QGraphicsPathItem*& p, const QColor& color, qreal z, Qt::PenStyle style, qreal dashOffset){
                if (!p) { p = new QGraphicsPathItem(); m_scene->addItem(p); p->setAcceptedMouseButtons(Qt::NoButton); p->setFlag(QGraphicsItem::ItemIgnoresTransformations, false); }
                QPen pen(color); pen.setCosmetic(true); pen.setWidth(1); pen.setStyle(style); if (style == Qt::DashLine) pen.setDashPattern({4,4}); if (dashOffset != 0.0) pen.setDashOffset(dashOffset); pen.setCapStyle(Qt::FlatCap); pen.setJoinStyle(Qt::MiterJoin); p->setPen(pen); p->setBrush(Qt::NoBrush); p->setZValue(z); p->setData(0, QVariant());
            };
            auto ensureHandle = [&](QGraphicsRectItem*& r){ if (!r) { r = new QGraphicsRectItem(); m_scene->addItem(r); r->setAcceptedMouseButtons(Qt::NoButton); r->setFlag(QGraphicsItem::ItemIgnoresTransformations, false);} r->setBrush(Qt::white); r->setPen(QPen(QColor(74,144,226), 0)); r->setZValue(zHandle); r->setData(0, QVariant()); };
            ensurePath(sc.borderWhite, QColor(255,255,255), zBorderWhite, Qt::DashLine, 0.0);
            ensurePath(sc.borderBlue,  QColor(74,144,226), zBorderBlue,  Qt::DashLine, 4.0);
            for (int i=0;i<8;++i) ensureHandle(sc.handles[i]);
            m_selectionChromeMap[media] = sc;
            updateSelectionChromeGeometry(media);
        }
    }
    // Remove chrome for items no longer selected
    QList<ResizableMediaBase*> toRemove;
    for (auto it = m_selectionChromeMap.begin(); it != m_selectionChromeMap.end(); ++it) {
        if (!stillSelected.contains(it.key())) toRemove.append(it.key());
    }
    for (ResizableMediaBase* m : toRemove) clearSelectionChromeFor(m);

    // Mettre à jour le style de surbrillance dans l'overlay sans forcer un rebuild complet
    const QString selectedBg = "rgba(255,255,255,0.10)";
    for (auto it = m_mediaContainerByItem.begin(); it != m_mediaContainerByItem.end(); ++it) {
        ResizableMediaBase* media = it.key(); QWidget* w = it.value(); if (!w) continue;
        bool sel = stillSelected.contains(media);
        w->setAutoFillBackground(true);
        w->setAttribute(Qt::WA_TranslucentBackground, false);
        // Pleine largeur: pas de border-radius pour que le fond touche les séparateurs
        w->setStyleSheet(QString("QWidget { background-color: %1; }")
                         .arg(sel ? selectedBg : "transparent"));
        w->update();
    }
}

void ScreenCanvas::updateSelectionChromeGeometry(ResizableMediaBase* item) {
    if (!item) return; auto it = m_selectionChromeMap.find(item); if (it == m_selectionChromeMap.end()) return; SelectionChrome& sc = it.value();
    // Build rectangle in scene coords matching item base rect exactly (no padding) so border hugs media edges
    QRectF brItem(0,0, item->baseSizePx().width(), item->baseSizePx().height());
    QRectF selRectItem = brItem; // no expansion — avoid visual gap between media and border
    QPainterPath path; path.addRect(selRectItem);
    if (sc.borderWhite) { sc.borderWhite->setPath(item->mapToScene(path)); }
    if (sc.borderBlue)  { sc.borderBlue->setPath(item->mapToScene(path)); }
    // Handle squares at corners in item coords
    const qreal s = itemLengthFromPixels(item, m_mediaHandleVisualSizePx);
    const QPointF tl = selRectItem.topLeft();
    const QPointF tr = QPointF(selRectItem.right(), selRectItem.top());
    const QPointF bl = QPointF(selRectItem.left(), selRectItem.bottom());
    const QPointF br = selRectItem.bottomRight();
    const QPointF topMid    = QPointF(selRectItem.center().x(), selRectItem.top());
    const QPointF bottomMid = QPointF(selRectItem.center().x(), selRectItem.bottom());
    const QPointF leftMid   = QPointF(selRectItem.left(), selRectItem.center().y());
    const QPointF rightMid  = QPointF(selRectItem.right(), selRectItem.center().y());
    auto place = [&](QGraphicsRectItem* r, const QPointF& centerItem){ if (!r) return; QRectF rect(centerItem.x()-s/2.0, centerItem.y()-s/2.0, s, s); QRectF sceneRect = item->mapToScene(rect).boundingRect(); r->setRect(sceneRect); };
    place(sc.handles[0], tl);
    place(sc.handles[1], tr);
    place(sc.handles[2], bl);
    place(sc.handles[3], br);
    place(sc.handles[4], topMid);
    place(sc.handles[5], bottomMid);
    place(sc.handles[6], leftMid);
    place(sc.handles[7], rightMid);
}

void ScreenCanvas::clearSelectionChromeFor(ResizableMediaBase* item) {
    auto it = m_selectionChromeMap.find(item); if (it == m_selectionChromeMap.end()) return; SelectionChrome sc = it.value();
    if (sc.borderWhite) { if (m_scene) m_scene->removeItem(sc.borderWhite); delete sc.borderWhite; }
    if (sc.borderBlue)  { if (m_scene) m_scene->removeItem(sc.borderBlue);  delete sc.borderBlue; }
    for (QGraphicsRectItem*& r : sc.handles) { if (r) { if (m_scene) m_scene->removeItem(r); delete r; r = nullptr; } }
    m_selectionChromeMap.erase(it);
}

void ScreenCanvas::clearAllSelectionChrome() {
    QList<ResizableMediaBase*> keys = m_selectionChromeMap.keys();
    for (ResizableMediaBase* k : keys) clearSelectionChromeFor(k);
}


qreal ScreenCanvas::applyAxisSnapWithHysteresis(ResizableMediaBase* item,
                                      qreal proposedScale,
                                      const QPointF& fixedScenePoint,
                                      const QSize& baseSize,
                                      ResizableMediaBase::Handle activeHandle) const {
    if (!item) return proposedScale;
    using H = ResizableMediaBase::Handle;
    bool isSide = (activeHandle == H::LeftMid || activeHandle == H::RightMid || activeHandle == H::TopMid || activeHandle == H::BottomMid);
    if (!isSide) return proposedScale;
    if (!m_scene) return proposedScale;
    // Global modality: only snap when Shift is currently pressed (mirrors corner snap rule)
    if (!QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) return proposedScale;

    const QList<QRectF> screenRects = getScreenBorderRects();
    if (screenRects.isEmpty()) return proposedScale;

    const QTransform t = transform();
    const qreal snapDistanceScene = m_snapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);
    constexpr qreal releaseFactor = 1.4; // must move 40% farther to release
    const qreal releaseDist = snapDistanceScene * releaseFactor;

    // current half sizes with proposed scale
    const qreal halfW = (baseSize.width() * proposedScale) / 2.0;
    const qreal halfH = (baseSize.height() * proposedScale) / 2.0;

    // Compute moving edge coordinate based on which handle is active; fixedScenePoint is opposite side midpoint.
    auto movingEdgePos = [&]() -> qreal {
        switch (activeHandle) {
            case H::LeftMid:   return fixedScenePoint.x() - 2*halfW; // left = fixedRight - width
            case H::RightMid:  return fixedScenePoint.x() + 2*halfW; // right = fixedLeft + width
            case H::TopMid:    return fixedScenePoint.y() - 2*halfH; // top = fixedBottom - height
            case H::BottomMid: return fixedScenePoint.y() + 2*halfH; // bottom = fixedTop + height
            default: return 0.0;
        }
    }();

    // Gather candidate border positions along movement axis (screens + other media items).
    QList<qreal> targetEdges;
    targetEdges.reserve(screenRects.size() * 2);
    for (const QRectF& sr : screenRects) {
        if (activeHandle == H::LeftMid || activeHandle == H::RightMid) { targetEdges << sr.left() << sr.right(); }
        else if (activeHandle == H::TopMid || activeHandle == H::BottomMid) { targetEdges << sr.top() << sr.bottom(); }
    }
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            auto* other = dynamic_cast<ResizableMediaBase*>(gi);
            if (!other || other == item) continue;
            QRectF r = other->sceneBoundingRect();
            if (activeHandle == H::LeftMid || activeHandle == H::RightMid) { targetEdges << r.left() << r.right(); }
            else if (activeHandle == H::TopMid || activeHandle == H::BottomMid) { targetEdges << r.top() << r.bottom(); }
        }
    }
    // Remove duplicates (optional) to reduce redundant computations
    std::sort(targetEdges.begin(), targetEdges.end());
    targetEdges.erase(std::unique(targetEdges.begin(), targetEdges.end(), [](qreal a, qreal b){ return std::abs(a-b) < 1e-6; }), targetEdges.end());
    if (targetEdges.isEmpty()) return proposedScale;

    bool snapActive = item->isAxisSnapActive();
    H snapHandle = item->axisSnapHandle();
    qreal snapTargetScale = item->axisSnapTargetScale();

    auto computeScaleFor = [&](qreal edgeScenePos) -> qreal {
        if (activeHandle == H::LeftMid || activeHandle == H::RightMid) {
            // width = |fixed - moving|
            qreal desiredHalfWidth = (activeHandle == H::LeftMid)
                ? (fixedScenePoint.x() - edgeScenePos) / 2.0
                : (edgeScenePos - fixedScenePoint.x()) / 2.0;
            if (desiredHalfWidth <= 0) return proposedScale;
            return (desiredHalfWidth * 2.0) / baseSize.width();
        } else {
            qreal desiredHalfHeight = (activeHandle == H::TopMid)
                ? (fixedScenePoint.y() - edgeScenePos) / 2.0
                : (edgeScenePos - fixedScenePoint.y()) / 2.0;
            if (desiredHalfHeight <= 0) return proposedScale;
            return (desiredHalfHeight * 2.0) / baseSize.height();
        }
    };

    // If a snap is already active, evaluate release OR switch-to-closer-edge logic.
    if (snapActive && snapHandle == activeHandle) {
        auto snappedEdgePosForScale = [&](qreal s) -> qreal {
            qreal halfWLocked = (baseSize.width() * s) / 2.0;
            qreal halfHLocked = (baseSize.height() * s) / 2.0;
            switch (activeHandle) {
                case H::LeftMid:   return fixedScenePoint.x() - 2*halfWLocked;
                case H::RightMid:  return fixedScenePoint.x() + 2*halfWLocked;
                case H::TopMid:    return fixedScenePoint.y() - 2*halfHLocked;
                case H::BottomMid: return fixedScenePoint.y() + 2*halfHLocked;
                default: return 0.0;
            }
        };
        qreal snappedEdgePos = snappedEdgePosForScale(snapTargetScale);
        // Distance of current proposed geometry to the locked snap edge.
        qreal distToLocked = std::abs(movingEdgePos - snappedEdgePos);

        // Attempt switch: find a candidate edge meaningfully closer than the locked one.
        qreal bestSwitchDist = std::numeric_limits<qreal>::max();
        qreal bestSwitchScale = snapTargetScale;
        bool haveSwitch = false;
        for (qreal edge : targetEdges) {
            // Skip if this edge corresponds (numerically) to the locked one.
            if (std::abs(edge - snappedEdgePos) < 1e-6) continue;
            qreal candDist = std::abs(movingEdgePos - edge);
            if (candDist > snapDistanceScene) continue; // Only consider within engage zone.
            if (candDist + 0.25 <= distToLocked && candDist < bestSwitchDist) { // 0.25px bias to avoid rapid toggling.
                qreal candScale = computeScaleFor(edge);
                if (candScale > 0.0) {
                    bestSwitchDist = candDist;
                    bestSwitchScale = candScale;
                    haveSwitch = true;
                }
            }
        }

        if (haveSwitch) {
            // Switch directly to the new closer edge without full release cycle.
            item->setAxisSnapActive(true, activeHandle, bestSwitchScale);
            qreal snappedHalfW = (baseSize.width() * bestSwitchScale)/2.0;
            qreal snappedHalfH = (baseSize.height() * bestSwitchScale)/2.0;
            QVector<QLineF> lines;
            switch (activeHandle) {
                case H::LeftMid:   lines.append(QLineF(fixedScenePoint.x() - 2*snappedHalfW, -1e6, fixedScenePoint.x() - 2*snappedHalfW, 1e6)); break;
                case H::RightMid:  lines.append(QLineF(fixedScenePoint.x() + 2*snappedHalfW, -1e6, fixedScenePoint.x() + 2*snappedHalfW, 1e6)); break;
                case H::TopMid:    lines.append(QLineF(-1e6, fixedScenePoint.y() - 2*snappedHalfH, 1e6, fixedScenePoint.y() - 2*snappedHalfH)); break;
                case H::BottomMid: lines.append(QLineF(-1e6, fixedScenePoint.y() + 2*snappedHalfH, 1e6, fixedScenePoint.y() + 2*snappedHalfH)); break;
                default: break;
            }
            const_cast<ScreenCanvas*>(this)->updateSnapIndicators(lines);
            return bestSwitchScale;
        }

        // No switch candidate: decide whether to remain locked or release.
        if (distToLocked <= releaseDist) {
            // Remain snapped: always show the indicator while snap is logically active.
            // (Previously the line was hidden in the hysteresis band between snapDistance and releaseDist
            // which felt like a premature disappearance while still locked.)
            QVector<QLineF> lines;
            qreal snappedHalfW = (baseSize.width() * snapTargetScale)/2.0;
            qreal snappedHalfH = (baseSize.height() * snapTargetScale)/2.0;
            switch (activeHandle) {
                case H::LeftMid:   lines.append(QLineF(fixedScenePoint.x() - 2*snappedHalfW, -1e6, fixedScenePoint.x() - 2*snappedHalfW, 1e6)); break;
                case H::RightMid:  lines.append(QLineF(fixedScenePoint.x() + 2*snappedHalfW, -1e6, fixedScenePoint.x() + 2*snappedHalfW, 1e6)); break;
                case H::TopMid:    lines.append(QLineF(-1e6, fixedScenePoint.y() - 2*snappedHalfH, 1e6, fixedScenePoint.y() - 2*snappedHalfH)); break;
                case H::BottomMid: lines.append(QLineF(-1e6, fixedScenePoint.y() + 2*snappedHalfH, 1e6, fixedScenePoint.y() + 2*snappedHalfH)); break;
                default: break;
            }
            const_cast<ScreenCanvas*>(this)->updateSnapIndicators(lines);
            return snapTargetScale;
        }

        // Release: user moved far enough from locked edge and no closer switch candidate.
        item->setAxisSnapActive(false, H::None, 0.0);
        const_cast<ScreenCanvas*>(this)->clearSnapIndicators();
        // Continue to acquisition logic below (do not early-return) using unsnapped path.
    }

    // Otherwise evaluate for potential new snap engagement (reverted to original directional filtering logic).
    qreal bestDist = snapDistanceScene;
    qreal bestScale = proposedScale;
    const qreal currentScale = item->scale();
    bool growing = proposedScale > currentScale + 1e-9;
    for (qreal edge : targetEdges) {
        if (growing) {
            if (activeHandle == H::RightMid  && edge < movingEdgePos) continue;
            if (activeHandle == H::LeftMid   && edge > movingEdgePos) continue;
            if (activeHandle == H::BottomMid && edge < movingEdgePos) continue;
            if (activeHandle == H::TopMid    && edge > movingEdgePos) continue;
        }
        qreal dist = std::abs(movingEdgePos - edge);
        if (dist < bestDist) {
            qreal targetScale = computeScaleFor(edge);
            if (targetScale > 0.0) {
                bestDist = dist;
                bestScale = targetScale;
            }
        }
    }
    if (bestScale != proposedScale && bestDist < snapDistanceScene) {
        item->setAxisSnapActive(true, activeHandle, bestScale);
        // Visual snapping line for axis resizing
        QVector<QLineF> lines;
        qreal snappedHalfW = (baseSize.width() * bestScale)/2.0;
        qreal snappedHalfH = (baseSize.height() * bestScale)/2.0;
        switch (activeHandle) {
            case H::LeftMid:   lines.append(QLineF(fixedScenePoint.x() - 2*snappedHalfW, -1e6, fixedScenePoint.x() - 2*snappedHalfW, 1e6)); break;
            case H::RightMid:  lines.append(QLineF(fixedScenePoint.x() + 2*snappedHalfW, -1e6, fixedScenePoint.x() + 2*snappedHalfW, 1e6)); break;
            case H::TopMid:    lines.append(QLineF(-1e6, fixedScenePoint.y() - 2*snappedHalfH, 1e6, fixedScenePoint.y() - 2*snappedHalfH)); break;
            case H::BottomMid: lines.append(QLineF(-1e6, fixedScenePoint.y() + 2*snappedHalfH, 1e6, fixedScenePoint.y() + 2*snappedHalfH)); break;
            default: break;
        }
        const_cast<ScreenCanvas*>(this)->updateSnapIndicators(lines);
        return bestScale;
    }
    return proposedScale;
}

ScreenCanvas::CornerAltSnapResult ScreenCanvas::applyCornerAltSnapWithHysteresis(ResizableMediaBase* item,
                                                         ResizableMediaBase::Handle activeHandle,
                                                         const QPointF& fixedScenePoint,
                                                         const QSize& originalBaseSize,
                                                         qreal proposedW,
                                                         qreal proposedH) const {
    CornerAltSnapResult result; result.cornerSnapped = false;
    using H = ResizableMediaBase::Handle;
    bool isCorner = (activeHandle == H::TopLeft || activeHandle == H::TopRight || activeHandle == H::BottomLeft || activeHandle == H::BottomRight);
    if (!isCorner) return result;
    if (!QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) return result; // Shift required for snapping
    if (!m_scene) return result;

    // Convert snap distances to scene units
    const QTransform t = transform();
    const qreal snapDist = m_snapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);
    const qreal cornerZone = m_cornerSnapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);
    constexpr qreal releaseFactor = 1.4;
    const qreal releaseDist = cornerZone * releaseFactor;

    // Derive current moving corner scene point from proposedW/H keeping fixed corner anchored.
    auto movingCornerSceneFor = [&](qreal w, qreal h) -> QPointF {
        // Top-left of new rect given fixed corner and which corner is fixed
        // We know fixedScenePoint corresponds to opposite corner (fixed) recorded at press.
        // Determine orientation relative to proposed width/height.
        // For simplicity compute new rect by placing fixed corner then offsetting.
        if (activeHandle == H::TopLeft) {
            // fixed is BottomRight
            return QPointF(fixedScenePoint.x() - w, fixedScenePoint.y() - h);
        } else if (activeHandle == H::TopRight) {
            // fixed is BottomLeft
            return QPointF(fixedScenePoint.x(), fixedScenePoint.y() - h);
        } else if (activeHandle == H::BottomLeft) {
            // fixed is TopRight
            return QPointF(fixedScenePoint.x() - w, fixedScenePoint.y());
        } else { // BottomRight
            // fixed is TopLeft
            return QPointF(fixedScenePoint.x(), fixedScenePoint.y());
        }
    };
    auto movingCornerPoint = [&](qreal w, qreal h) -> QPointF {
        QPointF tl = movingCornerSceneFor(w, h); // tl
        if (activeHandle == H::TopLeft) return tl; // moving corner is top-left
        if (activeHandle == H::TopRight) return QPointF(tl.x() + w, tl.y());
        if (activeHandle == H::BottomLeft) return QPointF(tl.x(), tl.y() + h);
        return QPointF(tl.x() + w, tl.y() + h);
    };

    QPointF candidate = movingCornerPoint(proposedW, proposedH);

    // Collect potential corner targets (screen corners + other media corners)
    QList<QPointF> targets;
    for (const QRectF& sr : getScreenBorderRects()) {
        targets << sr.topLeft() << sr.topRight() << sr.bottomLeft() << sr.bottomRight();
    }
    for (QGraphicsItem* gi : m_scene->items()) {
        auto* other = dynamic_cast<ResizableMediaBase*>(gi);
        if (!other || other == item) continue;
        QRectF r = other->sceneBoundingRect();
        targets << r.topLeft() << r.topRight() << r.bottomLeft() << r.bottomRight();
    }
    if (targets.isEmpty()) return result;

    struct Cand { qreal err; QPointF target; } best{ std::numeric_limits<qreal>::max(), QPointF() };
    for (const QPointF& tpt : targets) {
        qreal dx = std::abs(candidate.x() - tpt.x());
        qreal dy = std::abs(candidate.y() - tpt.y());
        if (dx > cornerZone || dy > cornerZone) continue; // outside snapping zone
        qreal err = std::hypot(dx, dy);
        if (err < best.err) { best.err = err; best.target = tpt; }
    }
    if (best.err == std::numeric_limits<qreal>::max()) return result; // no candidate

    // Hysteresis: if already corner-snapped (tracked globally via axis flags?), rely on axis system for release.
    // For now simple engage without persistent state: treat like immediate corner snap.
    if (best.err <= cornerZone) {
        // Compute snapped dimensions from fixed corner to snapped target
        qreal snappedW = proposedW;
        qreal snappedH = proposedH;
        if (activeHandle == H::TopLeft) { snappedW = fixedScenePoint.x() - best.target.x(); snappedH = fixedScenePoint.y() - best.target.y(); }
        else if (activeHandle == H::TopRight) { snappedW = best.target.x() - fixedScenePoint.x(); snappedH = fixedScenePoint.y() - best.target.y(); }
        else if (activeHandle == H::BottomLeft) { snappedW = fixedScenePoint.x() - best.target.x(); snappedH = best.target.y() - fixedScenePoint.y(); }
        else { snappedW = best.target.x() - fixedScenePoint.x(); snappedH = best.target.y() - fixedScenePoint.y(); }
        if (snappedW > 0 && snappedH > 0) {
            result.cornerSnapped = true;
            result.snappedW = snappedW;
            result.snappedH = snappedH;
            result.snappedCorner = best.target;
            // Visual: reuse existing updateSnapIndicators with two orthogonal lines
            QVector<QLineF> lines;
            lines.append(QLineF(best.target.x(), -1e6, best.target.x(), 1e6));
            lines.append(QLineF(-1e6, best.target.y(), 1e6, best.target.y()));
            const_cast<ScreenCanvas*>(this)->updateSnapIndicators(lines);
        }
    }
    return result;
}

bool ScreenCanvas::eventFilter(QObject* watched, QEvent* event) {
    // Handle clicks on media container widgets to select the associated scene media item
    if (event->type() == QEvent::MouseButtonPress) {
        if (auto* w = qobject_cast<QWidget*>(watched)) {
            auto it = m_mediaItemByContainer.find(w);
            if (it != m_mediaItemByContainer.end()) {
                if (ResizableMediaBase* media = it.value()) {
                    if (m_scene) {
                        // Clear existing selection unless multi-select with modifier could be added later
                        m_scene->clearSelection();
                        media->setSelected(true);
                        updateSelectionChrome();
                        return true; // consume event
                    }
                }
            }
        }
    } else if (event->type() == QEvent::Enter) {
        if (auto* w = qobject_cast<QWidget*>(watched)) {
            if (m_mediaItemByContainer.contains(w)) {
                // Subtle hover feedback (light background) only if not already selected
                ResizableMediaBase* media = m_mediaItemByContainer.value(w);
                if (media && !media->isSelected()) {
                    w->setStyleSheet("QWidget { background-color: rgba(255,255,255,0.05); }");
                }
            }
        }
    } else if (event->type() == QEvent::Leave) {
        if (auto* w = qobject_cast<QWidget*>(watched)) {
            if (m_mediaItemByContainer.contains(w)) {
                ResizableMediaBase* media = m_mediaItemByContainer.value(w);
                const QString selectedBg = "rgba(255,255,255,0.10)";
                bool sel = media && media->isSelected();
                w->setStyleSheet(QString("QWidget { background-color: %1; }")
                                  .arg(sel ? selectedBg : "transparent"));
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

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
            // Throttle overlay layout during rapid native pinch; rely on updateInfoOverlayGeometryForViewport on resize
            if (m_lastOverlayLayoutTimer.elapsed() > 16) { layoutInfoOverlay(); m_lastOverlayLayoutTimer.restart(); }
            updateSelectionChrome();
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
            updateSelectionChrome();
            event->accept();
            return true;
        }
    }
#endif
    return QGraphicsView::viewportEvent(event);
}

bool ScreenCanvas::gestureEvent(QGestureEvent* event) {
    if (QGesture* g = event->gesture(Qt::PinchGesture)) {
        // Treat pinch as fresh input; cancel momentum ignore state
        if (m_ignorePanMomentum) { m_ignorePanMomentum = false; m_momentumPrimed = false; }
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
    // Any key press is considered fresh input; cancel momentum blocking
    if (m_ignorePanMomentum) { m_ignorePanMomentum = false; m_momentumPrimed = false; }
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
    if (event->key() == Qt::Key_Shift) {
        clearSnapIndicators();
    }
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

void ScreenCanvas::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Shift) {
        clearSnapIndicators();
    }
    QGraphicsView::keyReleaseEvent(event);
}

void ScreenCanvas::mousePressEvent(QMouseEvent* event) {
    if (m_hostSceneActive) {
        // Block selection interactions during host scene state
        event->ignore();
        return;
    }
    // Fresh user interaction cancels any momentum ignore state
    if (m_ignorePanMomentum) { m_ignorePanMomentum = false; m_momentumPrimed = false; }
    // When pressing over the overlay, forward event to the overlay widget ONLY if we're not in the middle of drag/resize/pan operations
    if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        bool anyResizing = false;
        if (m_scene) {
            for (QGraphicsItem* it : m_scene->items()) {
                if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) { if (rp->isActivelyResizing()) { anyResizing = true; break; } }
            }
        }
        const bool dragging = (m_draggingSelected != nullptr);
        const bool panningNow = m_panning;
        const QPoint vpPos = viewport()->mapFrom(this, event->pos());
        if (!dragging && !anyResizing && !panningNow && m_infoWidget->geometry().contains(vpPos)) {
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
    // (Space-to-pan currently disabled; can be re-enabled with dedicated key tracking)
    const bool spaceHeld = false;
    if (event->button() == Qt::LeftButton) {
        // Record selection at press for persistence across drag/release
        m_leftMouseActive = true;
        m_draggingSincePress = false;
        m_pressViewPos = event->pos();
        m_selectionAtPress.clear();
        if (m_scene) {
            for (QGraphicsItem* it : m_scene->selectedItems()) if (auto* m = dynamic_cast<ResizableMediaBase*>(it)) m_selectionAtPress.append(m);
        }
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
    const QList<QGraphicsItem*> hitItems = items(event->pos());
        bool hasOverlay = false; for (QGraphicsItem* hi : hitItems) if (hi->data(0).toString() == QLatin1String("overlay")) { hasOverlay = true; break; }
        if (hasOverlay) {
            // Do not alter media selection on overlay clicks; just pass event to overlay
            QGraphicsView::mousePressEvent(event);
            return;
        }
        // If there is a selected media under the cursor (possibly occluded), prefer dragging it instead of top item
        ResizableMediaBase* selectedUnderCursor = nullptr;
        if (m_scene) {
            const QPointF sceneP = mapToScene(event->pos());
            // Find any selected item whose shape contains the point, regardless of z
            for (QGraphicsItem* it : m_scene->selectedItems()) {
                if (auto* m = dynamic_cast<ResizableMediaBase*>(it)) {
                    if (m->contains(m->mapFromScene(sceneP))) { selectedUnderCursor = m; break; }
                }
            }
        }
        ResizableMediaBase* mediaHit = nullptr; for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (selectedUnderCursor) {
            // Begin manual drag of the already-selected item; do not change selection
            m_draggingSelected = selectedUnderCursor;
            m_dragStartScene = mapToScene(event->pos());
            m_dragItemStartPos = m_draggingSelected->pos();
            event->accept();
            return;
        }
        if (mediaHit) {
            // If we already have a selection and we're dragging/moving, do not steal selection
            bool hadSelection = m_scene && !m_scene->selectedItems().isEmpty();
            if (!hadSelection || !mediaHit->isSelected()) {
                if (m_scene) m_scene->clearSelection();
                mediaHit->setSelected(true);
            }
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) {
                const QPointF itemPos = v->mapFromScene(mapToScene(event->pos()));
                if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; }
            }
            QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(), event->button(), event->buttons(), Qt::NoModifier);
            QGraphicsView::mousePressEvent(&synthetic);
            return;
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
    // Forward double-clicks to overlay when over it ONLY if we're not in the middle of drag/resize/pan operations
    if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        bool anyResizing = false;
        if (m_scene) {
            for (QGraphicsItem* it : m_scene->items()) {
                if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) { if (rp->isActivelyResizing()) { anyResizing = true; break; } }
            }
        }
        const bool dragging = (m_draggingSelected != nullptr);
        const bool panningNow = m_panning;
        const QPoint vpPos = viewport()->mapFrom(this, event->pos());
        if (!dragging && !anyResizing && !panningNow && m_infoWidget->geometry().contains(vpPos)) {
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
            // Prefer the already-selected item under the cursor, even if occluded
            for (QGraphicsItem* it : sel) {
                if (auto* m = dynamic_cast<ResizableMediaBase*>(it)) {
                    if (m->contains(m->mapFromScene(scenePosSel))) {
                        // Consume the double-click so selection is not transferred to a top item
                        event->accept();
                        return;
                    }
                }
            }
        }
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        ResizableMediaBase* mediaHit = nullptr; for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (mediaHit) {
            // Do not steal selection from an already selected media (persistent selection)
            if (scene() && !mediaHit->isSelected()) { scene()->clearSelection(); mediaHit->setSelected(true); }
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) {
                const QPointF itemPos = v->mapFromScene(mapToScene(event->pos()));
                if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; }
            }
            QGraphicsView::mouseDoubleClickEvent(event);
            // Re-assert selection of mediaHit (or keep previous selection)
            if (scene() && !mediaHit->isBeingDeleted()) mediaHit->setSelected(true);
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ScreenCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (m_ignorePanMomentum) { m_ignorePanMomentum = false; m_momentumPrimed = false; }
    // While over overlay, forward moves to overlay widget and block canvas handling ONLY if we're not dragging/resizing/panning
    if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        bool anyResizing = false;
        if (m_scene) {
            for (QGraphicsItem* it : m_scene->items()) {
                if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) { if (rp->isActivelyResizing()) { anyResizing = true; break; } }
            }
        }
        const bool dragging = (m_draggingSelected != nullptr);
        const bool panningNow = m_panning;
        const QPoint vpPos = viewport()->mapFrom(this, event->pos());
        // Also check m_draggingSincePress to prevent overlay interference during any drag operation
        if (!dragging && !m_draggingSincePress && !anyResizing && !panningNow && m_infoWidget->geometry().contains(vpPos)) {
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
        // Priority: if we are manually dragging a previously selected item (even if occluded), move it now
        if (m_draggingSelected) {
            const QPointF sceneNow = mapToScene(event->pos());
            const QPointF delta = sceneNow - m_dragStartScene;
            m_draggingSelected->setPos(m_dragItemStartPos + delta);
            m_draggingSelected->updateOverlayLayout();
            updateSelectionChromeGeometry(m_draggingSelected);
            event->accept();
            return;
        }
        for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) { v->updateDragWithScenePos(mapToScene(event->pos())); event->accept(); return; }
    const QList<QGraphicsItem*> hitItems = items(event->pos()); bool hitMedia = false; for (QGraphicsItem* it : hitItems) if (toMedia(it)) { hitMedia = true; break; } if (hitMedia) { QGraphicsView::mouseMoveEvent(event); return; }
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
    // If left is held and mouse moved beyond a small threshold, mark as drag
    if (m_leftMouseActive && (event->buttons() & Qt::LeftButton)) {
        if ((event->pos() - m_pressViewPos).manhattanLength() > 2) m_draggingSincePress = true;
        if (m_draggingSelected) {
            const QPointF sceneNow = mapToScene(event->pos());
            const QPointF delta = sceneNow - m_dragStartScene;
            m_draggingSelected->setPos(m_dragItemStartPos + delta);
            // Keep overlays and selection chrome in sync while dragging
            m_draggingSelected->updateOverlayLayout();
            updateSelectionChromeGeometry(m_draggingSelected);
            event->accept();
            return;
        }
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ScreenCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (m_ignorePanMomentum) { m_ignorePanMomentum = false; m_momentumPrimed = false; }
    // Forward release to overlay when over it ONLY if we're not dragging/resizing/panning
    if (m_infoWidget && m_infoWidget->isVisible() && viewport()) {
        bool anyResizing = false;
        if (m_scene) {
            for (QGraphicsItem* it : m_scene->items()) {
                if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) { if (rp->isActivelyResizing()) { anyResizing = true; break; } }
            }
        }
        const bool dragging = (m_draggingSelected != nullptr);
        const bool panningNow = m_panning;
        const QPoint vpPos = viewport()->mapFrom(this, event->pos());
        if (!dragging && !anyResizing && !panningNow && m_infoWidget->geometry().contains(vpPos)) {
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
        // Preserve the selection that existed before dispatching to base handler
        // No need to capture prevSelected; if no drag occurred we let base selection stand
        // If we were manually dragging a selected (possibly occluded) item, finish without letting base change selection
        if (m_draggingSelected) {
            m_draggingSelected = nullptr;
            m_leftMouseActive = false; m_draggingSincePress = false; m_selectionAtPress.clear();
            event->accept();
            return;
        }
        QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(), event->button(), event->buttons(), Qt::NoModifier);
        QGraphicsView::mouseReleaseEvent(&synthetic);
        if (m_scene) {
            // Don't restore stale selection - let current selection stand after drag operations
            // The problematic restoration logic was reverting selection to items that were selected
            // at press time, not accounting for selection changes during the press/drag sequence
            updateSelectionChrome();
        }
        m_leftMouseActive = false; m_draggingSincePress = false; m_selectionAtPress.clear();
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
            updateSelectionChrome();
            event->accept();
            return;
        }
    }
    QPoint delta;
    if (!event->pixelDelta().isNull()) delta = event->pixelDelta();
    else if (!event->angleDelta().isNull()) delta = event->angleDelta() / 8;
    if (!delta.isNull()) {
        // Momentum blocking: if we're in an ignore state (just after recenter),
        // drop deltas that are smaller than the previous one; lift the block if a
        // larger delta arrives, which we treat as fresh user input.
        if (m_ignorePanMomentum) {
            const double curMag = std::sqrt(double(delta.x())*delta.x() + double(delta.y())*delta.y());
            if (!m_momentumPrimed) {
                // First delta after recenter primes the baseline and is ignored
                m_lastMomentumMag = curMag;
                m_lastMomentumDelta = delta;
                m_momentumPrimed = true;
                event->accept();
                return;
            } else {
                if (curMag <= m_lastMomentumMag) {
                    // Still decaying momentum: ignore
                    m_lastMomentumMag = curMag; // track decay to tighten the gate
                    m_lastMomentumDelta = delta;
                    event->accept();
                    return;
                }
                // Larger delta => fresh input, lift the block
                m_ignorePanMomentum = false;
                m_momentumPrimed = false;
            }
        }
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
    // Clear any previous UI overlay rects so we don't accumulate duplicates
    for (auto* it : m_uiZoneItems) { if (it && m_scene) m_scene->removeItem(it); delete it; }
    m_uiZoneItems.clear();
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
    // Z-order previously normalized by ensureZOrder(); items now rely on explicit z-values at creation time.

    // Draw per-screen UI zones (new approach superseding global remap logic)
    QColor genericFill(128,128,128,90); // semi-transparent gray for non-taskbar zones
    QColor taskbarFill = AppColors::gSystemTaskbarColor; // configurable taskbar color
    QPen pen(Qt::NoPen);
    for (const auto &screen : m_screens) {
        if (screen.uiZones.isEmpty()) continue;
        auto it = m_sceneScreenRects.find(screen.id);
        if (it == m_sceneScreenRects.end()) continue;
        QRectF sceneRect = it.value();
        for (const auto &zone : screen.uiZones) {
            if (screen.width <=0 || screen.height <=0) continue;
            double sx = zone.x / double(screen.width);
            double sy = zone.y / double(screen.height);
            double sw = zone.width / double(screen.width);
            double sh = zone.height / double(screen.height);
            if (sw <= 0 || sh <= 0) continue;
            QRectF zr(sceneRect.x() + sx * sceneRect.width(),
                      sceneRect.y() + sy * sceneRect.height(),
                      sw * sceneRect.width(),
                      sh * sceneRect.height());
            // Enforce a minimum visual thickness so it can't vanish after scaling
            if (zr.height() < 3.0) {
                qreal delta = 3.0 - zr.height();
                zr.setHeight(3.0);
                // Anchor to bottom if it's a bottom taskbar (heuristic: y near bottom)
                if (sy > 0.5) zr.moveTop(zr.top() - delta);
            }
            auto* rItem = new QGraphicsRectItem(zr);
            // System UI bars (taskbar / dock / menu_bar) share the same configurable color
            if (zone.type.compare("taskbar", Qt::CaseInsensitive) == 0 ||
                zone.type.compare("dock", Qt::CaseInsensitive) == 0 ||
                zone.type.compare("menu_bar", Qt::CaseInsensitive) == 0) {
                rItem->setBrush(QBrush(taskbarFill));
            } else {
                rItem->setBrush(QBrush(genericFill));
            }
            rItem->setPen(pen);
            rItem->setZValue(-500.0);
            rItem->setAcceptedMouseButtons(Qt::NoButton);
            m_scene->addItem(rItem);
            // Keep in same container as legacy system UI items for unified clearing
            m_uiZoneItems.append(rItem);
        }
    }
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

// ensureZOrder removed (previously a placeholder) -- z layering enforced directly where items are created.

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

ScreenCanvas::ResizeSnapResult ScreenCanvas::snapResizeToScreenBorders(qreal currentScale,
                                                                      const QPointF& fixedScenePoint,
                                                                      const QPointF& fixedItemPoint,
                                                                      const QSize& baseSize,
                                                                      bool shiftPressed,
                                                                      ResizableMediaBase* movingItem) const {
    ResizeSnapResult result; result.scale = currentScale; result.cornerSnapped = false;
    if (!shiftPressed) {
        const_cast<ScreenCanvas*>(this)->clearSnapIndicators();
        return result;
    }

    const QList<QRectF> screenRects = getScreenBorderRects();
    const QTransform t = transform();
    const qreal snapDistanceScene = m_snapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);
    const qreal cornerSnapDistanceScene = m_cornerSnapDistancePx / (t.m11() > 1e-6 ? t.m11() : 1.0);

    const QPointF mediaTopLeft = fixedScenePoint - currentScale * fixedItemPoint;
    const qreal mediaWidth = currentScale * baseSize.width();
    const qreal mediaHeight = currentScale * baseSize.height();

    const bool fixedIsTopLeft = (fixedItemPoint.x() < baseSize.width() * 0.5 && fixedItemPoint.y() < baseSize.height() * 0.5);
    const bool fixedIsTopRight = (fixedItemPoint.x() > baseSize.width() * 0.5 && fixedItemPoint.y() < baseSize.height() * 0.5);
    const bool fixedIsBottomLeft = (fixedItemPoint.x() < baseSize.width() * 0.5 && fixedItemPoint.y() > baseSize.height() * 0.5);
    const bool fixedIsBottomRight = (fixedItemPoint.x() > baseSize.width() * 0.5 && fixedItemPoint.y() > baseSize.height() * 0.5);

    const bool movingRight = fixedIsTopLeft || fixedIsBottomLeft;
    const bool movingDown = fixedIsTopLeft || fixedIsTopRight;
    const bool movingLeft = fixedIsTopRight || fixedIsBottomRight;
    const bool movingUp = fixedIsBottomLeft || fixedIsBottomRight;

    // Helper lambdas
    auto movingCornerPoint = [&]() -> QPointF {
        QPointF mediaTL = mediaTopLeft;
        QPointF mediaTR = QPointF(mediaTopLeft.x() + mediaWidth, mediaTopLeft.y());
        QPointF mediaBL = QPointF(mediaTopLeft.x(), mediaTopLeft.y() + mediaHeight);
        QPointF mediaBR = QPointF(mediaTopLeft.x() + mediaWidth, mediaTopLeft.y() + mediaHeight);
        if (fixedIsTopLeft) return mediaBR;
        if (fixedIsTopRight) return mediaBL;
        if (fixedIsBottomLeft) return mediaTR;
        return mediaTL; // fixedIsBottomRight
    }();

    auto recomputeMovingCornerForScale = [&](qreal s) -> QPointF {
        qreal w = s * baseSize.width();
        qreal h = s * baseSize.height();
        QPointF tl = fixedScenePoint - s * fixedItemPoint; // new top-left
        if (fixedIsTopLeft) return QPointF(tl.x() + w, tl.y() + h);
        if (fixedIsTopRight) return QPointF(tl.x(), tl.y() + h);
        if (fixedIsBottomLeft) return QPointF(tl.x() + w, tl.y());
        return tl; // fixedIsBottomRight
    };

    // 1. Corner snapping across screens and other media (precedence)
    struct CornerCandidate { qreal err; qreal scale; QPointF target; };
    CornerCandidate bestCorner { std::numeric_limits<qreal>::max(), currentScale, QPointF() };

    auto considerCornerTarget = [&](const QPointF& sc){
        const QPointF mc = movingCornerPoint;
        const qreal dx = std::abs(mc.x() - sc.x());
        const qreal dy = std::abs(mc.y() - sc.y());
        if (dx >= cornerSnapDistanceScene || dy >= cornerSnapDistanceScene) return; // outside corner zone
        // Derive potential target width/height given which corner is moving
        qreal targetWidth = mediaWidth;
        qreal targetHeight = mediaHeight;
        const qreal mediaLeft = mediaTopLeft.x();
        const qreal mediaRight = mediaTopLeft.x() + mediaWidth;
        const qreal mediaTop = mediaTopLeft.y();
        const qreal mediaBottom = mediaTopLeft.y() + mediaHeight;
        if (fixedIsTopLeft) { targetWidth = sc.x() - mediaLeft; targetHeight = sc.y() - mediaTop; }
        else if (fixedIsTopRight) { targetWidth = mediaRight - sc.x(); targetHeight = sc.y() - mediaTop; }
        else if (fixedIsBottomLeft) { targetWidth = sc.x() - mediaLeft; targetHeight = mediaBottom - sc.y(); }
        else { targetWidth = mediaRight - sc.x(); targetHeight = mediaBottom - sc.y(); }
        if (targetWidth <= 0 || targetHeight <= 0) return;
        const qreal scaleW = targetWidth / baseSize.width();
        const qreal scaleH = targetHeight / baseSize.height();
        qreal candidates[3] = { scaleW, scaleH, currentScale };
        qreal bestC = currentScale;
        qreal bestErr = std::numeric_limits<qreal>::max();
        auto errorFor = [&](qreal s){ QPointF mcs = recomputeMovingCornerForScale(s); return std::hypot(mcs.x() - sc.x(), mcs.y() - sc.y()); };
        for (qreal c : candidates) { if (c <= 0.05 || c >= 100.0) continue; qreal e = errorFor(c); if (e < bestErr) { bestErr = e; bestC = c; } }
        if (std::abs(scaleW - scaleH) / std::max(0.0001, std::max(scaleW, scaleH)) < 0.05) {
            qreal avg = (scaleW + scaleH) * 0.5; qreal eAvg = errorFor(avg); if (eAvg < bestErr) { bestErr = eAvg; bestC = avg; }
        }
        if (bestErr < bestCorner.err) {
            bestCorner.err = bestErr;
            bestCorner.scale = std::clamp<qreal>(bestC, 0.05, 100.0);
            bestCorner.target = sc;
        }
    };

    // Screen corners
    for (const QRectF& sr : screenRects) {
        considerCornerTarget(sr.topLeft());
        considerCornerTarget(sr.topRight());
        considerCornerTarget(sr.bottomLeft());
        considerCornerTarget(sr.bottomRight());
    }
    // Media corners (other items)
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            auto* other = dynamic_cast<ResizableMediaBase*>(gi);
            if (!other || other == movingItem) continue;
            QRectF r = other->sceneBoundingRect();
            considerCornerTarget(r.topLeft());
            considerCornerTarget(r.topRight());
            considerCornerTarget(r.bottomLeft());
            considerCornerTarget(r.bottomRight());
        }
    }
    if (bestCorner.err < std::numeric_limits<qreal>::max()) {
        // Only treat as a corner snap if error is sufficiently small relative to corner zone.
        const qreal cornerAcceptThreshold = cornerSnapDistanceScene * 0.65; // 65% of zone radius
        if (bestCorner.err <= cornerAcceptThreshold) {
            result.scale = bestCorner.scale;
            result.cornerSnapped = true;
            result.snappedMovingCornerScene = bestCorner.target;
            // Visual indicators: two orthogonal lines through snapped corner
            QVector<QLineF> lines;
            lines.append(QLineF(bestCorner.target.x(), -1e6, bestCorner.target.x(), 1e6));
            lines.append(QLineF(-1e6, bestCorner.target.y(), 1e6, bestCorner.target.y()));
            const_cast<ScreenCanvas*>(this)->updateSnapIndicators(lines);
            return result;
        }
        // Otherwise do NOT early-return; allow edge snapping to compete.
    }

    // 2. Edge snapping (only if no corner snap) – evaluate against screens then other media; choose closest scale delta.
    struct EdgeCandidate { qreal dist; qreal scale; enum Orientation { None, Horizontal, Vertical } orient; qreal lineCoord; };
    EdgeCandidate bestEdge { std::numeric_limits<qreal>::max(), currentScale, EdgeCandidate::None, 0.0 };
    auto considerEdgeWidth = [&](qreal targetWidth){
        if (targetWidth <= 0) return; qreal s = targetWidth / baseSize.width(); if (s <= 0.05 || s >= 100.0) return; qreal d = std::abs(s - currentScale);
        if (d < bestEdge.dist) {
            // Compute new top-left X for this scale
            qreal newTLX = fixedScenePoint.x() - s * fixedItemPoint.x();
            qreal movingEdgeX = movingRight ? (newTLX + s * baseSize.width()) : newTLX; // which horizontal edge moves
            bestEdge = { d, s, EdgeCandidate::Horizontal, movingEdgeX };
        }
    };
    auto considerEdgeHeight = [&](qreal targetHeight){
        if (targetHeight <= 0) return; qreal s = targetHeight / baseSize.height(); if (s <= 0.05 || s >= 100.0) return; qreal d = std::abs(s - currentScale);
        if (d < bestEdge.dist) {
            qreal newTLY = fixedScenePoint.y() - s * fixedItemPoint.y();
            qreal movingEdgeY = movingDown ? (newTLY + s * baseSize.height()) : newTLY; // which vertical edge moves
            bestEdge = { d, s, EdgeCandidate::Vertical, movingEdgeY };
        }
    };

    const qreal mediaLeft = mediaTopLeft.x();
    const qreal mediaRight = mediaTopLeft.x() + mediaWidth;
    const qreal mediaTop = mediaTopLeft.y();
    const qreal mediaBottom = mediaTopLeft.y() + mediaHeight;

    auto testRectEdges = [&](const QRectF& r){
        // Horizontal moving edges (include adjacency: right->left / left->right)
        if (movingRight) {
            qreal distSame = std::abs(mediaRight - r.right());
            if (distSame < snapDistanceScene) considerEdgeWidth(r.right() - mediaLeft); // right-right
            qreal distAdj = std::abs(mediaRight - r.left());
            if (distAdj < snapDistanceScene) considerEdgeWidth(r.left() - mediaLeft);  // right-left adjacency
        }
        if (movingLeft) {
            qreal distSame = std::abs(mediaLeft - r.left());
            if (distSame < snapDistanceScene) considerEdgeWidth(mediaRight - r.left()); // left-left
            qreal distAdj = std::abs(mediaLeft - r.right());
            if (distAdj < snapDistanceScene) considerEdgeWidth(mediaRight - r.right()); // left-right adjacency
        }
        // Vertical moving edges (include adjacency: bottom->top / top->bottom)
        if (movingDown) {
            qreal distSame = std::abs(mediaBottom - r.bottom());
            if (distSame < snapDistanceScene) considerEdgeHeight(r.bottom() - mediaTop); // bottom-bottom
            qreal distAdj = std::abs(mediaBottom - r.top());
            if (distAdj < snapDistanceScene) considerEdgeHeight(r.top() - mediaTop);    // bottom-top adjacency
        }
        if (movingUp) {
            qreal distSame = std::abs(mediaTop - r.top());
            if (distSame < snapDistanceScene) considerEdgeHeight(mediaBottom - r.top()); // top-top
            qreal distAdj = std::abs(mediaTop - r.bottom());
            if (distAdj < snapDistanceScene) considerEdgeHeight(mediaBottom - r.bottom()); // top-bottom adjacency
        }
    };

    for (const QRectF& sr : screenRects) testRectEdges(sr);
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            auto* other = dynamic_cast<ResizableMediaBase*>(gi);
            if (!other || other == movingItem) continue;
            testRectEdges(other->sceneBoundingRect());
        }
    }

    if (bestEdge.dist < std::numeric_limits<qreal>::max()) {
        result.scale = std::clamp<qreal>(bestEdge.scale, 0.05, 100.0);
        // Show a single snap line along the snapped edge
        QVector<QLineF> lines;
        if (bestEdge.orient == EdgeCandidate::Horizontal) {
            lines.append(QLineF(bestEdge.lineCoord, -1e6, bestEdge.lineCoord, 1e6));
        } else if (bestEdge.orient == EdgeCandidate::Vertical) {
            lines.append(QLineF(-1e6, bestEdge.lineCoord, 1e6, bestEdge.lineCoord));
        }
        if (!lines.isEmpty()) const_cast<ScreenCanvas*>(this)->updateSnapIndicators(lines);
        else const_cast<ScreenCanvas*>(this)->clearSnapIndicators();
    } else {
        const_cast<ScreenCanvas*>(this)->clearSnapIndicators();
    }
    return result;
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

void ScreenCanvas::updateLaunchSceneButtonStyle() {
    if (!m_launchSceneButton) return;

    // Idle (stopped) style: transparent background, overlay text color
    const QString idleStyle =
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
        "}";

    // Active (launched) style: magenta tint background + magenta text
    const QString activeStyle =
        "QPushButton { "
        "    padding: 8px 0px; "
        "    font-weight: bold; "
        "    font-size: 12px; "
        "    color: " + AppColors::gLaunchRemoteSceneText.name() + "; "
        "    background: " + AppColors::colorToCss(AppColors::gLaunchRemoteSceneBg) + "; "
        "    border: none; "
        "    border-radius: 0px; "
        "} "
        "QPushButton:hover { "
        "    color: " + AppColors::gLaunchRemoteSceneText.name() + "; "
        "    background: " + AppColors::colorToCss(AppColors::gLaunchRemoteSceneHover) + "; "
        "} "
        "QPushButton:pressed { "
        "    color: " + AppColors::gLaunchRemoteSceneText.name() + "; "
        "    background: " + AppColors::colorToCss(AppColors::gLaunchRemoteScenePressed) + "; "
        "}";

    if (m_sceneLaunched) {
        m_launchSceneButton->setText("Stop Remote Scene");
        m_launchSceneButton->setChecked(true);
        m_launchSceneButton->setStyleSheet(activeStyle);
    } else {
        m_launchSceneButton->setText("Launch Remote Scene");
        m_launchSceneButton->setChecked(false);
        m_launchSceneButton->setStyleSheet(idleStyle);
    }
    // Greyed style for disabled state
    if (!m_launchSceneButton->isEnabled()) {
        m_launchSceneButton->setStyleSheet(
            "QPushButton { padding:8px 0px; font-weight:bold; font-size:12px; color: rgba(255,255,255,0.4); background: rgba(255,255,255,0.04); border:none; }" );
    }
    m_launchSceneButton->setFixedHeight(40);
}

void ScreenCanvas::updateLaunchTestSceneButtonStyle() {
    if (!m_launchTestSceneButton) return;
    // Idle (stopped) style: transparent background, overlay text color
    const QString idleStyle =
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
        "}";

    // Active (launched) style: test scene magenta variant
    const QString activeStyle =
        "QPushButton { "
        "    padding: 8px 0px; "
        "    font-weight: bold; "
        "    font-size: 12px; "
        "    color: " + AppColors::gLaunchTestSceneText.name() + "; "
        "    background: " + AppColors::colorToCss(AppColors::gLaunchTestSceneBg) + "; "
        "    border: none; "
        "    border-radius: 0px; "
        "} "
        "QPushButton:hover { "
        "    color: " + AppColors::gLaunchTestSceneText.name() + "; "
        "    background: " + AppColors::colorToCss(AppColors::gLaunchTestSceneHover) + "; "
        "} "
        "QPushButton:pressed { "
        "    color: " + AppColors::gLaunchTestSceneText.name() + "; "
        "    background: " + AppColors::colorToCss(AppColors::gLaunchTestScenePressed) + "; "
        "}";

    if (m_testSceneLaunched) {
        m_launchTestSceneButton->setText("Stop Test Scene");
        m_launchTestSceneButton->setChecked(true);
        m_launchTestSceneButton->setStyleSheet(activeStyle);
    } else {
        m_launchTestSceneButton->setText("Launch Test Scene");
        m_launchTestSceneButton->setChecked(false);
        m_launchTestSceneButton->setStyleSheet(idleStyle);
    }
    if (!m_launchTestSceneButton->isEnabled()) {
        m_launchTestSceneButton->setStyleSheet(
            "QPushButton { padding:8px 0px; font-weight:bold; font-size:12px; color: rgba(255,255,255,0.4); background: rgba(255,255,255,0.04); border:none; }" );
    }
    m_launchTestSceneButton->setFixedHeight(40);

}

// (Removed legacy duplicate snap indicator drawing functions; SnapGuideItem now handles rendering.)

void ScreenCanvas::startHostSceneState() {
    if (m_hostSceneActive) return;
    m_hostSceneActive = true;
    // Capture current selection (only ResizableMediaBase items) before clearing so we can restore later.
    m_prevSelectionBeforeHostScene.clear();
    m_prevVideoStates.clear();
    if (m_scene) {
        const auto selectedNow = m_scene->selectedItems();
        for (QGraphicsItem* it : selectedNow) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                m_prevSelectionBeforeHostScene.append(media);
            }
        }
    }
    // Deselect all media and block further selection by clearing selections.
    if (m_scene) {
        for (QGraphicsItem* it : m_scene->selectedItems()) it->setSelected(false);
    }
    // Hide all media then schedule per-item auto display / playback based on their individual automation settings.
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(gi)) {
                // Decide if it should auto-display immediately
                bool shouldAutoDisplay = false;
                int displayDelayMs = 0;
                bool shouldAutoPlay = false;
                int playDelayMs = 0;
                if (media->settingsPanel()) {
                    shouldAutoDisplay = media->settingsPanel()->displayAutomaticallyEnabled();
                    displayDelayMs = media->settingsPanel()->displayDelayMillis();
                    if (media->isVideoMedia()) {
                        shouldAutoPlay = media->settingsPanel()->playAutomaticallyEnabled();
                        playDelayMs = media->settingsPanel()->playDelayMillis();
                    }
                }
                // Snapshot and reset videos
                if (auto* vid = dynamic_cast<ResizableVideoItem*>(media)) {
                    VideoPreState st; st.video = vid; st.posMs = vid->currentPositionMs(); st.wasPlaying = vid->isPlaying();
                    m_prevVideoStates.append(st);
                    vid->stopToBeginning();
                }
                media->hideImmediateNoFade();
                // 1. Schedule (or immediate) display
                if (shouldAutoDisplay) {
                    if (displayDelayMs > 0) {
                        QTimer::singleShot(displayDelayMs, this, [this, media]() {
                            if (!m_hostSceneActive) return;
                            media->showWithConfiguredFade();
                        });
                    } else {
                        media->showWithConfiguredFade();
                    }
                }
                // 2. Schedule video playback only if BOTH autoDisplay and autoPlay are enabled (play occurs after show completes)
                if (shouldAutoDisplay && shouldAutoPlay && media->isVideoMedia()) {
                    int combinedDelay = displayDelayMs + playDelayMs;
                    QTimer::singleShot(combinedDelay, this, [this, media]() {
                        if (!m_hostSceneActive) return;
                        if (auto* vid = dynamic_cast<ResizableVideoItem*>(media)) {
                            vid->togglePlayPause();
                        }
                    });
                }
            }
        }
    }
    // Dispatch remote scene start (independent of UI toggle race) if a target is set
    if (m_wsClient && !m_remoteSceneTargetClientId.isEmpty()) {
        QJsonObject sceneObj = serializeSceneState();
        qDebug() << "ScreenCanvas: sending remote_scene_start to" << m_remoteSceneTargetClientId
                 << "mediaCount=" << sceneObj.value("media").toArray().size()
                 << "screenCount=" << sceneObj.value("screens").toArray().size();
        m_wsClient->sendRemoteSceneStart(m_remoteSceneTargetClientId, sceneObj);
    }
}

void ScreenCanvas::stopHostSceneState() {
    if (!m_hostSceneActive) return;
    m_hostSceneActive = false;
    if (m_autoDisplayTimer) m_autoDisplayTimer->stop(); // legacy batch timers (may be null if never created)
    if (m_autoPlayTimer) m_autoPlayTimer->stop();
    // Restore visibility of media items (leave videos stopped).
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(gi)) media->showImmediateNoFade();
        }
        // Restore previous selection (only items that still exist in the scene)
        for (auto* media : std::as_const(m_prevSelectionBeforeHostScene)) {
            if (media && media->scene() == m_scene) {
                media->setSelected(true);
            }
        }
        m_prevSelectionBeforeHostScene.clear();
    }
    // Restore pre-scene video positions (always paused)
    for (const VideoPreState& st : std::as_const(m_prevVideoStates)) {
        if (st.video && st.video->scene() == m_scene) {
            st.video->pauseAndSetPosition(st.posMs);
        }
    }
    m_prevVideoStates.clear();

    // Notify remote client to stop scene if applicable (only if we previously launched)
    if (m_wsClient && !m_remoteSceneTargetClientId.isEmpty()) {
        qDebug() << "ScreenCanvas: sending remote_scene_stop to" << m_remoteSceneTargetClientId;
        m_wsClient->sendRemoteSceneStop(m_remoteSceneTargetClientId);
    }
    // Notify remote client scene stop if we were in remote scene mode
    if (m_sceneLaunched && m_wsClient && !m_remoteSceneTargetClientId.isEmpty()) {
        m_wsClient->sendRemoteSceneStop(m_remoteSceneTargetClientId);
    }
}

void ScreenCanvas::scheduleAutoDisplayAndPlayback() {
    if (!m_hostSceneActive) return;
    // Retain legacy batch timer only if some media rely on old path (fallback)
    if (m_autoDisplayTimer) m_autoDisplayTimer->start(1500);
    if (m_autoPlayTimer) m_autoPlayTimer->start(1000);
}

void ScreenCanvas::handleAutoDisplay() {
    // Legacy batch auto-display no longer needed (per-item scheduling). Retained as safety net if invoked.
    if (!m_hostSceneActive) return;
    if (!m_scene) return;
    for (QGraphicsItem* gi : m_scene->items()) {
        if (auto* media = dynamic_cast<ResizableMediaBase*>(gi)) {
            if (media->settingsPanel() && media->settingsPanel()->displayAutomaticallyEnabled()) {
                media->showWithConfiguredFade();
            }
        }
    }
}

void ScreenCanvas::handleAutoPlayback() {
    if (!m_hostSceneActive) return;
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            if (auto* vid = dynamic_cast<ResizableVideoItem*>(gi)) {
                // Using existing togglePlayPause if currently paused at start.
                vid->togglePlayPause();
            }
        }
    }
}
