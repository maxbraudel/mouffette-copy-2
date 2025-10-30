// ScreenCanvas implementation (snap guides rendered via dedicated SnapGuideItem)
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "backend/network/WebSocketClient.h" // for remote scene start/stop messaging
#include "backend/network/UploadManager.h" // for upload state checking
#include "backend/domain/session/SessionManager.h" // Phase 3: For DEFAULT_IDEA_ID constant
#include <QTimer>
#include "frontend/ui/theme/AppColors.h"
#include "backend/files/Theme.h"
#include "backend/domain/media/MediaItems.h"
#include "backend/domain/media/MediaSettingsPanel.h" // for settingsPanel() accessor usage
#include "frontend/ui/notifications/ToastNotificationSystem.h" // for toast notifications
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
#include <QToolButton>
#include <QFont>
#include <QIcon>
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
#include <QSignalBlocker>
#include <QRandomGenerator>
#include <QSizePolicy>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QRegion>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <limits>

#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacVideoThumbnailer.h"
#endif
#ifdef Q_OS_WIN
#include "backend/platform/windows/WindowsVideoThumbnailer.h"
#endif

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

QSet<ScreenCanvas*> ScreenCanvas::s_activeCanvases;
bool ScreenCanvas::s_applicationSuspended = false;

void ScreenCanvas::registerCanvas(ScreenCanvas* canvas) {
    if (!canvas) return;
    const bool wasEmpty = s_activeCanvases.isEmpty();
    s_activeCanvases.insert(canvas);
    if (wasEmpty) {
        ResizableMediaBase::setUploadChangedNotifier([]() {
            ScreenCanvas::dispatchUploadStateChanged();
        });
    }
    if (s_applicationSuspended) {
        canvas->applyApplicationSuspended(true);
    }
}

void ScreenCanvas::unregisterCanvas(ScreenCanvas* canvas) {
    if (!canvas) return;
    s_activeCanvases.remove(canvas);
    if (s_activeCanvases.isEmpty()) {
        ResizableMediaBase::setUploadChangedNotifier(nullptr);
    }
}

void ScreenCanvas::dispatchUploadStateChanged() {
    const auto canvases = s_activeCanvases;
    for (ScreenCanvas* canvas : canvases) {
        if (canvas) {
            canvas->scheduleInfoOverlayRefresh();
        }
    }
}

void ScreenCanvas::setAllCanvasesSuspended(bool suspended) {
    if (s_applicationSuspended == suspended) {
        return;
    }
    s_applicationSuspended = suspended;
    const auto canvases = s_activeCanvases;
    for (ScreenCanvas* canvas : canvases) {
        if (canvas) {
            canvas->applyApplicationSuspended(suspended);
        }
    }
}

void ScreenCanvas::applyApplicationSuspended(bool suspended) {
    if (m_applicationSuspended == suspended) {
        return;
    }
    m_applicationSuspended = suspended;
    if (!m_scene) {
        return;
    }
    const QList<QGraphicsItem*> items = m_scene->items();
    for (QGraphicsItem* gi : items) {
        if (auto* video = dynamic_cast<ResizableVideoItem*>(gi)) {
            video->setApplicationSuspended(suspended);
        }
    }
}

void ScreenCanvas::drawBackground(QPainter* painter, const QRectF& rect) {
    QGraphicsView::drawBackground(painter, rect);
}

void ScreenCanvas::drawForeground(QPainter* painter, const QRectF& rect) {
    QGraphicsView::drawForeground(painter, rect);
}

// Local lightweight ClippedContainer for viewport overlay clipping.

// ================================================================================================
// Z-ORDER HIERARCHY (QGraphicsScene Z-value ranges)
// ================================================================================================
// Media items:          1.0 - 9999.0   (user-controlled Z-order via Bring to Front/Send to Back)
// Remote cursor:        11500.0        (always visible above all media, not in media Z-group)
// Selection chrome:     11998.0 - 11999.5 (selection borders and resize handles)
// Overlays & UI:        ≥12000.0       (info panel background, snap guides, etc.)
//
// This separation ensures the remote cursor cannot be obscured by media items when users
// repeatedly bring items to front, while selection chrome remains visible above the cursor.
// ================================================================================================

// Configuration constants
static const int gMediaListItemSpacing = 3; // Spacing between media list items (name, status, details)
int gMediaListOverlayAbsoluteMaxWidthPx = 420; // Absolute width cap (px) for media list overlay; 0 disables the cap
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

static bool uiZonesEquivalent(const ScreenInfo::UIZone& a, const ScreenInfo::UIZone& b) {
    return a.type == b.type && a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

static bool screensEquivalent(const ScreenInfo& a, const ScreenInfo& b) {
    if (a.id != b.id || a.width != b.width || a.height != b.height || a.x != b.x || a.y != b.y || a.primary != b.primary) {
        return false;
    }
    if (a.uiZones.size() != b.uiZones.size()) {
        return false;
    }
    for (int i = 0; i < a.uiZones.size(); ++i) {
        if (!uiZonesEquivalent(a.uiZones.at(i), b.uiZones.at(i))) {
            return false;
        }
    }
    return true;
}

static bool screenListsEquivalent(const QList<ScreenInfo>& a, const QList<ScreenInfo>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (!screensEquivalent(a.at(i), b.at(i))) {
            return false;
        }
    }
    return true;
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
    unregisterCanvas(this);
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
    // Phase 3: canvasSessionId is MANDATORY - only include in manifest if it's not the default
    if (m_activeIdeaId != DEFAULT_IDEA_ID) {
        root["canvasSessionId"] = m_activeIdeaId;
    }
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
            if (!media->fileId().isEmpty() && m_fileManager) {
                QString p = m_fileManager->getFilePathForId(media->fileId());
                if (!p.isEmpty()) {
                    QFileInfo fi(p); m["fileName"] = fi.fileName();
                }
            }
            m["type"] = media->isVideoMedia() ? "video" : "image";
            QRectF br;
            const QSize baseSize = media->baseSizePx();
            if (baseSize.width() > 0 && baseSize.height() > 0) {
                br = media->mapRectToScene(QRectF(QPointF(0, 0), QSizeF(baseSize)));
            } else {
                br = media->sceneBoundingRect();
            }
            br = br.normalized();
            m["x"] = br.x();
            m["y"] = br.y();
            m["width"] = br.width();
            m["height"] = br.height();
            m["baseWidth"] = media->baseSizePx().width();
            m["baseHeight"] = media->baseSizePx().height();
            m["visible"] = media->isContentVisible();
            // Compute per-screen spans: for every screen intersecting this media, include a span with normalized geometry
            // relative to that screen. Do NOT clamp; negatives/overflow allow the remote to position and rely on parent clipping.
            int bestScreenId = -1; QRectF bestScreenRect; qreal bestArea = 0.0;
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
                // Track best-overlap screen for potential fallback span creation
                if (area > bestArea) { bestArea = area; bestScreenId = sid; bestScreenRect = srect; }
            }
            if (spans.isEmpty() && bestScreenId != -1 && bestScreenRect.width() > 0.0 && bestScreenRect.height() > 0.0) {
                QJsonObject span;
                span["screenId"] = bestScreenId;
                span["normX"] = (br.x() - bestScreenRect.x()) / bestScreenRect.width();
                span["normY"] = (br.y() - bestScreenRect.y()) / bestScreenRect.height();
                span["normW"] = br.width() / bestScreenRect.width();
                span["normH"] = br.height() / bestScreenRect.height();
                spans.append(span);
            }
            if (!spans.isEmpty()) {
                m["spans"] = spans;
            }
            const bool autoDisplay = media->autoDisplayEnabled();
            m["autoDisplay"] = autoDisplay;
            m["autoDisplayDelayMs"] = media->autoDisplayDelayMs();
            const bool autoHide = media->autoHideEnabled();
            m["autoHide"] = autoHide;
            m["autoHideDelayMs"] = media->autoHideDelayMs();
            m["hideWhenVideoEnds"] = media->hideWhenVideoEnds();
            if (media->isVideoMedia()) {
                m["autoPlay"] = media->autoPlayEnabled();
                m["autoPlayDelayMs"] = media->autoPlayDelayMs();
                m["autoPause"] = media->autoPauseEnabled();
                m["autoPauseDelayMs"] = media->autoPauseDelayMs();
                if (auto* v = dynamic_cast<ResizableVideoItem*>(media)) {
                    m["muted"] = v->isMuted();
                    m["volume"] = v->volume();
                    const auto& settings = media->mediaSettingsState();
                    m["repeatEnabled"] = settings.repeatEnabled;
                    int repeatCount = 0;
                    if (settings.repeatEnabled) {
                        bool ok = false;
                        int value = settings.repeatCountText.trimmed().toInt(&ok);
                        if (ok && value > 0) {
                            repeatCount = value;
                        }
                    }
                    m["repeatCount"] = repeatCount;
                    m["autoUnmute"] = media->autoUnmuteEnabled();
                    m["autoUnmuteDelayMs"] = media->autoUnmuteDelayMs();
                    m["autoMute"] = media->autoMuteEnabled();
                    m["autoMuteDelayMs"] = media->autoMuteDelayMs();
                    m["muteWhenVideoEnds"] = media->muteWhenVideoEnds();
                    m["audioFadeInSeconds"] = media->audioFadeInDurationSeconds();
                    m["audioFadeOutSeconds"] = media->audioFadeOutDurationSeconds();
                    const qint64 currentPos = std::max<qint64>(0, v->currentPositionMs());
                    const qint64 displayedTimestamp = v->displayedFrameTimestampMs();
                    if (displayedTimestamp >= 0) {
                        m["startPositionMs"] = static_cast<double>(displayedTimestamp);
                        m["displayedFrameTimestampMs"] = static_cast<double>(displayedTimestamp);
                    } else {
                        m["startPositionMs"] = static_cast<double>(currentPos);
                    }
                }
            }
            m["fadeInSeconds"] = media->fadeInDurationSeconds();
            m["fadeOutSeconds"] = media->fadeOutDurationSeconds();
            // Always include base content opacity (user configured). Animation multiplier not serialized.
            m["contentOpacity"] = media->contentOpacity();
            mediaArr.append(m);
        }
    }
    root["media"] = mediaArr;
    return root;
}

void ScreenCanvas::setActiveIdeaId(const QString& canvasSessionId) {
    if (m_activeIdeaId == canvasSessionId) {
        return;
    }
    m_activeIdeaId = canvasSessionId;
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
    // Use a vertical stack to host the launch buttons above Upload with separators
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

        const QString canvasFontCss = AppColors::canvasButtonFontCss();

        // Top separator
        vHeaderLayout->addWidget(createSeparator());

        // Launch Remote Scene toggle button
        m_launchSceneButton = new QPushButton("Launch Remote Scene", m_overlayHeaderWidget);
        m_launchSceneButton->setCheckable(true);
        QFont launchSceneFont = m_launchSceneButton->font();
        AppColors::applyCanvasButtonFont(launchSceneFont);
        m_launchSceneButton->setFont(launchSceneFont);
        m_launchSceneButton->setStyleSheet(QString(
            "QPushButton { "
            "    padding: 8px 0px; "
            "    %1 "
            "    color: %2; "
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
        ).arg(canvasFontCss, AppColors::colorToCss(AppColors::gOverlayTextColor)));
        m_launchSceneButton->setFixedHeight(40);
        m_launchSceneButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        vHeaderLayout->addWidget(m_launchSceneButton);

        // Separator between Launch Remote Scene and Launch Test Scene
        vHeaderLayout->addWidget(createSeparator());

        // Launch Test Scene toggle button
        m_launchTestSceneButton = new QPushButton("Launch Test Scene", m_overlayHeaderWidget);
        m_launchTestSceneButton->setCheckable(true);
        QFont launchTestSceneFont = m_launchTestSceneButton->font();
        AppColors::applyCanvasButtonFont(launchTestSceneFont);
        m_launchTestSceneButton->setFont(launchTestSceneFont);
        m_launchTestSceneButton->setStyleSheet(QString(
            "QPushButton { "
            "    padding: 8px 0px; "
            "    %1 "
            "    color: %2; "
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
        ).arg(canvasFontCss, AppColors::colorToCss(AppColors::gOverlayTextColor)));
        m_launchTestSceneButton->setFixedHeight(40);
        m_launchTestSceneButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        vHeaderLayout->addWidget(m_launchTestSceneButton);

        // Separator between Launch Test Scene and Upload
        vHeaderLayout->addWidget(createSeparator());

        // Upload button (kept as before, with no top border)
        m_uploadButton = new QPushButton("Upload", m_overlayHeaderWidget);
        QFont uploadFont = m_uploadButton->font();
        AppColors::applyCanvasButtonFont(uploadFont);
        m_uploadButton->setFont(uploadFont);
        m_uploadButton->setStyleSheet(QString(
            "QPushButton { "
            "    padding: 8px 0px; "
            "    %1 "
            "    color: %2; "
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
        ).arg(canvasFontCss, AppColors::colorToCss(AppColors::gOverlayTextColor)));
        m_uploadButton->setFixedHeight(40);
        m_uploadButton->setMinimumWidth(0);
        const int uploadButtonMaxWidth = (gMediaListOverlayAbsoluteMaxWidthPx > 0)
            ? gMediaListOverlayAbsoluteMaxWidthPx
            : std::numeric_limits<int>::max();
        m_uploadButton->setMaximumWidth(uploadButtonMaxWidth);
        m_uploadButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        vHeaderLayout->addWidget(m_uploadButton);

        // Wire Launch Remote Scene toggle behavior (Remote mode = local + remote)
        connect(m_launchSceneButton, &QPushButton::clicked, this, [this]() {
            // Ignore clicks while launch/stop handshake in progress
            if (m_sceneLaunching || m_sceneStopping) return;

            if (!m_sceneLaunched) {
                // Early error detection: check prerequisites before starting
                if (!m_wsClient) {
                    TOAST_ERROR("Cannot launch scene: Not connected to server", 3000);
                    return;
                }
                if (!m_wsClient->isConnected()) {
                    TOAST_ERROR("Cannot launch scene: Connection lost", 3000);
                    return;
                }
                if (m_remoteSceneTargetClientId.isEmpty()) {
                    TOAST_ERROR("Cannot launch scene: No target client selected", 3000);
                    return;
                }
                
                // Check if there are any media items in the scene
                bool hasMedia = false;
                if (m_scene) {
                    for (QGraphicsItem* gi : m_scene->items()) {
                        if (dynamic_cast<ResizableMediaBase*>(gi)) {
                            hasMedia = true;
                            break;
                        }
                    }
                }
                if (!hasMedia) {
                    TOAST_ERROR("Cannot launch scene: No media items in scene", 3000);
                    return;
                }
                
                if (m_testSceneLaunched) {
                    m_testSceneLaunched = false;
                    if (m_launchTestSceneButton && m_launchTestSceneButton->isCheckable()) {
                        m_launchTestSceneButton->setChecked(false);
                    }
                }
                
                // Enter loading state
                m_sceneLaunching = true;
                updateLaunchSceneButtonStyle();
                updateLaunchTestSceneButtonStyle(); // Update test scene button (will be disabled)
                TOAST_INFO("Sending scene to remote client...", 2000);
                
                // Start timeout timer
                if (!m_sceneLaunchTimeoutTimer) {
                    m_sceneLaunchTimeoutTimer = new QTimer(this);
                    m_sceneLaunchTimeoutTimer->setSingleShot(true);
                    connect(m_sceneLaunchTimeoutTimer, &QTimer::timeout, this, &ScreenCanvas::onRemoteSceneLaunchTimeout);
                }
                m_sceneLaunchTimeoutTimer->start(REMOTE_SCENE_LAUNCH_TIMEOUT_MS);
                
                // Send scene data (validation will happen on remote side)
                if (m_wsClient) {
                    QJsonObject sceneObj = serializeSceneState();
                    qDebug() << "ScreenCanvas: sending remote_scene_start to" << m_remoteSceneTargetClientId
                             << "mediaCount=" << sceneObj.value("media").toArray().size()
                             << "screenCount=" << sceneObj.value("screens").toArray().size();
                    m_wsClient->sendRemoteSceneStart(m_remoteSceneTargetClientId, sceneObj);
                }

            } else {
                if (!m_wsClient || !m_wsClient->isConnected()) {
                    TOAST_WARNING("Connection lost while stopping scene; cleaning up locally", 3500);
                    stopHostSceneState();
                    updateLaunchSceneButtonStyle();
                    updateLaunchTestSceneButtonStyle();
                    return;
                }

                m_sceneStopping = true;
                updateLaunchSceneButtonStyle();
                updateLaunchTestSceneButtonStyle();

                if (!m_sceneStopTimeoutTimer) {
                    m_sceneStopTimeoutTimer = new QTimer(this);
                    m_sceneStopTimeoutTimer->setSingleShot(true);
                    connect(m_sceneStopTimeoutTimer, &QTimer::timeout, this, &ScreenCanvas::onRemoteSceneStopTimeout);
                }
                m_sceneStopTimeoutTimer->start(REMOTE_SCENE_STOP_TIMEOUT_MS);

                TOAST_INFO("Requesting remote scene stop...", 2000);
                if (m_wsClient) {
                    qDebug() << "ScreenCanvas: requesting remote_scene_stop from" << m_remoteSceneTargetClientId;
                    m_wsClient->sendRemoteSceneStop(m_remoteSceneTargetClientId);
                }
            }
        });

        // Wire Launch Test Scene toggle behavior (Test mode = local only)
        connect(m_launchTestSceneButton, &QPushButton::clicked, this, [this]() {
            bool newState = !m_testSceneLaunched;
            if (newState) {
                if (m_sceneLaunched) {
                    m_sceneLaunched = false;
                    if (m_launchSceneButton && m_launchSceneButton->isCheckable()) {
                        m_launchSceneButton->setChecked(false);
                    }
                    emitRemoteSceneLaunchStateChanged();
                }
                startHostSceneState(HostSceneMode::Test);
            } else {
                stopHostSceneState();
            }
            m_testSceneLaunched = newState;
            if (m_launchTestSceneButton->isCheckable()) {
                m_launchTestSceneButton->setChecked(m_testSceneLaunched);
            }
            updateLaunchTestSceneButtonStyle();
            updateLaunchSceneButtonStyle(); // Update remote scene button (will be enabled/disabled)
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
    
    // Ensure settings toggle button + detached panel are ready
    ensureSettingsToggleButton();
    if (!m_globalSettingsPanel) {
        m_globalSettingsPanel = new MediaSettingsPanel(viewport());
        m_globalSettingsPanel->setVisible(false);
        m_globalSettingsPanel->updatePosition();
    }
    updateSettingsToggleButtonGeometry();
    updateToolSelectorGeometry();
    
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
    
    const int availableTextWidth = std::max(0, m_infoWidget->width() - 40); // 20px margins on each side
    
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

    // Apply relative (50% viewport) and absolute caps
    const int viewportCap = static_cast<int>(viewport()->width() * 0.5);
    int effectiveCap = viewportCap;
    if (gMediaListOverlayAbsoluteMaxWidthPx > 0) {
        effectiveCap = (effectiveCap > 0)
            ? std::min(effectiveCap, gMediaListOverlayAbsoluteMaxWidthPx)
            : gMediaListOverlayAbsoluteMaxWidthPx;
    }

    // If viewportCap is zero (e.g., viewport not yet sized) and no absolute cap, skip constraining
    if (effectiveCap <= 0) {
        return {desiredW, false};
    }

    const bool isWidthConstrained = desiredW > effectiveCap;
    return {isWidthConstrained ? effectiveCap : desiredW, isWidthConstrained};
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
    connect(m_scene, &QGraphicsScene::selectionChanged, this, [this](){ 
        updateSelectionChrome(); 
        updateGlobalSettingsPanelVisibility(); // Update settings panel when selection changes
    });
    
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

    // Register for global upload state callbacks
    registerCanvas(this);
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
    QTimer::singleShot(0, this, [this]() { 
        updateSettingsToggleButtonGeometry(); 
        updateToolSelectorGeometry();
    });
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    if (screenListsEquivalent(m_screens, screens)) {
        return;
    }
    // Skip updates with empty screens if we already have active screens
    // This prevents flicker when the remote client temporarily disconnects
    if (screens.isEmpty() && !m_screens.isEmpty()) {
        return;
    }
    m_screens = screens;
    
    // Disable ALL updates during reconstruction to prevent visible intermediate states
    QWidget* vp = viewport();
    const bool vpUpdatesEnabled = vp ? vp->updatesEnabled() : true;
    const bool viewUpdatesEnabled = updatesEnabled();
    
    if (vp) vp->setUpdatesEnabled(false);
    setUpdatesEnabled(false);
    
    if (m_scene) {
        QSignalBlocker blockScene(m_scene);
        m_scene->setSceneRect(m_scene->sceneRect()); // Force scene to not emit changed
        createScreenItems();
    } else {
        createScreenItems();
    }
    
    // Re-enable updates and trigger single consolidated update
    if (vp && vpUpdatesEnabled) {
        vp->setUpdatesEnabled(true);
    }
    if (viewUpdatesEnabled) {
        setUpdatesEnabled(true);
        // Single update at the end
        viewport()->update();
    }
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

bool ScreenCanvas::hasActiveScreens() const {
    if (!m_screens.isEmpty()) return true;
    for (auto* item : m_screenItems) {
        if (item) return true;
    }
    return false;
}

void ScreenCanvas::hideContentPreservingState() {
    if (!m_scene) return;
    if (m_contentHiddenPreservingState) return;
    // Hide screen items and remote cursor without deleting them
    // This preserves the viewport state (zoom/pan position)
    for (auto* r : m_screenItems) {
        if (r) r->setVisible(false);
    }
    hideRemoteCursor();
    
    // Hide overlays but don't clear them
    if (m_infoWidget) {
        m_infoWidgetWasVisibleBeforeHide = m_infoWidget->isVisible();
        m_infoWidget->setVisible(false);
    } else {
        m_infoWidgetWasVisibleBeforeHide = false;
    }
    m_contentHiddenPreservingState = true;
}

void ScreenCanvas::showContentAfterReconnect() {
    if (!m_scene) return;
    if (!m_contentHiddenPreservingState) {
        // Nothing was hidden; ensure overlay stays visible without forced refresh
        if (m_infoWidget && !m_infoWidget->isHidden()) {
            m_infoWidget->update();
        }
        m_infoWidgetWasVisibleBeforeHide = false;
        return;
    }
    // Show screen items and overlays again
    // Viewport state (zoom/pan) is automatically preserved
    for (auto* r : m_screenItems) {
        if (r) r->setVisible(true);
    }
    
    // Show overlays again
    if (m_infoWidget) {
        if (m_infoWidgetWasVisibleBeforeHide) {
            m_infoWidget->setVisible(true);
        }
    }
    
    // Only refresh overlay if content actually changed during disconnection
    // This avoids unnecessary rebuilds that cause flicker
    // The overlay will be updated by normal mechanisms (maybeRefreshInfoOverlayOnSceneChanged)
    // refreshInfoOverlay(); // Removed to prevent flicker
    m_contentHiddenPreservingState = false;
    m_infoWidgetWasVisibleBeforeHide = false;
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
            // Z-order hierarchy: Media (1-9999) < Remote Cursor (11500) < Selection Chrome (11998-11999.5) < Overlays (≥12000)
            const qreal zBorderWhite = 11998.0;
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
    const QString hoverBg = "rgba(255,255,255,0.05)";
    const QString disabledBg = "rgba(255,255,255,0.03)"; // Very subtle grey for disabled state
    
    bool remoteSceneActive = m_sceneLaunched || m_sceneLaunching || m_sceneStopping;
    
    for (auto it = m_mediaContainerByItem.begin(); it != m_mediaContainerByItem.end(); ++it) {
        ResizableMediaBase* media = it.key(); QWidget* w = it.value(); if (!w) continue;
        bool sel = stillSelected.contains(media);
        bool hovered = (m_hoveredMediaItem == media);
        w->setAutoFillBackground(true);
        w->setAttribute(Qt::WA_TranslucentBackground, false);
        // Pleine largeur: pas de border-radius pour que le fond touche les séparateurs
        // Priority: Remote Scene Disabled > Selected > Hovered > Transparent
        QString bgColor;
        QString opacity;
        if (remoteSceneActive) {
            // When remote scene is active, apply disabled appearance
            bgColor = disabledBg;
            opacity = "0.4"; // Make text/labels less prominent
            w->setStyleSheet(QString("QWidget { background-color: %1; } QLabel { opacity: %2; }")
                             .arg(bgColor).arg(opacity));
        } else {
            bgColor = sel ? selectedBg : (hovered ? hoverBg : "transparent");
            w->setStyleSheet(QString("QWidget { background-color: %1; }")
                             .arg(bgColor));
        }
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
    // Disable media container interactions when remote scene is active
    bool remoteSceneActive = m_sceneLaunched || m_sceneLaunching;
    
    // Handle clicks on media container widgets to select the associated scene media item
    if (event->type() == QEvent::MouseButtonPress) {
        if (auto* w = qobject_cast<QWidget*>(watched)) {
            auto it = m_mediaItemByContainer.find(w);
            if (it != m_mediaItemByContainer.end()) {
                // Block selection when remote scene is active
                if (remoteSceneActive) {
                    return true; // consume event without selecting
                }
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
                // Disable hover effect when remote scene is active
                if (remoteSceneActive) {
                    return false; // don't track hover
                }
                // Track hovered item and apply hover feedback
                ResizableMediaBase* media = m_mediaItemByContainer.value(w);
                m_hoveredMediaItem = media;
                // Update style through updateSelectionChrome to maintain consistency
                updateSelectionChrome();
            }
        }
    } else if (event->type() == QEvent::Leave) {
        if (auto* w = qobject_cast<QWidget*>(watched)) {
            if (m_mediaItemByContainer.contains(w)) {
                ResizableMediaBase* media = m_mediaItemByContainer.value(w);
                // Clear hover tracking if leaving the currently hovered item
                if (m_hoveredMediaItem == media) {
                    m_hoveredMediaItem = nullptr;
                }
                // Update style through updateSelectionChrome to maintain consistency
                updateSelectionChrome();
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
                        deleteMediaItem(base);
                    }
                }
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

void ScreenCanvas::deleteMediaItem(ResizableMediaBase* item) {
    if (!item) return;

    item->setVisible(false);
    item->setEnabled(false);
    item->prepareForDeletion();

    clearSelectionChromeFor(item);
    if (item->isSelected()) item->setSelected(false);

    if (QWidget* container = m_mediaContainerByItem.take(item)) {
        m_mediaItemByContainer.remove(container);
        container->hide();
        container->deleteLater();
    } else {
        // Ensure reverse map doesn't retain the pointer if it wasn't registered in the hash yet
        for (auto it = m_mediaItemByContainer.begin(); it != m_mediaItemByContainer.end();) {
            if (it.value() == item) {
                if (QWidget* w = it.key()) {
                    w->hide();
                    w->deleteLater();
                }
                it = m_mediaItemByContainer.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Drop any cached host-scene selection references
    for (int i = m_prevSelectionBeforeHostScene.size() - 1; i >= 0; --i) {
        if (m_prevSelectionBeforeHostScene[i].media == item) {
            m_prevSelectionBeforeHostScene.removeAt(i);
        }
    }
    if (auto* video = dynamic_cast<ResizableVideoItem*>(item)) {
        for (int i = m_prevVideoStates.size() - 1; i >= 0; --i) {
            if (m_prevVideoStates[i].video == video) {
                m_prevVideoStates.removeAt(i);
            }
        }
    }

    QGraphicsScene* owningScene = item->scene();
    if (!owningScene && m_scene) {
        owningScene = m_scene;
    }
    if (owningScene) {
        owningScene->removeItem(item);
        owningScene->update();
    }

    // Emit signal before deleting so handlers can still access item data
    emit mediaItemRemoved(item);

    delete item;

    refreshInfoOverlay();
    layoutInfoOverlay();

    if (viewport()) viewport()->update();
}

void ScreenCanvas::requestMediaDeletion(ScreenCanvas* canvas, ResizableMediaBase* item) {
    if (!canvas) {
        return;
    }
    canvas->deleteMediaItem(item);
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
            if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) { if (v->isDraggingProgress() || v->isDraggingVolume()) { v->endDrag(); } } }
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
    updateSettingsToggleButtonGeometry();
    updateToolSelectorGeometry();
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
    const QTransform originalTransform = transform();
    QPointF originalCenter;
    if (viewport()) {
        originalCenter = mapToScene(viewport()->rect().center());
    } else {
        originalCenter = mapToScene(rect().center());
    }

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
                    if (m_applicationSuspended) {
                        v->setApplicationSuspended(true);
                    }
                    // Record original file path for later upload manifest collection
                    v->setSourcePath(localPath);
                    // Preserve global canvas media scale (so video size matches screens & images 1:1)
                    v->setInitialScaleFactor(m_scaleFactor);
                    
                    // If a drag preview frame was captured for this video, use it immediately as a poster to avoid flicker gap
                    // Scale the poster to match the actual video dimensions so adoptBaseSize gets the right size
                    // IMPORTANT: Set poster BEFORE positioning, because setExternalPosterImage calls adoptBaseSize which repositions
                    if (m_dragPreviewIsVideo && m_dragPreviewGotFrame && !m_dragPreviewPixmap.isNull()) {
                        QImage poster = m_dragPreviewPixmap.toImage();
                        if (!poster.isNull()) {
                            // If we know the actual video size and it differs from the thumbnail, scale the poster
                            if (!m_dragPreviewVideoSize.isEmpty() && poster.size() != m_dragPreviewVideoSize) {
                                poster = poster.scaled(m_dragPreviewVideoSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                            }
                            v->setExternalPosterImage(poster);
                        }
                    }
                    
                    // Use actual video dimensions from preview if available, otherwise use default placeholder
                    QSize videoSize = m_dragPreviewVideoSize.isEmpty() ? QSize(640, 360) : m_dragPreviewVideoSize;
                    const qreal phW = videoSize.width() * m_scaleFactor;
                    const qreal phH = videoSize.height() * m_scaleFactor;
                    v->setPos(scenePos - QPointF(phW/2.0, phH/2.0));
                    v->setScale(m_scaleFactor); // adoptBaseSize already called by setExternalPosterImage
                    
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

    QPointF currentCenter;
    if (viewport()) {
        currentCenter = mapToScene(viewport()->rect().center());
    } else {
        currentCenter = mapToScene(rect().center());
    }
    const qreal dx = currentCenter.x() - originalCenter.x();
    const qreal dy = currentCenter.y() - originalCenter.y();
    const bool transformChanged = !(transform() == originalTransform);
    const bool centerShifted = std::hypot(dx, dy) > 0.5;
    if (transformChanged || centerShifted) {
        setTransform(originalTransform);
        centerOn(originalCenter);
        if (m_scene) {
            relayoutAllMediaOverlays(m_scene);
        }
        layoutInfoOverlay();
        updateSelectionChrome();
    }
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
        auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); 
        pmItem->setOpacity(0.0); 
        pmItem->setZValue(5000.0); 
        // Don't set scale here for non-video - updateDragPreviewPos will handle it
        pmItem->setScale(m_scaleFactor); 
        m_scene->addItem(pmItem); 
        m_dragPreviewItem = pmItem; 
        startDragPreviewFadeIn();
    }
}

void ScreenCanvas::updateDragPreviewPos(const QPointF& scenePos) {
    if (!m_dragPreviewItem) return; 
    
    auto* pmItem = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem);
    
    // Determine the scale factor for the preview item
    qreal itemScale = m_scaleFactor;
    QSize displaySize = m_dragPreviewBaseSize;
    
    // If we have video dimensions and they differ from the pixmap size, scale accordingly
    if (pmItem && !m_dragPreviewVideoSize.isEmpty() && !m_dragPreviewPixmap.isNull()) {
        QSize thumbSize = m_dragPreviewPixmap.size();
        if (!thumbSize.isEmpty() && thumbSize.width() > 0 && thumbSize.height() > 0) {
            // Scale thumbnail to display at actual video dimensions
            qreal scaleX = static_cast<qreal>(m_dragPreviewVideoSize.width()) / thumbSize.width();
            qreal scaleY = static_cast<qreal>(m_dragPreviewVideoSize.height()) / thumbSize.height();
            itemScale = scaleX * m_scaleFactor; // Use X scale (they should be proportional)
            displaySize = m_dragPreviewVideoSize;
        }
    }
    
    if (displaySize.isEmpty()) displaySize = QSize(400, 240);
    
    if (pmItem) {
        pmItem->setScale(itemScale);
    }
    
    QPointF topLeft = scenePos - QPointF(displaySize.width()/2.0 * m_scaleFactor, displaySize.height()/2.0 * m_scaleFactor);
    m_dragPreviewItem->setPos(topLeft);
}

void ScreenCanvas::clearDragPreview() {
    stopVideoPreviewProbe(); stopDragPreviewFade(); if (m_dragPreviewItem) { m_scene->removeItem(m_dragPreviewItem); delete m_dragPreviewItem; m_dragPreviewItem = nullptr; }
    m_dragPreviewPixmap = QPixmap(); m_dragPreviewGotFrame = false; m_dragPreviewIsVideo = false; m_dragPreviewVideoSize = QSize();
}


void ScreenCanvas::startVideoPreviewProbe(const QString& localFilePath) {
#ifdef Q_OS_MACOS
    startFastMacThumbnailProbe(localFilePath);
#elif defined(Q_OS_WIN)
    if (m_dragPreviewGotFrame) {
        return;
    }
    // Get actual video dimensions first
    QSize dims = WindowsVideoThumbnailer::videoDimensions(localFilePath);
    if (!dims.isEmpty()) {
        m_dragPreviewVideoSize = dims;
        m_dragPreviewBaseSize = dims;
    }
    QImage thumb = WindowsVideoThumbnailer::firstFrame(localFilePath);
    if (!thumb.isNull()) {
        onFastVideoThumbnailReady(thumb);
        return;
    }
    startVideoPreviewProbeFallback(localFilePath);
#else
    startVideoPreviewProbeFallback(localFilePath);
#endif
}

void ScreenCanvas::startVideoPreviewProbeFallback(const QString& localFilePath) {
    if (m_dragPreviewPlayer) return; 
    m_dragPreviewPlayer = new QMediaPlayer(this); 
    m_dragPreviewAudio = new QAudioOutput(this); 
    m_dragPreviewAudio->setMuted(true); 
    m_dragPreviewPlayer->setAudioOutput(m_dragPreviewAudio); 
    m_dragPreviewSink = new QVideoSink(this); 
    m_dragPreviewPlayer->setVideoSink(m_dragPreviewSink); 
    m_dragPreviewPlayer->setSource(QUrl::fromLocalFile(localFilePath));
    
    // Capture actual video dimensions from first frame
    connect(m_dragPreviewSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f){ 
        if (m_dragPreviewGotFrame || !f.isValid()) return; 
        QImage img = f.toImage(); 
        if (img.isNull()) return; 
        m_dragPreviewGotFrame = true; 
        QPixmap newPm = QPixmap::fromImage(img); 
        if (newPm.isNull()) return; 
        m_dragPreviewPixmap = newPm; 
        m_dragPreviewVideoSize = newPm.size(); // store actual video dimensions
        m_dragPreviewBaseSize = newPm.size(); // use video dimensions for preview
        if (!m_dragPreviewItem) { 
            auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); 
            pmItem->setOpacity(0.0); 
            pmItem->setZValue(5000.0); 
            // Don't set scale here - updateDragPreviewPos will handle it
            m_scene->addItem(pmItem); 
            m_dragPreviewItem = pmItem; 
            updateDragPreviewPos(m_dragPreviewLastScenePos); 
            startDragPreviewFadeIn(); 
        } else if (auto* pmItem = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) { 
            pmItem->setPixmap(m_dragPreviewPixmap); 
            updateDragPreviewPos(m_dragPreviewLastScenePos); 
        } 
        if (m_dragPreviewPlayer) m_dragPreviewPlayer->pause(); 
        if (m_dragPreviewFallbackTimer) { 
            m_dragPreviewFallbackTimer->stop(); 
            m_dragPreviewFallbackTimer->deleteLater(); 
            m_dragPreviewFallbackTimer = nullptr; 
        } 
    });
    m_dragPreviewPlayer->play();
}

#ifdef Q_OS_MACOS
void ScreenCanvas::startFastMacThumbnailProbe(const QString& localFilePath) {
    cancelFastMacThumbnailProbe();
    m_dragPreviewPendingVideoPath = localFilePath;

    // Get actual video dimensions immediately (very fast, no frame extraction)
    QSize dims = MacVideoThumbnailer::videoDimensions(localFilePath);
    if (!dims.isEmpty()) {
        m_dragPreviewVideoSize = dims;
        m_dragPreviewBaseSize = dims;
    }

    auto* watcher = new QFutureWatcher<QImage>(this);
    m_dragPreviewThumbnailWatcher = watcher;

    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher]() {
        QImage img = watcher->result();
        watcher->deleteLater();
        m_dragPreviewThumbnailWatcher = nullptr;

        if (m_dragPreviewFallbackDelayTimer) {
            m_dragPreviewFallbackDelayTimer->stop();
        }

        if (!img.isNull()) {
            onFastVideoThumbnailReady(img);
        } else if (!m_dragPreviewPlayer && !m_dragPreviewPendingVideoPath.isEmpty()) {
            startVideoPreviewProbeFallback(m_dragPreviewPendingVideoPath);
        }

        m_dragPreviewPendingVideoPath.clear();
    });

    watcher->setFuture(QtConcurrent::run([path = localFilePath]() {
        return MacVideoThumbnailer::firstFrame(path);
    }));

    if (!m_dragPreviewFallbackDelayTimer) {
        m_dragPreviewFallbackDelayTimer = new QTimer(this);
        m_dragPreviewFallbackDelayTimer->setSingleShot(true);
        connect(m_dragPreviewFallbackDelayTimer, &QTimer::timeout, this, [this]() {
            if (m_dragPreviewPlayer || m_dragPreviewGotFrame) {
                return;
            }
            if (m_dragPreviewPendingVideoPath.isEmpty()) {
                return;
            }
            startVideoPreviewProbeFallback(m_dragPreviewPendingVideoPath);
        });
    }

    m_dragPreviewFallbackDelayTimer->start(250);
}

void ScreenCanvas::cancelFastMacThumbnailProbe() {
    if (m_dragPreviewThumbnailWatcher) {
        disconnect(m_dragPreviewThumbnailWatcher, nullptr, this, nullptr);
        m_dragPreviewThumbnailWatcher->cancel();
        m_dragPreviewThumbnailWatcher->deleteLater();
        m_dragPreviewThumbnailWatcher = nullptr;
    }
    if (m_dragPreviewFallbackDelayTimer) {
        m_dragPreviewFallbackDelayTimer->stop();
    }
    m_dragPreviewPendingVideoPath.clear();
}
#endif

void ScreenCanvas::stopVideoPreviewProbe() {
#ifdef Q_OS_MACOS
    cancelFastMacThumbnailProbe();
    if (m_dragPreviewFallbackDelayTimer) {
        m_dragPreviewFallbackDelayTimer->stop();
    }
#endif
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
    if (img.isNull()) return; 
    if (m_dragPreviewGotFrame) return; 
    m_dragPreviewGotFrame = true; 
    QPixmap pm = QPixmap::fromImage(img); 
    if (pm.isNull()) return; 
    m_dragPreviewPixmap = pm; 
    // Only use thumbnail size if we don't already have actual video dimensions
    if (m_dragPreviewVideoSize.isEmpty()) {
        m_dragPreviewVideoSize = pm.size();
        m_dragPreviewBaseSize = pm.size();
    }
    if (!m_dragPreviewItem) { 
        auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); 
        pmItem->setOpacity(0.0); 
        pmItem->setZValue(5000.0); 
        // Don't set scale here - updateDragPreviewPos will handle it
        if (m_scene) m_scene->addItem(pmItem); 
        m_dragPreviewItem = pmItem; 
        updateDragPreviewPos(m_dragPreviewLastScenePos); 
        startDragPreviewFadeIn(); 
    } else if (auto* pix = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) { 
        pix->setPixmap(m_dragPreviewPixmap); 
        updateDragPreviewPos(m_dragPreviewLastScenePos); 
    }
#ifdef Q_OS_MACOS
    if (m_dragPreviewFallbackDelayTimer) {
        m_dragPreviewFallbackDelayTimer->stop();
    }
    m_dragPreviewPendingVideoPath.clear();
#endif
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    if (m_dragPreviewPlayer) { m_dragPreviewPlayer->stop(); m_dragPreviewPlayer->deleteLater(); m_dragPreviewPlayer = nullptr; }
    if (m_dragPreviewSink) { m_dragPreviewSink->deleteLater(); m_dragPreviewSink = nullptr; }
    if (m_dragPreviewAudio) { m_dragPreviewAudio->deleteLater(); m_dragPreviewAudio = nullptr; }
}

static void updateScreenItemGeometry(QGraphicsRectItem* item,
                                     const ScreenInfo& screen,
                                     int index,
                                     const QRectF& position,
                                     int borderWidthPx,
                                     int labelFontPt) {
    if (!item) return;
    item->setRect(position);
    item->setZValue(-1000.0);
    item->setData(0, index);

    const QColor fill = screen.primary ? QColor(74,144,226,180) : QColor(80,80,80,180);
    const QColor border = screen.primary ? QColor(74,144,226) : QColor(160,160,160);
    item->setBrush(QBrush(fill));
    QPen pen(border);
    pen.setWidth(borderWidthPx);
    item->setPen(pen);

    QGraphicsTextItem* label = nullptr;
    const QList<QGraphicsItem*> children = item->childItems();
    for (QGraphicsItem* child : children) {
        if (auto* text = qgraphicsitem_cast<QGraphicsTextItem*>(child)) {
            label = text;
            break;
        }
    }
    if (!label) {
        label = new QGraphicsTextItem(item);
    }
    label->setDefaultTextColor(Qt::white);
    QFont font("Arial", labelFontPt, QFont::Bold);
    label->setFont(font);
    label->setPlainText(QStringLiteral("Screen %1\n%2×%3").arg(index + 1).arg(screen.width).arg(screen.height));
    QRectF labelRect = label->boundingRect();
    QRectF screenRect = item->rect();
    const QPointF labelPos(screenRect.center().x() - labelRect.width() / 2.0,
                           screenRect.center().y() - labelRect.height() / 2.0);
    label->setPos(labelPos);
}

void ScreenCanvas::createScreenItems() {
    if (!m_scene) return;

    // Incremental update: only rebuild UI zones that changed
    // Clear ONLY UI zones, keep screen items for update in place
    for (auto* it : m_uiZoneItems) {
        if (it && m_scene) {
            m_scene->removeItem(it);
        }
        delete it;
    }
    m_uiZoneItems.clear();

    QMap<int, QRectF> compactPositions = calculateCompactPositions(1.0);
    m_sceneScreenRects.clear();

    // Update existing items in place, only create new ones if needed
    const int oldCount = m_screenItems.size();
    for (int i = 0; i < m_screens.size(); ++i) {
        const ScreenInfo& s = m_screens[i];
        const QRectF pos = compactPositions.value(i);

        QGraphicsRectItem* rect = (i < oldCount) ? m_screenItems.at(i) : nullptr;
        if (!rect) {
            // Only create if doesn't exist
            rect = createScreenItem(s, i, pos);
            rect->setZValue(-1000.0);
            m_scene->addItem(rect);
            if (i < m_screenItems.size()) {
                m_screenItems[i] = rect;
            } else {
                m_screenItems.append(rect);
            }
        }

        // Update geometry without recreating
        updateScreenItemGeometry(rect, s, i, pos, m_screenBorderWidthPx, m_screenLabelFontPt);
        rect->setVisible(true);
        m_sceneScreenRects.insert(s.id, pos);
    }

    // Remove any excess items when screen count shrinks
    while (m_screenItems.size() > m_screens.size()) {
        QGraphicsRectItem* extra = m_screenItems.takeLast();
        if (extra) {
            if (m_scene) m_scene->removeItem(extra);
            delete extra;
        }
    }

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

    // Use the full position rectangle to maintain exact screen positioning without gaps
    QGraphicsRectItem* item = new QGraphicsRectItem(position);

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

QMap<int, QRectF> ScreenCanvas::calculateCompactPositions(double scaleFactor) const {
    // Reflect the OS-defined virtual desktop arrangement exactly.
    // We place screens at their absolute positions relative to the virtual desktop origin (minX/minY normalized to 0).
    QMap<int, QRectF> positions; if (m_screens.isEmpty()) return positions;
    // Compute normalization offsets so the top-left of the virtual desktop maps to (0,0) in scene space
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const auto &s : m_screens) { minX = std::min(minX, s.x); minY = std::min(minY, s.y); }
    if (minX == std::numeric_limits<int>::max()) { minX = 0; }
    if (minY == std::numeric_limits<int>::max()) { minY = 0; }

    for (int i = 0; i < m_screens.size(); ++i) {
        const ScreenInfo& s = m_screens[i];
        const double px = (static_cast<double>(s.x - minX)) * scaleFactor;
        const double py = (static_cast<double>(s.y - minY)) * scaleFactor;
        const double pw = (static_cast<double>(s.width)) * scaleFactor;
        const double ph = (static_cast<double>(s.height)) * scaleFactor;
        positions[i] = QRectF(px, py, pw, ph);
    }
    return positions;
}

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
    // Z-order hierarchy: Media (1-9999) < Remote Cursor (11500) < Selection Chrome (11998-11999.5) < Overlays (≥12000)
    // Remote cursor must stay above all media items regardless of bring-to-front operations
    m_remoteCursorDot->setZValue(11500.0);
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
    // Cap media Z-values at 9999 to keep them below remote cursor (11500) and selection chrome (11998+)
    if (m_nextMediaZValue >= 10000.0) {
        m_nextMediaZValue = 1.0; // Wrap around if limit reached (very unlikely in practice)
    }
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
        
        // Refresh overlay to reflect new Z-order
        refreshInfoOverlay();
        layoutInfoOverlay();
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
        
        // Refresh overlay to reflect new Z-order
        refreshInfoOverlay();
        layoutInfoOverlay();
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

    const QString canvasFontCss = AppColors::canvasButtonFontCss();

    QFont launchSceneFont = m_launchSceneButton->font();
    AppColors::applyCanvasButtonFont(launchSceneFont);
    m_launchSceneButton->setFont(launchSceneFont);

    // Idle (stopped) style: transparent background, overlay text color
    const QString idleStyle = QString(
        "QPushButton { "
        "    padding: 8px 0px; "
        "    %1 "
        "    color: %2; "
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
    ).arg(canvasFontCss, AppColors::colorToCss(AppColors::gOverlayTextColor));

    // Loading style: blue tint background + blue text (like upload button)
    const QString loadingStyle = QString(
        "QPushButton { "
        "    padding: 8px 0px; "
        "    %1 "
        "    color: %2; "
        "    background: %3; "
        "    border: none; "
        "    border-radius: 0px; "
        "}"
    ).arg(canvasFontCss,
          AppColors::gLaunchRemoteSceneLoadingText.name(),
          AppColors::colorToCss(AppColors::gLaunchRemoteSceneLoadingBg));

    // Active (launched) style: magenta tint background + magenta text
    const QString activeStyle = QString(
        "QPushButton { "
        "    padding: 8px 0px; "
        "    %1 "
        "    color: %2; "
        "    background: %3; "
        "    border: none; "
        "    border-radius: 0px; "
        "} "
        "QPushButton:hover { "
        "    color: %2; "
        "    background: %4; "
        "} "
        "QPushButton:pressed { "
        "    color: %2; "
        "    background: %5; "
        "}"
    ).arg(canvasFontCss,
          AppColors::gLaunchRemoteSceneText.name(),
          AppColors::colorToCss(AppColors::gLaunchRemoteSceneBg),
          AppColors::colorToCss(AppColors::gLaunchRemoteSceneHover),
          AppColors::colorToCss(AppColors::gLaunchRemoteScenePressed));

    // Check if upload is in progress
    bool uploadInProgress = m_uploadManager && (m_uploadManager->isUploading() || m_uploadManager->isFinalizing());
    // Check if test scene is active (mutual exclusion)
    bool testSceneActive = m_testSceneLaunched;
    
    if (m_sceneStopping) {
        m_launchSceneButton->setText("Stopping Remote Scene...");
        m_launchSceneButton->setChecked(true);
        m_launchSceneButton->setEnabled(false);
        m_launchSceneButton->setStyleSheet(loadingStyle);
    } else if (m_sceneLaunching) {
        m_launchSceneButton->setText("Launching Remote Scene...");
        m_launchSceneButton->setEnabled(false); // Disable during loading
        m_launchSceneButton->setStyleSheet(loadingStyle);
    } else if (m_sceneLaunched) {
        m_launchSceneButton->setText("Stop Remote Scene");
        m_launchSceneButton->setChecked(true);
        m_launchSceneButton->setStyleSheet(activeStyle);
        m_launchSceneButton->setEnabled(m_overlayActionsEnabled && !uploadInProgress);
    } else {
        m_launchSceneButton->setText("Launch Remote Scene");
        m_launchSceneButton->setChecked(false);
        m_launchSceneButton->setStyleSheet(idleStyle);
        m_launchSceneButton->setEnabled(m_overlayActionsEnabled && !uploadInProgress && !testSceneActive);
    }
    // Greyed style for disabled state (offline, test scene active, upload in progress, or other locks)
    if (!m_sceneLaunching && !m_sceneStopping && !m_launchSceneButton->isEnabled()) {
        m_launchSceneButton->setStyleSheet(overlayDisabledButtonStyle());
    }
    m_launchSceneButton->setFixedHeight(40);
}

void ScreenCanvas::updateLaunchTestSceneButtonStyle() {
    if (!m_launchTestSceneButton) return;

    const QString canvasFontCss = AppColors::canvasButtonFontCss();

    QFont launchTestSceneFont = m_launchTestSceneButton->font();
    AppColors::applyCanvasButtonFont(launchTestSceneFont);
    m_launchTestSceneButton->setFont(launchTestSceneFont);
    // Idle (stopped) style: transparent background, overlay text color
    const QString idleStyle = QString(
        "QPushButton { "
        "    padding: 8px 0px; "
        "    %1 "
        "    color: %2; "
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
    ).arg(canvasFontCss, AppColors::colorToCss(AppColors::gOverlayTextColor));

    // Active (launched) style: test scene magenta variant
    const QString activeStyle = QString(
        "QPushButton { "
        "    padding: 8px 0px; "
        "    %1 "
        "    color: %2; "
        "    background: %3; "
        "    border: none; "
        "    border-radius: 0px; "
        "} "
        "QPushButton:hover { "
        "    color: %2; "
        "    background: %4; "
        "} "
        "QPushButton:pressed { "
        "    color: %2; "
        "    background: %5; "
        "}"
    ).arg(canvasFontCss,
          AppColors::gLaunchTestSceneText.name(),
          AppColors::colorToCss(AppColors::gLaunchTestSceneBg),
          AppColors::colorToCss(AppColors::gLaunchTestSceneHover),
          AppColors::colorToCss(AppColors::gLaunchTestScenePressed));

    // Check if remote scene is active (mutual exclusion with test scene)
    bool remoteSceneActive = m_sceneLaunched || m_sceneLaunching;
    
    if (m_testSceneLaunched) {
        m_launchTestSceneButton->setText("Stop Test Scene");
        m_launchTestSceneButton->setChecked(true);
        m_launchTestSceneButton->setStyleSheet(activeStyle);
        m_launchTestSceneButton->setEnabled(m_overlayActionsEnabled);
    } else {
        m_launchTestSceneButton->setText("Launch Test Scene");
        m_launchTestSceneButton->setChecked(false);
        m_launchTestSceneButton->setStyleSheet(idleStyle);
        m_launchTestSceneButton->setEnabled(m_overlayActionsEnabled && !remoteSceneActive);
    }
    // Greyed style for disabled state (offline or remote scene active)
    if (!m_launchTestSceneButton->isEnabled()) {
        m_launchTestSceneButton->setStyleSheet(overlayDisabledButtonStyle());
    }
    m_launchTestSceneButton->setFixedHeight(40);

}

QString ScreenCanvas::overlayDisabledButtonStyle() {
    return QString(
        "QPushButton { padding:8px 0px; %1 color: rgba(255,255,255,0.4); background: rgba(255,255,255,0.04); border:none; }"
    ).arg(AppColors::canvasButtonFontCss());
}

void ScreenCanvas::setOverlayActionsEnabled(bool enabled) {
    m_overlayActionsEnabled = enabled;

    updateLaunchSceneButtonStyle();
    updateLaunchTestSceneButtonStyle();

    if (!m_uploadButton) return;

    // Ensure upload button keeps bold weight without resetting font size applied via stylesheet
    QFont uploadFont = m_uploadButton->font();
    AppColors::applyCanvasButtonFont(uploadFont);
    m_uploadButton->setFont(uploadFont);

    if (!m_overlayActionsEnabled) {
        m_uploadButton->setEnabled(false);
        m_uploadButton->setCheckable(false);
        m_uploadButton->setChecked(false);
        m_uploadButton->setStyleSheet(overlayDisabledButtonStyle());
        m_uploadButton->setFixedHeight(40);
        m_uploadButton->setMinimumWidth(0);
    } else {
        // Restore normal upload button style when re-enabled
        m_uploadButton->setEnabled(true);
        m_uploadButton->setStyleSheet(
            "QPushButton { "
            "    padding: 8px 0px; "
            "    " + AppColors::canvasButtonFontCss() + " "
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
        m_uploadButton->setMinimumWidth(0);
    }
}

void ScreenCanvas::handleRemoteConnectionLost() {
    const bool remoteFlowActive = m_sceneLaunching || m_sceneLaunched || (m_hostSceneActive && m_hostSceneMode == HostSceneMode::Remote);
    if (!remoteFlowActive) return;

    if (m_sceneLaunchTimeoutTimer && m_sceneLaunchTimeoutTimer->isActive()) {
        m_sceneLaunchTimeoutTimer->stop();
    }
    if (m_sceneStopTimeoutTimer && m_sceneStopTimeoutTimer->isActive()) {
        m_sceneStopTimeoutTimer->stop();
    }

    const bool shouldStopHostScene = (m_hostSceneActive && m_hostSceneMode == HostSceneMode::Remote);
    const bool wasLaunched = m_sceneLaunched;

    m_sceneLaunching = false;
    m_sceneLaunched = false;
    m_sceneStopping = false;

    if (m_launchSceneButton) {
        QSignalBlocker blocker(m_launchSceneButton);
        m_launchSceneButton->setChecked(false);
    }

    if (shouldStopHostScene) {
        // Skip the remote notification so the client keeps its last frame during transient loss.
        stopHostSceneState(false);
    }

    updateLaunchSceneButtonStyle();
    updateLaunchTestSceneButtonStyle();

    if (wasLaunched) {
        emitRemoteSceneLaunchStateChanged();
    }

    TOAST_WARNING("Remote scene stopped: connection lost", 3500);
}

// (Removed legacy duplicate snap indicator drawing functions; SnapGuideItem now handles rendering.)

void ScreenCanvas::startHostSceneState(HostSceneMode mode) {
    if (m_hostSceneActive) return;
    m_hostSceneActive = true;
    m_hostSceneMode = mode;
    m_sceneStopping = false;
    // Capture current selection (only ResizableMediaBase items) before clearing so we can restore later.
    m_prevSelectionBeforeHostScene.clear();
    m_prevVideoStates.clear();
    if (m_scene) {
        const auto selectedNow = m_scene->selectedItems();
        for (QGraphicsItem* it : selectedNow) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                SavedSelection snapshot;
                snapshot.media = media;
                snapshot.guard = media->lifetimeGuard();
                m_prevSelectionBeforeHostScene.append(snapshot);
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
                const bool shouldAutoDisplay = media->autoDisplayEnabled();
                const int displayDelayMs = media->autoDisplayDelayMs();
                const bool shouldAutoPlay = media->autoPlayEnabled();
                const int playDelayMs = media->autoPlayDelayMs();
                const bool shouldAutoHide = media->autoHideEnabled();
                const int hideDelayMs = media->autoHideDelayMs();
                const bool hideOnEnd = media->hideWhenVideoEnds();
                const bool muteOnEnd = media->muteWhenVideoEnds();
                const bool scheduleHideFromDisplay = shouldAutoHide && !hideOnEnd;
                // Snapshot and reset videos
                if (auto* vid = dynamic_cast<ResizableVideoItem*>(media)) {
                    m_prevVideoStates.append(VideoPreState{});
                    VideoPreState& videoState = m_prevVideoStates.last();
                    videoState.video = vid;
                    videoState.guard = vid->lifetimeGuard();
                    videoState.posMs = vid->currentPositionMs();
                    videoState.wasPlaying = vid->isPlaying();
                    videoState.wasMuted = vid->isMuted();

                    const bool shouldAutoMute = media->autoMuteEnabled();
                    const int muteDelayMs = media->autoMuteDelayMs();
                    const bool shouldAutoUnmute = media->autoUnmuteEnabled();
                    const int unmuteDelayMs = media->autoUnmuteDelayMs();
                    QMediaPlayer* player = vid->mediaPlayer();

                    if ((hideOnEnd || muteOnEnd) && player) {
                        auto hideTriggered = std::make_shared<bool>(false);
                        auto triggerHide = [this, media, videoPtr = vid, guard = videoState.guard, hideTriggered]() {
                            if (*hideTriggered) return;
                            if (!m_hostSceneActive) return;
                            if (guard.expired()) return;
                            if (!media || media->isBeingDeleted()) return;
                            const auto currentState = media->mediaSettingsState();
                            if (!currentState.hideWhenVideoEnds) return;
                            // Don't hide if repeats are still pending
                            if (videoPtr && videoPtr->settingsRepeatAvailable()) return;
                            *hideTriggered = true;
                            media->hideWithConfiguredFade();
                        };

                        QTimer* hideAfterEndTimer = nullptr;
                        if (hideOnEnd && shouldAutoHide && hideDelayMs > 0) {
                            hideAfterEndTimer = new QTimer(this);
                            hideAfterEndTimer->setSingleShot(true);
                            videoState.hideDelayTimer = hideAfterEndTimer;
                            connect(hideAfterEndTimer, &QTimer::timeout, this, [triggerHide]() { triggerHide(); });
                        }
                        QPointer<QTimer> hideTimerPtr = hideAfterEndTimer;

                        if (hideOnEnd) {
                            videoState.hideOnEndConnection = QObject::connect(player, &QMediaPlayer::mediaStatusChanged, this,
                                [this, triggerHide, hideTimerPtr, hideDelayMs, shouldAutoHide](QMediaPlayer::MediaStatus status) {
                                    if (!m_hostSceneActive) return;
                                    if (status != QMediaPlayer::EndOfMedia) return;
                                    if (shouldAutoHide && hideDelayMs > 0 && hideTimerPtr) {
                                        hideTimerPtr->stop();
                                        hideTimerPtr->start(hideDelayMs);
                                    } else {
                                        triggerHide();
                                    }
                                });

                            if (shouldAutoHide && hideDelayMs < 0) {
                                const qint64 offsetMs = -static_cast<qint64>(hideDelayMs);
                                QPointer<QMediaPlayer> playerPtr(player);
                                auto preEndHideCheck = [this, playerPtr, triggerHide, offsetMs]() {
                                    if (!m_hostSceneActive) return;
                                    if (!playerPtr) return;
                                    const qint64 duration = playerPtr->duration();
                                    if (duration <= 0) return;
                                    const qint64 position = playerPtr->position();
                                    if (position < 0) return;
                                    if ((duration - position) <= offsetMs) {
                                        triggerHide();
                                    }
                                };
                                videoState.hidePreEndPositionConnection = QObject::connect(player, &QMediaPlayer::positionChanged, this, [preEndHideCheck](qint64) { preEndHideCheck(); });
                                videoState.hidePreEndDurationConnection = QObject::connect(player, &QMediaPlayer::durationChanged, this, [preEndHideCheck](qint64) { preEndHideCheck(); });
                            }
                        }

                        auto muteTriggered = std::make_shared<bool>(false);
                        auto triggerMute = [this, videoPtr = vid, guard = videoState.guard, muteTriggered]() {
                            if (*muteTriggered) return;
                            if (!m_hostSceneActive) return;
                            if (guard.expired()) return;
                            if (!videoPtr || videoPtr->isBeingDeleted()) return;
                            const auto currentState = videoPtr->mediaSettingsState();
                            if (!currentState.muteWhenVideoEnds) return;
                            // Don't mute if repeats are still pending
                            if (videoPtr->settingsRepeatAvailable()) return;
                            *muteTriggered = true;
                            videoPtr->setMuted(true);
                        };

                        QTimer* muteAfterEndTimer = nullptr;
                        if (muteOnEnd && shouldAutoMute && muteDelayMs > 0) {
                            muteAfterEndTimer = new QTimer(this);
                            muteAfterEndTimer->setSingleShot(true);
                            videoState.muteDelayTimer = muteAfterEndTimer;
                            connect(muteAfterEndTimer, &QTimer::timeout, this, [triggerMute]() { triggerMute(); });
                        }
                        QPointer<QTimer> muteTimerPtr = muteAfterEndTimer;

                        if (muteOnEnd) {
                            videoState.muteOnEndConnection = QObject::connect(player, &QMediaPlayer::mediaStatusChanged, this,
                                [this, triggerMute, muteTimerPtr, muteDelayMs, shouldAutoMute](QMediaPlayer::MediaStatus status) {
                                    if (!m_hostSceneActive) return;
                                    if (status != QMediaPlayer::EndOfMedia) return;
                                    if (shouldAutoMute && muteDelayMs > 0 && muteTimerPtr) {
                                        muteTimerPtr->stop();
                                        muteTimerPtr->start(muteDelayMs);
                                    } else {
                                        triggerMute();
                                    }
                                });

                            if (shouldAutoMute && muteDelayMs < 0) {
                                const qint64 offsetMs = -static_cast<qint64>(muteDelayMs);
                                QPointer<QMediaPlayer> playerPtr(player);
                                auto preEndMuteCheck = [this, playerPtr, triggerMute, offsetMs]() {
                                    if (!m_hostSceneActive) return;
                                    if (!playerPtr) return;
                                    const qint64 duration = playerPtr->duration();
                                    if (duration <= 0) return;
                                    const qint64 position = playerPtr->position();
                                    if (position < 0) return;
                                    if ((duration - position) <= offsetMs) {
                                        triggerMute();
                                    }
                                };
                                videoState.mutePreEndPositionConnection = QObject::connect(player, &QMediaPlayer::positionChanged, this, [preEndMuteCheck](qint64) { preEndMuteCheck(); });
                                videoState.mutePreEndDurationConnection = QObject::connect(player, &QMediaPlayer::durationChanged, this, [preEndMuteCheck](qint64) { preEndMuteCheck(); });
                            }
                        }
                    }

                    vid->pauseAndSetPosition(videoState.posMs);

                    vid->setMuted(true, true);
                    if (shouldAutoUnmute) {
                        const auto unmuteGuard = vid->lifetimeGuard();
                        ResizableVideoItem* videoPtr = vid;
                        auto unmuteNow = [this, videoPtr, unmuteGuard]() {
                            if (!m_hostSceneActive) return;
                            if (unmuteGuard.expired()) return;
                            if (!videoPtr || videoPtr->isBeingDeleted()) return;
                            const auto state = videoPtr->mediaSettingsState();
                            if (!state.unmuteAutomatically) return;
                            videoPtr->setMuted(false);
                        };
                        if (unmuteDelayMs > 0) {
                            QTimer::singleShot(unmuteDelayMs, this, unmuteNow);
                        } else {
                            QTimer::singleShot(0, this, unmuteNow);
                        }
                    }

                    if (shouldAutoMute && !muteOnEnd) {
                        const auto muteGuard = vid->lifetimeGuard();
                        ResizableVideoItem* videoPtr = vid;
                        const int delay = std::max(0, muteDelayMs);
                        QTimer::singleShot(delay, this, [this, videoPtr, muteGuard]() {
                            if (!m_hostSceneActive) return;
                            if (muteGuard.expired()) return;
                            if (!videoPtr || videoPtr->isBeingDeleted()) return;
                            const auto state = videoPtr->mediaSettingsState();
                            if (!state.muteDelayEnabled) return;
                            videoPtr->setMuted(true);
                        });
                    }
                }
                media->hideImmediateNoFade();
                // 1. Schedule (or immediate) display
                if (shouldAutoDisplay) {
                    const auto displayGuard = media->lifetimeGuard();
                    if (displayDelayMs > 0) {
                        QTimer::singleShot(displayDelayMs, this, [this, media, displayGuard, scheduleHideFromDisplay, hideDelayMs]() {
                            if (!m_hostSceneActive) return;
                            if (displayGuard.expired()) return;
                            if (media->isBeingDeleted()) return;
                            media->showWithConfiguredFade();
                            if (scheduleHideFromDisplay) {
                                const auto hideGuard = media->lifetimeGuard();
                                QTimer::singleShot(std::max(0, hideDelayMs), this, [this, media, hideGuard]() {
                                    if (!m_hostSceneActive) return;
                                    if (hideGuard.expired()) return;
                                    if (!media || media->isBeingDeleted()) return;
                                    media->hideWithConfiguredFade();
                                });
                            }
                        });
                    } else {
                        media->showWithConfiguredFade();
                        if (scheduleHideFromDisplay) {
                            const auto hideGuard = media->lifetimeGuard();
                            QTimer::singleShot(std::max(0, hideDelayMs), this, [this, media, hideGuard]() {
                                if (!m_hostSceneActive) return;
                                if (hideGuard.expired()) return;
                                if (!media || media->isBeingDeleted()) return;
                                media->hideWithConfiguredFade();
                            });
                        }
                    }
                }
                // 2. Schedule video playback if autoPlay is enabled; allow playback even while hidden
                if (shouldAutoPlay && media->isVideoMedia()) {
                    const auto playbackGuard = media->lifetimeGuard();
                    const int playbackDelay = std::max(0, playDelayMs);
                    const bool shouldAutoPause = media->autoPauseEnabled();
                    const int pauseDelayMs = media->autoPauseDelayMs();
                    auto startPlayback = [this, media, playbackGuard, shouldAutoPause, pauseDelayMs]() {
                        if (!m_hostSceneActive) return;
                        if (playbackGuard.expired()) return;
                        if (media->isBeingDeleted()) return;
                        if (auto* vid = dynamic_cast<ResizableVideoItem*>(media)) {
                            if (!vid->isPlaying()) {
                                // Initialize repeat session before starting playback
                                vid->initializeSettingsRepeatSessionForPlaybackStart();
                                vid->togglePlayPause();
                                // Schedule pause if enabled
                                if (shouldAutoPause) {
                                    const auto pauseGuard = media->lifetimeGuard();
                                    QTimer::singleShot(std::max(0, pauseDelayMs), this, [this, media, pauseGuard]() {
                                        if (!m_hostSceneActive) return;
                                        if (pauseGuard.expired()) return;
                                        if (!media || media->isBeingDeleted()) return;
                                        if (auto* vid = dynamic_cast<ResizableVideoItem*>(media)) {
                                            if (vid->isPlaying()) {
                                                vid->togglePlayPause();
                                            }
                                        }
                                    });
                                }
                            }
                        }
                    };
                    if (playbackDelay > 0) {
                        QTimer::singleShot(playbackDelay, this, startPlayback);
                    } else {
                        startPlayback();
                    }
                }
            }
        }
    }
}

void ScreenCanvas::stopHostSceneState(bool notifyRemote) {
    if (m_sceneStopTimeoutTimer) {
        m_sceneStopTimeoutTimer->stop();
    }
    m_sceneStopping = false;

    if (!m_hostSceneActive) return;
    m_hostSceneActive = false;
    HostSceneMode prevMode = m_hostSceneMode;
    m_hostSceneMode = HostSceneMode::None;
    // Restore visibility of media items (leave videos stopped).
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(gi)) media->showImmediateNoFade();
        }
        // Restore previous selection (only items that still exist in the scene)
        for (const auto& mediaPtr : std::as_const(m_prevSelectionBeforeHostScene)) {
            if (!mediaPtr.media) continue;
            if (mediaPtr.guard.expired()) continue;
            if (mediaPtr.media->isBeingDeleted()) continue;
            if (mediaPtr.media->scene() == m_scene) {
                mediaPtr.media->setSelected(true);
            }
        }
        m_prevSelectionBeforeHostScene.clear();
    }
    // Restore pre-scene video positions (always paused)
    for (VideoPreState& st : m_prevVideoStates) {
        if (st.hideDelayTimer) {
            st.hideDelayTimer->stop();
            st.hideDelayTimer->deleteLater();
            st.hideDelayTimer = nullptr;
        }
        if (st.muteDelayTimer) {
            st.muteDelayTimer->stop();
            st.muteDelayTimer->deleteLater();
            st.muteDelayTimer = nullptr;
        }
        if (!st.video) continue;
        if (st.guard.expired()) continue;
        if (st.video->isBeingDeleted()) continue;
        if (st.video->scene() == m_scene) {
            if (st.hideOnEndConnection) {
                QObject::disconnect(st.hideOnEndConnection);
                st.hideOnEndConnection = QMetaObject::Connection();
            }
            if (st.hidePreEndPositionConnection) {
                QObject::disconnect(st.hidePreEndPositionConnection);
                st.hidePreEndPositionConnection = QMetaObject::Connection();
            }
            if (st.hidePreEndDurationConnection) {
                QObject::disconnect(st.hidePreEndDurationConnection);
                st.hidePreEndDurationConnection = QMetaObject::Connection();
            }
            if (st.muteOnEndConnection) {
                QObject::disconnect(st.muteOnEndConnection);
                st.muteOnEndConnection = QMetaObject::Connection();
            }
            if (st.mutePreEndPositionConnection) {
                QObject::disconnect(st.mutePreEndPositionConnection);
                st.mutePreEndPositionConnection = QMetaObject::Connection();
            }
            if (st.mutePreEndDurationConnection) {
                QObject::disconnect(st.mutePreEndDurationConnection);
                st.mutePreEndDurationConnection = QMetaObject::Connection();
            }
            st.video->pauseAndSetPosition(st.posMs);
            st.video->setMuted(st.wasMuted);
        }
    }
    m_prevVideoStates.clear();

    // Notify remote client to stop scene only if Remote mode was active
    if (prevMode == HostSceneMode::Remote) {
        bool wasLaunched = m_sceneLaunched;
        m_sceneLaunched = false;
        if (wasLaunched) {
            emitRemoteSceneLaunchStateChanged();
        }
        if (notifyRemote && m_wsClient && !m_remoteSceneTargetClientId.isEmpty()) {
            qDebug() << "ScreenCanvas: sending remote_scene_stop to" << m_remoteSceneTargetClientId;
            m_wsClient->sendRemoteSceneStop(m_remoteSceneTargetClientId);
        }
    }
}

void ScreenCanvas::ensureSettingsToggleButton() {
    if (m_settingsToggleButton || !viewport()) return;

    m_settingsToggleButton = new QToolButton(viewport());
    m_settingsToggleButton->setIcon(QIcon(QStringLiteral(":/icons/icons/settings.svg")));
    m_settingsToggleButton->setObjectName("SettingsToggleButton");
    m_settingsToggleButton->setCheckable(true);
    m_settingsToggleButton->setToolTip(tr("Settings"));
    m_settingsToggleButton->setAttribute(Qt::WA_NoMousePropagation, true);
    m_settingsToggleButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_settingsToggleButton->setAutoRaise(false);
    m_settingsToggleButton->setFocusPolicy(Qt::NoFocus);
    m_settingsToggleButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_settingsToggleButton->setAccessibleName(tr("Media settings"));

    const QString baseBg = AppColors::colorToCss(AppColors::gOverlayBackgroundColor);
    const QString activeBg = AppColors::colorToCss(AppColors::gOverlayActiveBackgroundColor);
    const QString borderColor = AppColors::colorToCss(AppColors::gOverlayBorderColor);
    QColor disabledColor = AppColors::gOverlayBackgroundColor;
    disabledColor.setAlphaF(std::clamp(disabledColor.alphaF() * 0.35, 0.0, 1.0));
    const QString disabledBg = AppColors::colorToCss(disabledColor);

    const QString cornerRadiusPx = QString::number(gOverlayCornerRadiusPx) + QStringLiteral("px");
    const QString style = QStringLiteral(
        "QToolButton#SettingsToggleButton {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: %3;"
        " padding: 0;"
        " margin: 0;"
        "}"
        "QToolButton#SettingsToggleButton:hover:!disabled:!checked { background-color: %1; }"
        "QToolButton#SettingsToggleButton:pressed { background-color: %4; }"
        "QToolButton#SettingsToggleButton:checked { background-color: %4; }"
        "QToolButton#SettingsToggleButton:checked:hover { background-color: %4; }"
        "QToolButton#SettingsToggleButton:disabled { background-color: %5; border: 1px solid %2; }"
    ).arg(baseBg, borderColor, cornerRadiusPx, activeBg, disabledBg);
    m_settingsToggleButton->setStyleSheet(style);

    connect(m_settingsToggleButton, &QToolButton::toggled, this, [this](bool checked) {
        m_settingsPanelPreferredVisible = checked;
        updateGlobalSettingsPanelVisibility();
    });

    m_settingsToggleButton->show();
}

void ScreenCanvas::ensureToolSelector() {
    if (m_toolSelectorContainer || !viewport()) return;

    // Create container for segmented control
    m_toolSelectorContainer = new QWidget(viewport());
    m_toolSelectorContainer->setAttribute(Qt::WA_NoMousePropagation, true);
    m_toolSelectorContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    
    QHBoxLayout* layout = new QHBoxLayout(m_toolSelectorContainer);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0); // Fused segments, no spacing
    layout->setSizeConstraint(QLayout::SetFixedSize);
    
    // Selection Tool button (left segment)
    m_selectionToolButton = new QToolButton(m_toolSelectorContainer);
    m_selectionToolButton->setIcon(QIcon(QStringLiteral(":/icons/icons/tools/selection-tool.svg")));
    m_selectionToolButton->setObjectName("SelectionToolButton");
    m_selectionToolButton->setCheckable(true);
    m_selectionToolButton->setChecked(true); // Default active tool
    m_selectionToolButton->setToolTip(tr("Selection Tool"));
    m_selectionToolButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_selectionToolButton->setAutoRaise(false);
    m_selectionToolButton->setFocusPolicy(Qt::NoFocus);
    m_selectionToolButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_selectionToolButton->setCursor(Qt::PointingHandCursor);
    
    // Text Tool button (right segment)
    m_textToolButton = new QToolButton(m_toolSelectorContainer);
    m_textToolButton->setIcon(QIcon(QStringLiteral(":/icons/icons/tools/text-tool.svg")));
    m_textToolButton->setObjectName("TextToolButton");
    m_textToolButton->setCheckable(true);
    m_textToolButton->setChecked(false);
    m_textToolButton->setToolTip(tr("Text Tool"));
    m_textToolButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_textToolButton->setAutoRaise(false);
    m_textToolButton->setFocusPolicy(Qt::NoFocus);
    m_textToolButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_textToolButton->setCursor(Qt::PointingHandCursor);
    
    // Visual divider between segments
    QFrame* divider = new QFrame(m_toolSelectorContainer);
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedWidth(1);
    divider->setStyleSheet(QStringLiteral("background-color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayBorderColor)));
    
    // Apply segmented control styling
    const QString baseBg = AppColors::colorToCss(AppColors::gOverlayBackgroundColor);
    const QString activeBg = AppColors::colorToCss(AppColors::gOverlayActiveBackgroundColor);
    const QString borderColor = AppColors::colorToCss(AppColors::gOverlayBorderColor);
    const QString cornerRadiusPx = QString::number(gOverlayCornerRadiusPx) + QStringLiteral("px");
    
    // Left segment style (rounded left corners only)
    const QString leftStyle = QStringLiteral(
        "QToolButton#SelectionToolButton {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-top-left-radius: %3;"
        " border-bottom-left-radius: %3;"
        " border-top-right-radius: 0px;"
        " border-bottom-right-radius: 0px;"
        " border-right: none;"
        " padding: 0;"
        " margin: 0;"
        "}"
        "QToolButton#SelectionToolButton:hover:!disabled:!checked { background-color: %1; }"
        "QToolButton#SelectionToolButton:pressed { background-color: %4; }"
        "QToolButton#SelectionToolButton:checked { background-color: %4; }"
        "QToolButton#SelectionToolButton:checked:hover { background-color: %4; }"
    ).arg(baseBg, borderColor, cornerRadiusPx, activeBg);
    
    // Right segment style (rounded right corners only)
    const QString rightStyle = QStringLiteral(
        "QToolButton#TextToolButton {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-top-left-radius: 0px;"
        " border-bottom-left-radius: 0px;"
        " border-top-right-radius: %3;"
        " border-bottom-right-radius: %3;"
        " padding: 0;"
        " margin: 0;"
        "}"
        "QToolButton#TextToolButton:hover:!disabled:!checked { background-color: %1; }"
        "QToolButton#TextToolButton:pressed { background-color: %4; }"
        "QToolButton#TextToolButton:checked { background-color: %4; }"
        "QToolButton#TextToolButton:checked:hover { background-color: %4; }"
    ).arg(baseBg, borderColor, cornerRadiusPx, activeBg);
    
    m_selectionToolButton->setStyleSheet(leftStyle);
    m_textToolButton->setStyleSheet(rightStyle);
    
    // Add widgets to layout
    layout->addWidget(m_selectionToolButton);
    layout->addWidget(divider);
    layout->addWidget(m_textToolButton);
    
    // Mutual exclusivity: only one tool can be active
    connect(m_selectionToolButton, &QToolButton::clicked, this, [this]() {
        m_selectionToolButton->setChecked(true);
        m_textToolButton->setChecked(false);
        // TODO: Trigger selection tool activation
    });
    connect(m_textToolButton, &QToolButton::clicked, this, [this]() {
        m_textToolButton->setChecked(true);
        m_selectionToolButton->setChecked(false);
        // TODO: Trigger text tool activation
    });

    m_toolSelectorContainer->show();
}

void ScreenCanvas::updateSettingsToggleButtonGeometry() {
    if (!viewport()) return;
    ensureSettingsToggleButton();
    if (!m_settingsToggleButton) return;

    const int margin = 16;
    const int spacing = 10;
    int buttonSize = ResizableMediaBase::getHeightOfMediaOverlaysPx();
    if (buttonSize <= 0) buttonSize = 36;
    buttonSize = std::max(buttonSize, 24);
    // Overlay buttons paint a 1px stroke that straddles the rect bounds; compensate so the
    // visible outer size matches the graphics overlay squares.
    buttonSize += 2;

    int iconSize = static_cast<int>(std::round(buttonSize * 0.6));
    const int maxIcon = std::max(16, buttonSize - 4);
    iconSize = std::clamp(iconSize, 16, maxIcon);

    m_settingsToggleButton->setFixedSize(buttonSize, buttonSize);
    m_settingsToggleButton->setIconSize(QSize(iconSize, iconSize));
    m_settingsToggleButton->move(margin, margin);
    m_settingsToggleButton->raise();
    m_settingsToggleButton->show();

    if (m_globalSettingsPanel) {
        const int panelTop = margin + buttonSize + spacing;
        const int bottomMargin = margin;
        m_globalSettingsPanel->setAnchorMargins(margin, panelTop, bottomMargin);
        if (m_globalSettingsPanel->isVisible()) {
            m_globalSettingsPanel->updatePosition();
        }
    }

    updateToolSelectorGeometry();
}

void ScreenCanvas::updateToolSelectorGeometry() {
    if (!viewport()) return;
    ensureToolSelector();
    if (!m_toolSelectorContainer || !m_settingsToggleButton) return;

    const int margin = 16;
    const int spacing = 10;
    int buttonSize = ResizableMediaBase::getHeightOfMediaOverlaysPx();
    if (buttonSize <= 0) buttonSize = 36;
    buttonSize = std::max(buttonSize, 24);
    // Match settings button compensation
    buttonSize += 2;

    int iconSize = static_cast<int>(std::round(buttonSize * 0.6));
    const int maxIcon = std::max(16, buttonSize - 4);
    iconSize = std::clamp(iconSize, 16, maxIcon);

    // Size each button in the segmented control
    m_selectionToolButton->setFixedSize(buttonSize, buttonSize);
    m_selectionToolButton->setIconSize(QSize(iconSize, iconSize));
    m_textToolButton->setFixedSize(buttonSize, buttonSize);
    m_textToolButton->setIconSize(QSize(iconSize, iconSize));

    const int dividerWidth = 1; // matches divider->setFixedWidth
    const int totalWidth = (buttonSize * 2) + dividerWidth;
    m_toolSelectorContainer->setFixedSize(totalWidth, buttonSize);
    
    // Position container to the right of settings button with spacing
    int settingsButtonRight = m_settingsToggleButton->x() + m_settingsToggleButton->width();
    m_toolSelectorContainer->move(settingsButtonRight + spacing, margin);
    m_toolSelectorContainer->raise();
    m_toolSelectorContainer->show();
}

void ScreenCanvas::updateGlobalSettingsPanelVisibility() {
    if (!m_globalSettingsPanel) return;
    ensureSettingsToggleButton();
    updateSettingsToggleButtonGeometry();
    updateToolSelectorGeometry();
    
    // Find currently selected media item
    ResizableMediaBase* selectedMedia = nullptr;
    if (m_scene) {
        for (QGraphicsItem* gi : m_scene->items()) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(gi)) {
                if (media->isSelected()) {
                    selectedMedia = media;
                    break;
                }
            }
        }
    }
    
    // Update button checked state to match preference
    if (m_settingsToggleButton) {
        if (m_settingsToggleButton->isChecked() != m_settingsPanelPreferredVisible) {
            QSignalBlocker blocker(m_settingsToggleButton);
            m_settingsToggleButton->setChecked(m_settingsPanelPreferredVisible);
        }
    }

    if (!selectedMedia) {
        // No media selected - hide panel but keep button state
        m_globalSettingsPanel->setMediaItem(nullptr);
        m_globalSettingsPanel->setVisible(false);
        return;
    }

    // Media selected - show panel if button is checked
    m_globalSettingsPanel->setMediaType(selectedMedia->isVideoMedia());
    m_globalSettingsPanel->setMediaItem(selectedMedia);

    const bool shouldShowPanel = !m_settingsToggleButton || m_settingsToggleButton->isChecked();
    m_globalSettingsPanel->setVisible(shouldShowPanel);
    if (shouldShowPanel) {
        m_globalSettingsPanel->updatePosition();
    }
}

void ScreenCanvas::refreshSettingsPanelVolumeDisplay() {
    if (m_globalSettingsPanel && m_globalSettingsPanel->isVisible()) {
        m_globalSettingsPanel->refreshVolumeDisplay();
    }
}

void ScreenCanvas::setWebSocketClient(WebSocketClient* client) {
    // Disconnect old client if any
    if (m_wsClient) {
        disconnect(m_wsClient, &WebSocketClient::remoteSceneValidationReceived, this, &ScreenCanvas::onRemoteSceneValidationReceived);
        disconnect(m_wsClient, &WebSocketClient::remoteSceneLaunchedReceived, this, &ScreenCanvas::onRemoteSceneLaunchedReceived);
        disconnect(m_wsClient, &WebSocketClient::remoteSceneStoppedReceived, this, &ScreenCanvas::onRemoteSceneStoppedReceived);
    }
    
    m_wsClient = client;
    
    // Connect new client signals
    if (m_wsClient) {
        connect(m_wsClient, &WebSocketClient::remoteSceneValidationReceived, this, &ScreenCanvas::onRemoteSceneValidationReceived);
        connect(m_wsClient, &WebSocketClient::remoteSceneLaunchedReceived, this, &ScreenCanvas::onRemoteSceneLaunchedReceived);
        connect(m_wsClient, &WebSocketClient::remoteSceneStoppedReceived, this, &ScreenCanvas::onRemoteSceneStoppedReceived);
    }
}

void ScreenCanvas::setUploadManager(UploadManager* manager) {
    // Disconnect old manager if any
    if (m_uploadManager) {
        disconnect(m_uploadManager, &UploadManager::uiStateChanged, this, &ScreenCanvas::updateLaunchSceneButtonStyle);
    }
    
    m_uploadManager = manager;
    
    // Connect new manager signals
    if (m_uploadManager) {
        connect(m_uploadManager, &UploadManager::uiStateChanged, this, &ScreenCanvas::updateLaunchSceneButtonStyle);
    }
}

void ScreenCanvas::emitRemoteSceneLaunchStateChanged()
{
    emit remoteSceneLaunchStateChanged(m_sceneLaunched, m_remoteSceneTargetClientId, m_remoteSceneTargetMachineName);
    // Trigger upload button update to reflect remote scene state (disabled when scene is active)
    if (m_uploadManager) {
        emit m_uploadManager->uiStateChanged();
    }
}

void ScreenCanvas::onRemoteSceneValidationReceived(const QString& targetClientId, bool success, const QString& errorMessage) {
    // Only handle if this is a response to our request
    if (targetClientId != m_remoteSceneTargetClientId) return;
    if (!m_sceneLaunching) return; // Ignore if not in launching state
    
    if (success) {
        // Validation successful - scene is being prepared on remote
        TOAST_SUCCESS("Remote client validated scene successfully", 2000);
        if (!m_hostSceneActive) {
            startHostSceneState(HostSceneMode::Remote);
        }
        // Keep loading state - wait for final "launched" confirmation
        // Restart timeout for the launch phase
        if (m_sceneLaunchTimeoutTimer && m_sceneLaunchTimeoutTimer->isActive()) {
            m_sceneLaunchTimeoutTimer->start(REMOTE_SCENE_LAUNCH_TIMEOUT_MS);
        }
    } else {
        // Stop timeout timer
        if (m_sceneLaunchTimeoutTimer) {
            m_sceneLaunchTimeoutTimer->stop();
        }
        
        // Validation failed - abort launch
        m_sceneLaunching = false;
        const bool wasLaunched = m_sceneLaunched;
        m_sceneLaunched = false;
        if (m_launchTestSceneButton) m_launchTestSceneButton->setEnabled(true);
        updateLaunchSceneButtonStyle();
        updateLaunchTestSceneButtonStyle();
        
        // Stop local scene too
        stopHostSceneState();
        if (wasLaunched) {
            emitRemoteSceneLaunchStateChanged();
        }
        
        QString message = errorMessage.isEmpty() ? "Scene validation failed" : errorMessage;
        TOAST_ERROR(QString("Scene launch failed: %1").arg(message), 4000);
    }
}

void ScreenCanvas::onRemoteSceneLaunchedReceived(const QString& targetClientId) {
    // Only handle if this is a response to our request
    if (targetClientId != m_remoteSceneTargetClientId) return;
    if (!m_sceneLaunching) return; // Ignore if not in launching state
    
    // Stop timeout timer
    if (m_sceneLaunchTimeoutTimer) {
        m_sceneLaunchTimeoutTimer->stop();
    }
    
    // Scene successfully launched on remote - exit loading state
    m_sceneLaunching = false;
    m_sceneLaunched = true;
    updateLaunchSceneButtonStyle();
    updateLaunchTestSceneButtonStyle(); // Update test scene button (remains disabled while remote scene is active)
    emitRemoteSceneLaunchStateChanged();
    
    TOAST_SUCCESS("Remote scene launched successfully!", 3000);
}

void ScreenCanvas::onRemoteSceneLaunchTimeout() {
    if (!m_sceneLaunching) return; // Ignore if not in launching state
    
    qWarning() << "Remote scene launch timed out after" << REMOTE_SCENE_LAUNCH_TIMEOUT_MS << "ms";
    
    // Timeout occurred - abort launch
    m_sceneLaunching = false;
    const bool wasLaunched = m_sceneLaunched;
    m_sceneLaunched = false;
    if (m_launchTestSceneButton) m_launchTestSceneButton->setEnabled(true);
    updateLaunchSceneButtonStyle();
    updateLaunchTestSceneButtonStyle();
    
    // Stop local scene too
    stopHostSceneState();
    if (wasLaunched) {
        emitRemoteSceneLaunchStateChanged();
    }
    
    TOAST_ERROR("Scene launch timed out: Remote client did not respond", 5000);
}

void ScreenCanvas::onRemoteSceneStoppedReceived(const QString& targetClientId, bool success, const QString& errorMessage) {
    if (targetClientId != m_remoteSceneTargetClientId) return;

    if (m_sceneStopTimeoutTimer) {
        m_sceneStopTimeoutTimer->stop();
    }

    const bool wasStopping = m_sceneStopping;
    m_sceneStopping = false;

    if (!success) {
        const QString message = errorMessage.isEmpty() ? QStringLiteral("Remote client failed to stop the scene") : errorMessage;
        TOAST_ERROR(message, 4000);
        updateLaunchSceneButtonStyle();
        updateLaunchTestSceneButtonStyle();
        return;
    }

    // Ensure local host scene halts without sending another stop request
    stopHostSceneState(false);
    if (!m_hostSceneActive && m_sceneLaunched) {
        m_sceneLaunched = false;
        emitRemoteSceneLaunchStateChanged();
    }
    updateLaunchSceneButtonStyle();
    updateLaunchTestSceneButtonStyle();

    if (wasStopping) {
        TOAST_SUCCESS("Remote scene stopped successfully", 3000);
    } else {
        TOAST_INFO("Remote scene stopped", 2500);
    }
}

void ScreenCanvas::onRemoteSceneStopTimeout() {
    if (!m_sceneStopping) return;
    m_sceneStopping = false;
    TOAST_ERROR("Scene stop timed out: Remote client did not respond", 5000);
    updateLaunchSceneButtonStyle();
    updateLaunchTestSceneButtonStyle();
}

void ScreenCanvas::updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients) {
    // If we have a target machine name set, update the client ID if the machine reconnected
    if (m_remoteSceneTargetMachineName.isEmpty()) return;
    
    for (const ClientInfo& client : clients) {
        if (client.getMachineName() == m_remoteSceneTargetMachineName) {
            // Found the same machine - update the ID if it changed
            if (client.getId() != m_remoteSceneTargetClientId) {
                qDebug() << "ScreenCanvas: updating remote scene target ID from" << m_remoteSceneTargetClientId
                         << "to" << client.getId() << "for machine" << m_remoteSceneTargetMachineName;
                m_remoteSceneTargetClientId = client.getId();
            }
            return;
        }
    }
    
    // Machine not found in current client list - it's disconnected
    // Keep the stored info in case it reconnects later
}
