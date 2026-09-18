#include "frontend/rendering/canvas/QuickCanvasController.h"

#include "backend/config/AppConfig.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "backend/media/MediaResidencyManager.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QFileInfo>
#include <QMetaObject>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr auto kCanvasClipboardMime = "application/x-mouffette-media-v4";
constexpr qreal kSnapDistancePx = 10.0;
constexpr qreal kCornerSnapDistancePx = 20.0;
constexpr qreal kSnapReleaseFactor = 1.4;
constexpr qreal kSnapEpsilon = 1e-5;

QVariantMap guide(qreal x1, qreal y1, qreal x2, qreal y2)
{
    return {{QStringLiteral("x1"), x1}, {QStringLiteral("y1"), y1},
            {QStringLiteral("x2"), x2}, {QStringLiteral("y2"), y2}};
}

QRectF allScreenBounds(const CanvasDocument* document)
{
    QRectF bounds;
    bool first = true;
    if (!document) return bounds;
    for (const QRectF& rect : document->screenRects()) {
        bounds = first ? rect : bounds.united(rect);
        first = false;
    }
    return bounds;
}

struct ResizeHandleAxes {
    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;

    bool movesX() const { return left || right; }
    bool movesY() const { return top || bottom; }
    bool corner() const { return movesX() && movesY(); }
};

ResizeHandleAxes resizeHandleAxes(const QString& handle)
{
    return {handle.contains(QLatin1String("left")),
            handle.contains(QLatin1String("right")),
            handle.contains(QLatin1String("top")),
            handle.contains(QLatin1String("bottom"))};
}

QPointF movingHandlePoint(const QRectF& rect, const ResizeHandleAxes& axes)
{
    return {axes.left ? rect.left() : (axes.right ? rect.right() : rect.center().x()),
            axes.top ? rect.top() : (axes.bottom ? rect.bottom() : rect.center().y())};
}

QPointF fixedHandlePoint(const QRectF& rect, const ResizeHandleAxes& axes)
{
    return {axes.left ? rect.right() : (axes.right ? rect.left() : rect.center().x()),
            axes.top ? rect.bottom() : (axes.bottom ? rect.top() : rect.center().y())};
}

QRectF uniformRectFromFixedPoint(const QRectF& original,
                                 const ResizeHandleAxes& axes,
                                 qreal scale)
{
    const QPointF fixed = fixedHandlePoint(original, axes);
    const qreal width = std::max<qreal>(1.0, original.width() * scale);
    const qreal height = std::max<qreal>(1.0, original.height() * scale);
    const qreal x = axes.left ? fixed.x() - width
                              : (axes.right ? fixed.x() : fixed.x() - width / 2.0);
    const qreal y = axes.top ? fixed.y() - height
                             : (axes.bottom ? fixed.y() : fixed.y() - height / 2.0);
    return {x, y, width, height};
}

QRectF uniformRectFromMovingCorner(const QRectF& original,
                                   const ResizeHandleAxes& axes,
                                   const QPointF& movingCorner,
                                   qreal scale)
{
    const qreal width = std::max<qreal>(1.0, original.width() * scale);
    const qreal height = std::max<qreal>(1.0, original.height() * scale);
    return {axes.left ? movingCorner.x() : movingCorner.x() - width,
            axes.top ? movingCorner.y() : movingCorner.y() - height,
            width, height};
}

template<typename T>
void sortAndDeduplicate(QVector<T>* values)
{
    if (!values) return;
    std::sort(values->begin(), values->end());
    values->erase(std::unique(values->begin(), values->end(), [](const T& a, const T& b) {
        return std::abs(a - b) <= kSnapEpsilon;
    }), values->end());
}

void sortAndDeduplicatePoints(QVector<QPointF>* values)
{
    if (!values) return;
    std::sort(values->begin(), values->end(), [](const QPointF& a, const QPointF& b) {
        return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y();
    });
    values->erase(std::unique(values->begin(), values->end(), [](const QPointF& a,
                                                                  const QPointF& b) {
        return std::abs(a.x() - b.x()) <= kSnapEpsilon
            && std::abs(a.y() - b.y()) <= kSnapEpsilon;
    }), values->end());
}

bool nearestSnapValue(qreal proposed, const QVector<qreal>& targets,
                      qreal threshold, qreal* snappedValue,
                      qreal* snappedDistance = nullptr)
{
    qreal bestDistance = threshold + kSnapEpsilon;
    qreal bestValue = proposed;
    bool found = false;
    for (qreal target : targets) {
        const qreal distance = std::abs(target - proposed);
        if (distance < bestDistance - kSnapEpsilon) {
            bestDistance = distance;
            bestValue = target;
            found = true;
        }
    }
    if (found && bestDistance <= threshold + kSnapEpsilon) {
        if (snappedValue) *snappedValue = bestValue;
        if (snappedDistance) *snappedDistance = bestDistance;
        return true;
    }
    return false;
}

bool containsGuideAt(const QVariantList& guides, bool vertical, qreal position)
{
    for (const QVariant& value : guides) {
        const QVariantMap existing = value.toMap();
        const qreal x1 = existing.value(QStringLiteral("x1")).toReal();
        const qreal y1 = existing.value(QStringLiteral("y1")).toReal();
        const qreal x2 = existing.value(QStringLiteral("x2")).toReal();
        const qreal y2 = existing.value(QStringLiteral("y2")).toReal();
        if (vertical && std::abs(x1 - x2) <= kSnapEpsilon
            && std::abs(x1 - position) <= kSnapEpsilon) return true;
        if (!vertical && std::abs(y1 - y2) <= kSnapEpsilon
            && std::abs(y1 - position) <= kSnapEpsilon) return true;
    }
    return false;
}

void appendGuide(QVariantList* guides, const QRectF& bounds,
                 bool vertical, qreal position)
{
    if (!guides || containsGuideAt(*guides, vertical, position)) return;
    if (vertical) {
        guides->append(guide(position, bounds.top(), position, bounds.bottom()));
    } else {
        guides->append(guide(bounds.left(), position, bounds.right(), position));
    }
}
}

QuickCanvasController::QuickCanvasController(CanvasDocument* document,
                                             QObject* parent)
    : QObject(parent)
    , m_scaleGestureEndTimer(new QTimer(this))
    , m_document(document)
    , m_mediaListModel(new MediaListModel(this))
    , m_videoStateTimer(new QTimer(this))
{
    Q_ASSERT(document);
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::errorOccurred,
            this, [this](const QString& owner, const QString& message) {
        if (!m_document) return;
        for (CanvasMedia* media : m_document->media()) {
            if (media && media->residencyOwnerId() == owner) {
                TOAST_ERROR(QStringLiteral("Could not load %1: %2").arg(media->displayName(), message));
                return;
            }
        }
    });
    connect(document, &CanvasDocument::timelineEvaluated,
            this, &QuickCanvasController::publishMedia);
    connect(document, &CanvasDocument::mediaAdded,
            this, &QuickCanvasController::publishMedia);
    connect(document, &CanvasDocument::mediaRemoved,
            this, &QuickCanvasController::publishMedia);
    connect(document, &CanvasDocument::mediaChanged,
            this, [this](const QString& id) {
        publishMedia();
        if (m_document->selectedMedia() && m_document->selectedMedia()->mediaId() == id)
            emit selectedMediaChanged();
    });
    connect(document, &CanvasDocument::mediaSourceInvalidated, this,
            [](const QString&, const QString& reason) { TOAST_WARNING(reason); });
    connect(document, &CanvasDocument::mediaImportFailed, this,
            [](const QString&, const QString& path, const QString& reason) {
        TOAST_WARNING(QStringLiteral("Import refused: %1 — %2")
                          .arg(QFileInfo(path).fileName(), reason));
    });
    connect(document, &CanvasDocument::selectionChanged,
            this, [this]() {
        // A media press promotes its target before starting the new drag.
        // Cancel an existing transform, without revoking that fresh press.
        if (m_scaleGestureActive || !m_transformStarts.isEmpty()
            || !m_dragMediaId.isEmpty() || !m_resizeMediaId.isEmpty())
            cancelPendingEdits();
        const QString id = m_document && m_document->selectedMedia()
            ? m_document->selectedMedia()->mediaId() : QString();
        if (id != m_lastSelectedId) {
            m_lastSelectedId = id;
            emit selectedMediaChanged();
        }
        publishSelection();
    });
    connect(document, &CanvasDocument::screensChanged, this, [this] {
        publishScreens();
        ensureInitialFit(m_initialFitMargin);
    });
    connect(document, &CanvasDocument::cameraChanged,
            this, &QuickCanvasController::publishCamera);
    connect(document, &CanvasDocument::remoteCursorChanged,
            this, &QuickCanvasController::publishRemoteCursor);
    connect(document, &CanvasDocument::editsLockedChanged, this, [this] {
        if (editsLocked()) cancelPendingEdits();
        emit editingEnabledChanged();
    });
    connect(document, &CanvasDocument::mediaAboutToBeRemoved, this,
            [this](CanvasMedia* media) {
        if (media && m_transformStarts.contains(media->mediaId()))
            cancelPendingEdits();
    });
    m_videoStateTimer->setInterval(
        AppConfig::instance().videoStatePublishIntervalMs());
    connect(m_videoStateTimer, &QTimer::timeout,
            this, &QuickCanvasController::publishVideoState);
    m_scaleGestureEndTimer->setSingleShot(true);
    connect(m_scaleGestureEndTimer, &QTimer::timeout,
            this, &QuickCanvasController::finishSelectionScaleGesture);
}

QuickCanvasController::~QuickCanvasController() = default;

bool QuickCanvasController::initialize(QString* errorMessage)
{
    registerCanvasQmlTypes();
    m_videoStateTimer->start();
    publishAll();
    if (errorMessage) errorMessage->clear();
    return true;
}

void QuickCanvasController::registerWindow(QQuickWindow* window)
{
    if (m_renderWindow == window) return;
    finishSelectionScaleGesture();
    disconnect(m_scaleFrameConnection);
    m_renderWindow = window;
    if (window) {
        // afterAnimating runs on the GUI thread once per rendered frame, before
        // scene-graph synchronization. Consume every input delta but publish
        // only the newest preview for that frame.
        m_scaleFrameConnection = connect(window, &QQuickWindow::afterAnimating,
            this, &QuickCanvasController::flushSelectionScalePreview);
    }
}

QQuickWindow* QuickCanvasController::renderWindow() const
{
    return m_renderWindow;
}

CanvasMedia* QuickCanvasController::selectedMediaItem() const
{
    return m_document ? m_document->primarySelectedMedia() : nullptr;
}

QString QuickCanvasController::primarySelectedMediaId() const
{
    return m_document ? m_document->primarySelectedMediaId() : QString();
}

void QuickCanvasController::discardPendingEdits()
{
    cancelPendingEdits();
}

QObject* QuickCanvasController::mediaModel() const
{
    return m_mediaListModel;
}

bool QuickCanvasController::editsLocked() const
{
    return !m_document || m_document->editsLocked();
}

void QuickCanvasController::publishAll()
{
    publishCamera();
    publishScreens();
    publishMedia();
    publishSelection();
    publishRemoteCursor();
    publishVideoState();
    emit presentationChanged();
}

void QuickCanvasController::publishMedia()
{
    QVariantList list;
    if (m_document) {
        QList<CanvasMedia*> media = m_document->media();
        std::sort(media.begin(), media.end(), [](CanvasMedia* a, CanvasMedia* b) {
            return a && b ? a->mediaId() < b->mediaId() : a < b;
        });
        for (CanvasMedia* item : media) {
            if (!item) continue;
            QVariantMap projection = item->toModelMap();
            projection.insert(QStringLiteral("rowKey"), item->mediaId());
            list.append(projection);
        }
    }
    m_mediaListModel->updateFromList(list);
    // Move/snap, camera and video ticks must not invalidate the document and
    // selection bindings while a native pointer gesture is being activated.
    // Publish those snapshots only when their contents actually change.
    if (m_mediaSnapshot != list) {
        m_mediaSnapshot = list;
        emit mediaSnapshotChanged();
    }
    publishSelection();
    emit presentationChanged();
}

void QuickCanvasController::publishSelection()
{
    if (!m_document) return;
    QVariantList list;
    for (CanvasMedia* media : m_document->media()) {
        if (!media || !media->selected() || !media->clipActive()) continue;
        const QRectF rect = media->sceneRect();
        list.append(QVariantMap{{QStringLiteral("mediaId"), media->mediaId()},
                                {QStringLiteral("isPrimary"), media->mediaId() == m_document->primarySelectedMediaId()},
                                {QStringLiteral("x"), rect.x()},
                                {QStringLiteral("y"), rect.y()},
                                {QStringLiteral("width"), rect.width()},
                                {QStringLiteral("height"), rect.height()}});
    }
    if (m_selectionChromeModel != list) {
        m_selectionChromeModel = list;
        emit selectionChromeModelChanged();
    }
}

void QuickCanvasController::publishScreens()
{
    if (!m_document) return;
    QVariantList screens;
    QVariantList zones;
    QList<ScreenInfo> ordered = m_document->screens();
    std::sort(ordered.begin(), ordered.end(), [](const ScreenInfo& a, const ScreenInfo& b) {
        return a.y == b.y ? (a.x == b.x ? a.id < b.id : a.x < b.x) : a.y < b.y;
    });
    int displayIndex = 1;
    for (const ScreenInfo& screen : ordered) {
        const QRectF rect = m_document->screenRects().value(screen.id);
        screens.append(QVariantMap{
            {QStringLiteral("screenId"), screen.id},
            {QStringLiteral("displayIndex"), displayIndex++},
            {QStringLiteral("x"), rect.x()}, {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()},
            {QStringLiteral("height"), rect.height()},
            {QStringLiteral("primary"), screen.primary},
            {QStringLiteral("pixelWidth"), screen.width},
            {QStringLiteral("pixelHeight"), screen.height}});
        for (const ScreenInfo::UIZone& zone : screen.uiZones) {
            if (screen.width <= 0 || screen.height <= 0) continue;
            const QRectF zoneRect(rect.x() + zone.x * rect.width() / screen.width,
                                  rect.y() + zone.y * rect.height() / screen.height,
                                  zone.width * rect.width() / screen.width,
                                  zone.height * rect.height() / screen.height);
            zones.append(QVariantMap{
                {QStringLiteral("screenId"), screen.id},
                {QStringLiteral("type"), zone.type},
                {QStringLiteral("x"), zoneRect.x()},
                {QStringLiteral("y"), zoneRect.y()},
                {QStringLiteral("width"), zoneRect.width()},
                {QStringLiteral("height"), std::max<qreal>(3.0, zoneRect.height())}});
        }
    }
    m_screensModel = screens;
    m_uiZonesModel = zones;
    emit presentationChanged();
}

int QuickCanvasController::remoteCursorDiameter() const
{
    return AppConfig::instance().remoteCursorDiameterPx();
}

void QuickCanvasController::publishRemoteCursor()
{
    if (!m_document) return;
    if (m_remoteCursorVisible == m_document->remoteCursorVisible()
        && m_remoteCursorX == m_document->remoteCursorPosition().x()
        && m_remoteCursorY == m_document->remoteCursorPosition().y()) return;
    m_remoteCursorVisible = m_document->remoteCursorVisible();
    m_remoteCursorX = m_document->remoteCursorPosition().x();
    m_remoteCursorY = m_document->remoteCursorPosition().y();
    emit remoteCursorChanged();
}

void QuickCanvasController::publishVideoState()
{
    if (!m_document) return;
    QVariantMap states;
    for (CanvasMedia* media : m_document->media()) {
        if (!media || !media->isVideo()) continue;
        states.insert(media->mediaId(), QVariantMap{
            {QStringLiteral("mediaId"), media->mediaId()},
            {QStringLiteral("isPlaying"), media->isPlaying()},
            {QStringLiteral("isMuted"), media->muted()},
            {QStringLiteral("volume"), media->volume()}});
    }
    if (m_videoStateModel == states) return;
    m_videoStateModel = states;
    emit presentationChanged();
}

void QuickCanvasController::refreshMediaProjection()
{
    publishMedia();
}

void QuickCanvasController::selectMedia(const QString& mediaId, bool additive)
{
    handleMediaSelectRequested(mediaId, additive);
}

void QuickCanvasController::setShellActive(bool active)
{
    if (m_shellActive == active) return;
    m_shellActive = active;
    emit presentationChanged();
}

void QuickCanvasController::updateRemoteCursor(int screenId, const QPointF& screenPosition)
{
    if (m_document) m_document->updateRemoteCursor(screenId, screenPosition);
}

void QuickCanvasController::hideRemoteCursor()
{
    if (m_document) m_document->hideRemoteCursor();
}

void QuickCanvasController::resetView()
{
    if (!m_document) return;
    m_document->resetCamera();
}

void QuickCanvasController::recenterView(int marginPx)
{
    if (m_viewportSize.isEmpty()) return;
    if (!fitToScreens(marginPx)) resetView();
}

bool QuickCanvasController::fitToScreens(int marginPx)
{
    const QRectF bounds = allScreenBounds(m_document);
    return fitToBounds(bounds.x(), bounds.y(), bounds.width(), bounds.height(), marginPx);
}

bool QuickCanvasController::fitToBounds(qreal x, qreal y, qreal width,
                                       qreal height, qreal marginPx)
{
    if (!m_document || m_viewportSize.isEmpty()
        || !std::isfinite(x) || !std::isfinite(y)
        || !std::isfinite(width) || !std::isfinite(height)
        || !std::isfinite(marginPx) || width <= 0.0 || height <= 0.0) return false;
    const qreal margin = std::max<qreal>(0.0, marginPx);
    const qreal availableWidth = std::max<qreal>(1.0, m_viewportSize.width() - 2.0 * margin);
    const qreal availableHeight = std::max<qreal>(1.0, m_viewportSize.height() - 2.0 * margin);
    const qreal scale = std::min(availableWidth / width, availableHeight / height);
    // Fitting is allowed outside the manual zoom range.
    m_document->setCameraView({x + width / 2.0, y + height / 2.0},
        std::min(m_viewportSize.width(), m_viewportSize.height()) / scale);
    return true;
}

void QuickCanvasController::setTextToolActive(bool active)
{
    if (m_textToolActive == active) return;
    m_textToolActive = active;
    emit textToolActiveChanged();
    emit presentationChanged();
}

void QuickCanvasController::setProjectEditingEnabled(bool enabled)
{
    if (m_projectEditingEnabled == enabled) return;
    m_projectEditingEnabled = enabled;
    if (!enabled) cancelPendingEdits();
    emit editingEnabledChanged();
}

void QuickCanvasController::cancelPendingEdits()
{
    // Revocation is synchronous: discard every provisional edit so a delayed
    // QML release event cannot commit it after editing has been revoked.
    cancelLocalFileDrag();
    m_dragMediaId.clear();
    m_lastMoveSnapped = false;
    m_liveSnapDragMediaId.clear();
    m_transformStarts.clear();
    m_liveTransforms.clear();
    clearLiveResize();
    setTextToolActive(false);
    publishSnapGuides({});
    emit pendingEditsCanceled();
}

qreal QuickCanvasController::currentViewScale() const
{
    return m_viewScale > 0.0 ? m_viewScale : 1.0;
}

void QuickCanvasController::ensureInitialFit(int marginPx)
{
    m_initialFitMargin = marginPx;
    if (m_viewportSize.isEmpty() || !m_document || m_document->hasCamera()
        || !m_document->hasActiveScreens()) return;
    fitToScreens(marginPx);
}

void QuickCanvasController::setViewportSize(qreal width, qreal height)
{
    if (!std::isfinite(width) || !std::isfinite(height)
        || width <= 0.0 || height <= 0.0) return;
    const QSizeF size(width, height);
    if (m_viewportSize == size) return;
    m_viewportSize = size;
    publishCamera();
    ensureInitialFit(m_initialFitMargin);
}

void QuickCanvasController::publishCamera()
{
    if (!m_document) return;
    qreal scale = m_document->cameraScale();
    qreal panX = m_document->cameraPanX();
    qreal panY = m_document->cameraPanY();
    if (!m_viewportSize.isEmpty()) {
        const qreal side = std::min(m_viewportSize.width(), m_viewportSize.height());
        const QPointF viewCenter(m_viewportSize.width() / 2.0, m_viewportSize.height() / 2.0);
        if (m_document->hasCamera() && !m_document->hasNormalizedCamera()) {
            // Legacy projects have no saved viewport size. Adopt their pixel
            // transform in the first valid viewport, then keep only its framing.
            m_document->setCameraView((viewCenter - QPointF(panX, panY)) / scale,
                                      side / scale);
            return; // cameraChanged publishes the normalized projection.
        }
        scale = side / m_document->cameraSquareSceneSize();
        const QPointF pan = viewCenter - m_document->cameraCenter() * scale;
        panX = pan.x();
        panY = pan.y();
    }
    if (!std::isfinite(scale) || scale <= 0.0
        || !std::isfinite(panX) || !std::isfinite(panY)) return;
    m_document->setCameraProjection(scale, panX, panY);
    if (m_viewScale == scale && m_panX == panX && m_panY == panY) return;
    m_viewScale = scale;
    m_panX = panX;
    m_panY = panY;
    emit presentationChanged();
}

void QuickCanvasController::updateCamera(qreal scale, qreal panX, qreal panY)
{
    if (!m_document || !std::isfinite(scale) || scale <= 0.0
        || !std::isfinite(panX) || !std::isfinite(panY)) return;
    if (m_viewportSize.isEmpty()) {
        m_document->setCamera(scale, panX, panY);
        return;
    }
    m_document->setCameraView(
        {(m_viewportSize.width() / 2.0 - panX) / scale,
         (m_viewportSize.height() / 2.0 - panY) / scale},
        std::min(m_viewportSize.width(), m_viewportSize.height()) / scale);
}

void QuickCanvasController::panBy(qreal dx, qreal dy)
{
    if (!m_document || m_viewportSize.isEmpty()
        || !std::isfinite(dx) || !std::isfinite(dy)) return;
    m_document->setCameraView(m_document->cameraCenter() - QPointF(dx, dy) / m_viewScale,
                              m_document->cameraSquareSceneSize());
}

void QuickCanvasController::zoomAt(qreal x, qreal y, qreal factor)
{
    if (!m_document || m_viewportSize.isEmpty() || !std::isfinite(x)
        || !std::isfinite(y) || !std::isfinite(factor) || factor <= 0.0) return;
    const qreal zoom = 1000.0 / m_document->cameraSquareSceneSize();
    // When a fit lies outside [0.2, 10], allow gradual movement back into the
    // range, but do not allow a gesture to move farther away from it.
    const qreal nextZoom = std::clamp(zoom * factor,
                                     std::min(0.2, zoom), std::max(10.0, zoom));
    if (qFuzzyCompare(zoom, nextZoom)) return;
    const qreal scale = m_viewScale * (nextZoom / zoom);
    const QPointF anchor = mapViewPointToScene({x, y});
    updateCamera(scale, x - anchor.x() * scale, y - anchor.y() * scale);
}

QPointF QuickCanvasController::mapViewPointToScene(const QPointF& point) const
{
    const qreal scale = currentViewScale();
    return {(point.x() - m_panX) / scale,
            (point.y() - m_panY) / scale};
}

void QuickCanvasController::handleMediaSelectRequested(const QString& mediaId,
                                                        bool additive)
{
    finishSelectionScaleGesture();
    if (m_document && !m_document->editsLocked()) m_document->select(mediaId, additive);
}

void QuickCanvasController::handleClearSelectionRequested()
{
    finishSelectionScaleGesture();
    if (m_document && !m_document->editsLocked()) m_document->clearSelection();
}

void QuickCanvasController::handleMediaMoveStarted(const QString& mediaId,
                                                    qreal, qreal, bool)
{
    finishSelectionScaleGesture();
    if (!editingEnabled()) return;
    CanvasMedia* media = m_document->mediaById(mediaId);
    if (!media || !media->clipActive() || mediaId != primarySelectedMediaId()) return;
    m_dragMediaId = mediaId;
    m_lastMoveSnapped = false;
    m_liveSnapDragMediaId.clear();
    captureTransformSelection(media);
    rebuildSnapTargets(media);
    publishSnapGuides({});
}

void QuickCanvasController::rebuildSnapTargets(CanvasMedia* activeMedia)
{
    clearSnapTargets();
    if (!m_document || !activeMedia) return;

    for (const QRectF& screen : m_document->screenRects().values()) {
        if (screen.isValid() && !screen.isEmpty()) m_snapTargetRects.append(screen);
    }
    for (CanvasMedia* media : m_document->media()) {
        if (!media || !media->clipActive() || media == activeMedia
            || m_transformStarts.contains(media->mediaId())) continue;
        const QRectF rect = media->sceneRect();
        if (rect.isValid() && !rect.isEmpty()) m_snapTargetRects.append(rect);
    }

    std::sort(m_snapTargetRects.begin(), m_snapTargetRects.end(),
              [](const QRectF& a, const QRectF& b) {
        if (a.left() != b.left()) return a.left() < b.left();
        if (a.top() != b.top()) return a.top() < b.top();
        if (a.width() != b.width()) return a.width() < b.width();
        return a.height() < b.height();
    });

    for (const QRectF& rect : m_snapTargetRects) {
        m_snapEdgesX.append(rect.left());
        m_snapEdgesX.append(rect.right());
        m_snapEdgesY.append(rect.top());
        m_snapEdgesY.append(rect.bottom());
        m_snapCentersX.append(rect.center().x());
        m_snapCentersY.append(rect.center().y());
        m_snapCorners.append(rect.topLeft());
        m_snapCorners.append(rect.topRight());
        m_snapCorners.append(rect.bottomLeft());
        m_snapCorners.append(rect.bottomRight());
    }
    sortAndDeduplicate(&m_snapEdgesX);
    sortAndDeduplicate(&m_snapEdgesY);
    sortAndDeduplicate(&m_snapCentersX);
    sortAndDeduplicate(&m_snapCentersY);
    sortAndDeduplicatePoints(&m_snapCorners);
}

void QuickCanvasController::clearSnapTargets()
{
    m_snapTargetRects.clear();
    m_snapEdgesX.clear();
    m_snapEdgesY.clear();
    m_snapCentersX.clear();
    m_snapCentersY.clear();
    m_snapCorners.clear();
}

QPointF QuickCanvasController::snappedPosition(CanvasMedia* media,
                                               const QPointF& proposed,
                                               QVariantList* guides) const
{
    if (!media || !m_document) return proposed;
    const qreal viewScale = currentViewScale();
    const qreal edgeThreshold = kSnapDistancePx / viewScale;
    const qreal cornerThreshold = kCornerSnapDistancePx / viewScale;
    const QSizeF size(media->sceneRect().size());
    const QRectF proposedRect(proposed, size);
    QRectF bounds = allScreenBounds(m_document).united(proposedRect);
    for (const QRectF& target : m_snapTargetRects) bounds = bounds.united(target);
    bounds = bounds.adjusted(-1000.0 / viewScale, -1000.0 / viewScale,
                              1000.0 / viewScale, 1000.0 / viewScale);

    // Matching dimensions and position form an atomic full-box snap. Iterating
    // real rectangles is linear and avoids the old combinatorial edge-pair scan.
    const qreal sizeTolerance = std::max<qreal>(0.75, edgeThreshold * 0.15);
    const QRectF* bestBox = nullptr;
    qreal bestBoxError = std::numeric_limits<qreal>::max();
    for (const QRectF& target : m_snapTargetRects) {
        if (std::abs(target.width() - size.width()) > sizeTolerance
            || std::abs(target.height() - size.height()) > sizeTolerance) continue;
        const qreal dx = target.left() - proposedRect.left();
        const qreal dy = target.top() - proposedRect.top();
        if (std::abs(dx) > edgeThreshold || std::abs(dy) > edgeThreshold) continue;
        const qreal error = std::hypot(dx, dy);
        if (error < bestBoxError) {
            bestBoxError = error;
            bestBox = &target;
        }
    }
    if (bestBox) {
        appendGuide(guides, bounds, true, bestBox->left());
        appendGuide(guides, bounds, true, bestBox->right());
        appendGuide(guides, bounds, false, bestBox->top());
        appendGuide(guides, bounds, false, bestBox->bottom());
        return bestBox->topLeft();
    }

    // Corners are intentionally considered before independent borders. This
    // prevents a near-corner gesture from being captured by only one axis.
    const QPointF movingCorners[] = {proposedRect.topLeft(), proposedRect.topRight(),
                                     proposedRect.bottomLeft(), proposedRect.bottomRight()};
    bool cornerFound = false;
    qreal bestCornerError = std::numeric_limits<qreal>::max();
    QPointF bestCornerDelta;
    for (const QPointF& movingCorner : movingCorners) {
        for (const QPointF& targetCorner : m_snapCorners) {
            const qreal dx = targetCorner.x() - movingCorner.x();
            const qreal dy = targetCorner.y() - movingCorner.y();
            if (std::abs(dx) > cornerThreshold || std::abs(dy) > cornerThreshold) continue;
            const qreal error = std::hypot(dx, dy);
            if (error < bestCornerError) {
                bestCornerError = error;
                bestCornerDelta = {dx, dy};
                cornerFound = true;
            }
        }
    }
    if (cornerFound) {
        const QRectF snappedRect(proposed + bestCornerDelta, size);
        const qreal xValues[] = {snappedRect.left(), snappedRect.center().x(),
                                 snappedRect.right()};
        const qreal yValues[] = {snappedRect.top(), snappedRect.center().y(),
                                 snappedRect.bottom()};
        for (qreal source : xValues) {
            for (qreal target : m_snapEdgesX) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, true, target);
            }
            for (qreal target : m_snapCentersX) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, true, target);
            }
        }
        for (qreal source : yValues) {
            for (qreal target : m_snapEdgesY) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, false, target);
            }
            for (qreal target : m_snapCentersY) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, false, target);
            }
        }
        return snappedRect.topLeft();
    }

    qreal bestDx = edgeThreshold + 1.0;
    qreal bestDy = edgeThreshold + 1.0;
    qreal guideX = 0.0;
    qreal guideY = 0.0;
    const qreal sourceX[] = {proposedRect.left(), proposedRect.center().x(),
                             proposedRect.right()};
    const qreal sourceY[] = {proposedRect.top(), proposedRect.center().y(),
                             proposedRect.bottom()};
    auto considerX = [&](qreal target) {
        for (qreal source : sourceX) {
            const qreal delta = target - source;
            if (std::abs(delta) < std::abs(bestDx) - kSnapEpsilon) {
                bestDx = delta;
                guideX = target;
            }
        }
    };
    auto considerY = [&](qreal target) {
        for (qreal source : sourceY) {
            const qreal delta = target - source;
            if (std::abs(delta) < std::abs(bestDy) - kSnapEpsilon) {
                bestDy = delta;
                guideY = target;
            }
        }
    };
    for (qreal target : m_snapEdgesX) considerX(target);
    for (qreal target : m_snapCentersX) considerX(target);
    for (qreal target : m_snapEdgesY) considerY(target);
    for (qreal target : m_snapCentersY) considerY(target);

    const bool snapX = std::abs(bestDx) <= edgeThreshold;
    const bool snapY = std::abs(bestDy) <= edgeThreshold;
    const QRectF snappedRect(
        {proposed.x() + (snapX ? bestDx : 0.0),
         proposed.y() + (snapY ? bestDy : 0.0)}, size);
    if (snapX) {
        const qreal snappedSourceX[] = {snappedRect.left(), snappedRect.center().x(),
                                        snappedRect.right()};
        for (qreal source : snappedSourceX) {
            for (qreal target : m_snapEdgesX) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, true, target);
            }
            for (qreal target : m_snapCentersX) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, true, target);
            }
        }
        appendGuide(guides, bounds, true, guideX);
    }
    if (snapY) {
        const qreal snappedSourceY[] = {snappedRect.top(), snappedRect.center().y(),
                                        snappedRect.bottom()};
        for (qreal source : snappedSourceY) {
            for (qreal target : m_snapEdgesY) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, false, target);
            }
            for (qreal target : m_snapCentersY) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, false, target);
            }
        }
        appendGuide(guides, bounds, false, guideY);
    }
    return snappedRect.topLeft();
}

void QuickCanvasController::handleMediaMoveUpdated(const QString& mediaId,
                                                    qreal x, qreal y, bool snap)
{
    if (!editingEnabled() || m_dragMediaId != mediaId) return;
    CanvasMedia* media = m_document->mediaById(mediaId);
    if (!media) return;
    QVariantList guides;
    m_lastSnappedPosition = snap ? snappedPosition(media, {x, y}, &guides)
                                 : QPointF(x, y);
    m_lastMoveSnapped = !guides.isEmpty();
    m_liveSnapDragMediaId = m_lastMoveSnapped ? mediaId : QString();
    m_liveSnapDragX = m_lastSnappedPosition.x();
    m_liveSnapDragY = m_lastSnappedPosition.y();
    previewMove(m_lastSnappedPosition);
    publishSnapGuides(guides);
}

void QuickCanvasController::handleMediaMoveEnded(const QString& mediaId,
                                                  qreal x, qreal y, bool snap)
{
    if (mediaId != m_dragMediaId) return;
    if (editingEnabled() && m_document->mediaById(mediaId)) {
        previewMove(snap && m_lastMoveSnapped ? m_lastSnappedPosition : QPointF(x, y));
        commitTransforms(false, false);
    }
    m_dragMediaId.clear();
    m_lastMoveSnapped = false;
    m_liveSnapDragMediaId.clear();
    m_transformStarts.clear();
    m_liveTransforms.clear();
    emit liveTransformsChanged();
    clearSnapTargets();
    publishSnapGuides({});
    publishMedia();
}

void QuickCanvasController::captureTransformSelection(CanvasMedia* activeMedia)
{
    m_transformStarts.clear();
    m_liveTransforms.clear();
    for (CanvasMedia* media : m_document->media()) {
        if (media != activeMedia) continue;
        m_transformStarts.insert(media->mediaId(),
            {media, media->sceneRect(), media->baseSize(), media->scale()});
    }
}

void QuickCanvasController::previewMove(const QPointF& position)
{
    const QPointF delta = position - m_transformStarts.value(m_dragMediaId).rect.topLeft();
    QVariantMap transforms;
    for (auto it = m_transformStarts.cbegin(); it != m_transformStarts.cend(); ++it) {
        const TransformStart& start = it.value();
        if (!start.media) continue;
        const QPointF moved = start.rect.topLeft() + delta;
        transforms.insert(it.key(), QVariantMap{
            {QStringLiteral("x"), moved.x()}, {QStringLiteral("y"), moved.y()},
            {QStringLiteral("width"), start.baseSize.width()},
            {QStringLiteral("height"), start.baseSize.height()},
            {QStringLiteral("scale"), start.scale}});
    }
    m_liveTransforms = transforms;
    emit liveTransformsChanged();
}

void QuickCanvasController::previewResize()
{
    // Only the manipulated item resolves snapping. Apply its final relative
    // geometry to each item's own original rectangle, including its anchor.
    const qreal sx = m_pendingResizeRect.width() / m_resizeOriginalRect.width();
    const qreal sy = m_pendingResizeRect.height() / m_resizeOriginalRect.height();
    const qreal dx = (m_pendingResizeRect.x() - m_resizeOriginalRect.x())
        / m_resizeOriginalRect.width();
    const qreal dy = (m_pendingResizeRect.y() - m_resizeOriginalRect.y())
        / m_resizeOriginalRect.height();
    QVariantMap transforms;
    for (auto it = m_transformStarts.cbegin(); it != m_transformStarts.cend(); ++it) {
        const TransformStart& start = it.value();
        if (!start.media) continue;
        const qreal scale = m_pendingResizeAlt ? start.scale : start.scale * sx;
        const qreal width = m_pendingResizeAlt
            ? std::max(1, qRound(start.baseSize.width() * sx)) : start.baseSize.width();
        const qreal height = m_pendingResizeAlt
            ? std::max(1, qRound(start.baseSize.height() * sy)) : start.baseSize.height();
        transforms.insert(it.key(), QVariantMap{
            {QStringLiteral("x"), start.rect.x() + dx * start.rect.width()},
            {QStringLiteral("y"), start.rect.y() + dy * start.rect.height()},
            {QStringLiteral("width"), width}, {QStringLiteral("height"), height},
            {QStringLiteral("scale"), scale},
            {QStringLiteral("altResize"), m_pendingResizeAlt}});
    }
    m_liveTransforms = transforms;
    emit liveTransformsChanged();
}

void QuickCanvasController::commitTransforms(bool resize, bool alt)
{
    // Copies protect iteration if a synchronous document listener removes an
    // item or revokes editing while committing. QPointer protects lifetimes.
    const auto starts = m_transformStarts;
    const auto transforms = m_liveTransforms;
    for (auto it = starts.cbegin(); it != starts.cend(); ++it) {
        const QPointer<CanvasMedia> media = it->media;
        const QVariantMap geometry = transforms.value(it.key()).toMap();
        if (!editingEnabled() || !media || geometry.isEmpty()
            || media->mediaId() != primarySelectedMediaId()) continue;
        media->beginElementEdit();
        if (resize) {
            if (alt) {
                if (media->isText()) media->setFitToTextEnabled(false);
                media->setBaseSize(QSize(geometry.value(QStringLiteral("width")).toInt(),
                                         geometry.value(QStringLiteral("height")).toInt()));
            }
            media->setPositionAndScale(
                {geometry.value(QStringLiteral("x")).toReal(),
                 geometry.value(QStringLiteral("y")).toReal()},
                geometry.value(QStringLiteral("scale")).toReal());
        } else {
            media->setPosition({geometry.value(QStringLiteral("x")).toReal(),
                               geometry.value(QStringLiteral("y")).toReal()});
        }
    }
}

QRectF QuickCanvasController::resizedRect(const QRectF& original,
                                          const QString& handle,
                                          const QPointF& point,
                                          bool uniform)
{
    QPointF fixed = original.topLeft();
    bool moveLeft = handle.contains(QLatin1String("left"));
    bool moveRight = handle.contains(QLatin1String("right"));
    bool moveTop = handle.contains(QLatin1String("top"));
    bool moveBottom = handle.contains(QLatin1String("bottom"));
    if (moveLeft) fixed.setX(original.right());
    else if (moveRight) fixed.setX(original.left());
    else fixed.setX(original.center().x());
    if (moveTop) fixed.setY(original.bottom());
    else if (moveBottom) fixed.setY(original.top());
    else fixed.setY(original.center().y());

    qreal left = moveLeft ? point.x() : original.left();
    qreal right = moveRight ? point.x() : original.right();
    qreal top = moveTop ? point.y() : original.top();
    qreal bottom = moveBottom ? point.y() : original.bottom();
    if (!moveLeft && !moveRight) {
        top = moveTop ? point.y() : top;
        bottom = moveBottom ? point.y() : bottom;
    }
    if (!moveTop && !moveBottom) {
        left = moveLeft ? point.x() : left;
        right = moveRight ? point.x() : right;
    }
    QRectF rect(QPointF(std::min(left, right), std::min(top, bottom)),
                QPointF(std::max(left, right), std::max(top, bottom)));
    rect.setWidth(std::max<qreal>(1.0, rect.width()));
    rect.setHeight(std::max<qreal>(1.0, rect.height()));
    if (uniform && original.width() > 0 && original.height() > 0) {
        const qreal ratio = original.width() / original.height();
        qreal width = rect.width();
        qreal height = rect.height();
        if ((moveLeft || moveRight) && (moveTop || moveBottom)) {
            const qreal scale = std::max(width / original.width(),
                                         height / original.height());
            width = std::max<qreal>(1.0, original.width() * scale);
            height = std::max<qreal>(1.0, original.height() * scale);
        } else if (moveLeft || moveRight) {
            height = width / ratio;
        } else {
            width = height * ratio;
        }
        QPointF origin = fixed;
        if (moveLeft) origin.rx() -= width;
        if (moveTop) origin.ry() -= height;
        if (!moveLeft && !moveRight) origin.rx() -= width / 2.0;
        if (!moveTop && !moveBottom) origin.ry() -= height / 2.0;
        rect = QRectF(origin, QSizeF(width, height)).normalized();
    }
    return rect;
}

void QuickCanvasController::resetResizeSnapState()
{
    m_resizeSnapBoxActive = false;
    m_resizeSnapBox = {};
    m_resizeSnapCornerActive = false;
    m_resizeSnapCorner = {};
    m_resizeSnapXActive = false;
    m_resizeSnapX = 0.0;
    m_resizeSnapYActive = false;
    m_resizeSnapY = 0.0;
}

void QuickCanvasController::appendAlignedResizeGuides(
    const QRectF& rect, bool snappedX, bool snappedY, QVariantList* guides) const
{
    if (!guides || (!snappedX && !snappedY)) return;

    // When the result exactly occupies a target, expose the whole box rather
    // than only the edge which caused the capture.
    bool showX = snappedX;
    bool showY = snappedY;
    for (const QRectF& target : m_snapTargetRects) {
        if (std::abs(rect.left() - target.left()) <= kSnapEpsilon
            && std::abs(rect.right() - target.right()) <= kSnapEpsilon
            && std::abs(rect.top() - target.top()) <= kSnapEpsilon
            && std::abs(rect.bottom() - target.bottom()) <= kSnapEpsilon) {
            showX = true;
            showY = true;
            break;
        }
    }

    const qreal viewScale = currentViewScale();
    QRectF bounds = allScreenBounds(m_document).united(rect);
    for (const QRectF& target : m_snapTargetRects) bounds = bounds.united(target);
    bounds = bounds.adjusted(-1000.0 / viewScale, -1000.0 / viewScale,
                              1000.0 / viewScale, 1000.0 / viewScale);

    if (showX) {
        const qreal sourceValues[] = {rect.left(), rect.right()};
        for (qreal source : sourceValues) {
            for (qreal target : m_snapEdgesX) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, true, target);
            }
        }
    }
    if (showY) {
        const qreal sourceValues[] = {rect.top(), rect.bottom()};
        for (qreal source : sourceValues) {
            for (qreal target : m_snapEdgesY) {
                if (std::abs(source - target) <= kSnapEpsilon)
                    appendGuide(guides, bounds, false, target);
            }
        }
    }
}

QRectF QuickCanvasController::snappedResizeRect(
    const QRectF& proposed, const QRectF& original, const QString& handle,
    bool altPressed, QVariantList* guides)
{
    const ResizeHandleAxes axes = resizeHandleAxes(handle);
    if ((!axes.movesX() && !axes.movesY()) || original.isEmpty()
        || m_snapTargetRects.isEmpty()) return proposed;

    const qreal viewScale = currentViewScale();
    const qreal edgeThreshold = kSnapDistancePx / viewScale;
    const qreal edgeRelease = edgeThreshold * kSnapReleaseFactor;
    const qreal cornerThreshold = kCornerSnapDistancePx / viewScale;
    const qreal cornerRelease = cornerThreshold * kSnapReleaseFactor;
    const qreal sizeTolerance = std::max<qreal>(0.75, edgeThreshold * 0.15);
    const QPointF proposedMoving = movingHandlePoint(proposed, axes);

    auto boxStillCaptured = [&](const QRectF& box, qreal releaseDistance) {
        const QPointF boxMoving = movingHandlePoint(box, axes);
        if (axes.movesX()
            && std::abs(proposedMoving.x() - boxMoving.x()) > releaseDistance) return false;
        if (axes.movesY()
            && std::abs(proposedMoving.y() - boxMoving.y()) > releaseDistance) return false;
        return true;
    };

    if (m_resizeSnapBoxActive) {
        if (boxStillCaptured(m_resizeSnapBox, cornerRelease)) {
            appendAlignedResizeGuides(m_resizeSnapBox, true, true, guides);
            return m_resizeSnapBox;
        }
        m_resizeSnapBoxActive = false;
        m_resizeSnapBox = {};
    }

    // Full-box fitting works for free corner resize and for every uniform
    // handle when the target shares the media's aspect ratio.
    const QRectF* bestBox = nullptr;
    qreal bestBoxError = std::numeric_limits<qreal>::max();
    for (const QRectF& target : m_snapTargetRects) {
        bool compatible = false;
        if (altPressed) {
            compatible = axes.corner()
                || (axes.movesX()
                    && std::abs(proposed.top() - target.top()) <= sizeTolerance
                    && std::abs(proposed.bottom() - target.bottom()) <= sizeTolerance)
                || (axes.movesY()
                    && std::abs(proposed.left() - target.left()) <= sizeTolerance
                    && std::abs(proposed.right() - target.right()) <= sizeTolerance);
        } else {
            const qreal widthScale = target.width()
                / std::max<qreal>(1.0, original.width());
            const qreal heightScale = target.height()
                / std::max<qreal>(1.0, original.height());
            compatible = std::abs(widthScale - heightScale)
                * std::max(original.width(), original.height()) <= sizeTolerance;
        }
        if (!compatible) continue;
        if (std::abs(proposed.left() - target.left()) > edgeThreshold
            || std::abs(proposed.right() - target.right()) > edgeThreshold
            || std::abs(proposed.top() - target.top()) > edgeThreshold
            || std::abs(proposed.bottom() - target.bottom()) > edgeThreshold) continue;
        const qreal error = std::hypot(proposed.left() - target.left(),
                                       proposed.top() - target.top())
            + std::hypot(proposed.right() - target.right(),
                         proposed.bottom() - target.bottom());
        if (error < bestBoxError) {
            bestBoxError = error;
            bestBox = &target;
        }
    }
    if (bestBox) {
        m_resizeSnapBoxActive = true;
        m_resizeSnapBox = *bestBox;
        m_resizeSnapCornerActive = false;
        m_resizeSnapXActive = false;
        m_resizeSnapYActive = false;
        appendAlignedResizeGuides(*bestBox, true, true, guides);
        return *bestBox;
    }

    auto resolveAxisLock = [](qreal rawValue, const QVector<qreal>& targets,
                              qreal acquireDistance, qreal releaseDistance,
                              bool* active, qreal* lockedValue,
                              qreal* distance) {
        if (*active) {
            const qreal lockedDistance = std::abs(rawValue - *lockedValue);
            if (lockedDistance <= releaseDistance) {
                if (distance) *distance = lockedDistance;
                return true;
            }
            *active = false;
        }
        qreal candidate = rawValue;
        qreal candidateDistance = 0.0;
        if (!nearestSnapValue(rawValue, targets, acquireDistance,
                              &candidate, &candidateDistance)) return false;
        *active = true;
        *lockedValue = candidate;
        if (distance) *distance = candidateDistance;
        return true;
    };

    if (altPressed) {
        QRectF result = proposed;
        if (axes.corner()) {
            bool useCorner = false;
            if (m_resizeSnapCornerActive) {
                const qreal dx = std::abs(proposedMoving.x() - m_resizeSnapCorner.x());
                const qreal dy = std::abs(proposedMoving.y() - m_resizeSnapCorner.y());
                useCorner = dx <= cornerRelease && dy <= cornerRelease;
                if (!useCorner) m_resizeSnapCornerActive = false;
            }
            if (!m_resizeSnapCornerActive) {
                qreal bestError = std::numeric_limits<qreal>::max();
                QPointF bestTarget;
                for (const QPointF& target : m_snapCorners) {
                    const qreal dx = std::abs(proposedMoving.x() - target.x());
                    const qreal dy = std::abs(proposedMoving.y() - target.y());
                    if (dx > cornerThreshold || dy > cornerThreshold) continue;
                    const qreal error = std::hypot(dx, dy);
                    if (error < bestError) {
                        bestError = error;
                        bestTarget = target;
                    }
                }
                if (bestError < std::numeric_limits<qreal>::max()) {
                    m_resizeSnapCornerActive = true;
                    m_resizeSnapCorner = bestTarget;
                    useCorner = true;
                }
            }
            if (useCorner) {
                if (axes.left) result.setLeft(m_resizeSnapCorner.x());
                else result.setRight(m_resizeSnapCorner.x());
                if (axes.top) result.setTop(m_resizeSnapCorner.y());
                else result.setBottom(m_resizeSnapCorner.y());
                m_resizeSnapXActive = false;
                m_resizeSnapYActive = false;
                appendAlignedResizeGuides(result, true, true, guides);
                return result;
            }
        } else {
            m_resizeSnapCornerActive = false;
        }

        bool snappedX = false;
        bool snappedY = false;
        if (axes.movesX()) {
            snappedX = resolveAxisLock(proposedMoving.x(), m_snapEdgesX,
                                        edgeThreshold, edgeRelease,
                                        &m_resizeSnapXActive, &m_resizeSnapX, nullptr);
            if (snappedX) {
                if (axes.left) result.setLeft(m_resizeSnapX);
                else result.setRight(m_resizeSnapX);
            }
        } else {
            m_resizeSnapXActive = false;
        }
        if (axes.movesY()) {
            snappedY = resolveAxisLock(proposedMoving.y(), m_snapEdgesY,
                                        edgeThreshold, edgeRelease,
                                        &m_resizeSnapYActive, &m_resizeSnapY, nullptr);
            if (snappedY) {
                if (axes.top) result.setTop(m_resizeSnapY);
                else result.setBottom(m_resizeSnapY);
            }
        } else {
            m_resizeSnapYActive = false;
        }
        appendAlignedResizeGuides(result, snappedX, snappedY, guides);
        return result;
    }

    // Uniform corner snapping preserves the historical priority and lock: a
    // captured target corner owns both axes until the pointer exits its larger
    // release zone.
    const QPointF fixed = fixedHandlePoint(original, axes);
    const qreal minimumScale = std::max(1.0 / std::max<qreal>(1.0, original.width()),
                                        1.0 / std::max<qreal>(1.0, original.height()));
    if (axes.corner()) {
        bool useCorner = false;
        if (m_resizeSnapCornerActive) {
            const qreal dx = std::abs(proposedMoving.x() - m_resizeSnapCorner.x());
            const qreal dy = std::abs(proposedMoving.y() - m_resizeSnapCorner.y());
            useCorner = dx <= cornerRelease && dy <= cornerRelease;
            if (!useCorner) m_resizeSnapCornerActive = false;
        }
        if (!m_resizeSnapCornerActive) {
            qreal bestError = std::numeric_limits<qreal>::max();
            QPointF bestTarget;
            for (const QPointF& target : m_snapCorners) {
                const qreal dx = std::abs(proposedMoving.x() - target.x());
                const qreal dy = std::abs(proposedMoving.y() - target.y());
                if (dx > cornerThreshold || dy > cornerThreshold) continue;
                const qreal error = std::hypot(dx, dy);
                if (error < bestError) {
                    bestError = error;
                    bestTarget = target;
                }
            }
            if (bestError < std::numeric_limits<qreal>::max()) {
                m_resizeSnapCornerActive = true;
                m_resizeSnapCorner = bestTarget;
                useCorner = true;
            }
        }
        if (useCorner) {
            const qreal scale = std::max({minimumScale,
                std::abs(m_resizeSnapCorner.x() - fixed.x())
                    / std::max<qreal>(1.0, original.width()),
                std::abs(m_resizeSnapCorner.y() - fixed.y())
                    / std::max<qreal>(1.0, original.height())});
            const QRectF result = uniformRectFromMovingCorner(
                original, axes, m_resizeSnapCorner, scale);
            m_resizeSnapXActive = false;
            m_resizeSnapYActive = false;
            appendAlignedResizeGuides(result, true, true, guides);
            return result;
        }
    } else {
        m_resizeSnapCornerActive = false;
    }

    bool snappedX = false;
    bool snappedY = false;
    if (axes.corner() && m_resizeSnapXActive) {
        snappedX = resolveAxisLock(proposedMoving.x(), m_snapEdgesX,
                                    edgeThreshold, edgeRelease,
                                    &m_resizeSnapXActive, &m_resizeSnapX, nullptr);
    } else if (axes.corner() && m_resizeSnapYActive) {
        snappedY = resolveAxisLock(proposedMoving.y(), m_snapEdgesY,
                                    edgeThreshold, edgeRelease,
                                    &m_resizeSnapYActive, &m_resizeSnapY, nullptr);
    }
    if (axes.corner() && !snappedX && !snappedY) {
        qreal candidateX = proposedMoving.x();
        qreal candidateY = proposedMoving.y();
        qreal distanceX = 0.0;
        qreal distanceY = 0.0;
        const bool hasX = nearestSnapValue(proposedMoving.x(), m_snapEdgesX,
                                           edgeThreshold, &candidateX, &distanceX);
        const bool hasY = nearestSnapValue(proposedMoving.y(), m_snapEdgesY,
                                           edgeThreshold, &candidateY, &distanceY);
        if (hasX && (!hasY || distanceX <= distanceY)) {
            m_resizeSnapXActive = true;
            m_resizeSnapX = candidateX;
            snappedX = true;
        } else if (hasY) {
            m_resizeSnapYActive = true;
            m_resizeSnapY = candidateY;
            snappedY = true;
        }
    } else if (!axes.corner() && axes.movesX()) {
        snappedX = resolveAxisLock(proposedMoving.x(), m_snapEdgesX,
                                    edgeThreshold, edgeRelease,
                                    &m_resizeSnapXActive, &m_resizeSnapX, nullptr);
        m_resizeSnapYActive = false;
    } else if (!axes.corner() && axes.movesY()) {
        snappedY = resolveAxisLock(proposedMoving.y(), m_snapEdgesY,
                                    edgeThreshold, edgeRelease,
                                    &m_resizeSnapYActive, &m_resizeSnapY, nullptr);
        m_resizeSnapXActive = false;
    }

    qreal scale = 1.0;
    if (snappedX) {
        scale = std::max(minimumScale,
                         std::abs(m_resizeSnapX - fixed.x())
                             / std::max<qreal>(1.0, original.width()));
    } else if (snappedY) {
        scale = std::max(minimumScale,
                         std::abs(m_resizeSnapY - fixed.y())
                             / std::max<qreal>(1.0, original.height()));
    } else {
        return proposed;
    }
    const QRectF result = uniformRectFromFixedPoint(original, axes, scale);
    appendAlignedResizeGuides(result, snappedX, snappedY, guides);
    return result;
}

void QuickCanvasController::scaleSelectionBy(qreal factor)
{
    finishSelectionScaleGesture();
    updateSelectionScaleGesture(factor, false);
    finishSelectionScaleGesture();
}

void QuickCanvasController::updateSelectionScaleGesture(qreal factor, bool phased)
{
    if (!editingEnabled() || !std::isfinite(factor) || factor <= 0.0
        || qFuzzyCompare(factor, 1.0) || !m_dragMediaId.isEmpty()
        || !m_resizeMediaId.isEmpty()) return;
    CanvasMedia* active = selectedMediaItem();
    if (!active || !active->clipActive()) return;

    const bool startingGesture = !m_scaleGestureActive;
    if (!m_scaleGestureActive) {
        captureTransformSelection(active);
        // Keep each item's center/proportions and a shared lower bound. Capture
        // once per gesture, independent of the number of native wheel packets.
        m_scaleMinimumFactor = 0.0;
        for (const auto& start : std::as_const(m_transformStarts)) {
            if (start.rect.isEmpty()) {
                clearLiveResize();
                return;
            }
            m_scaleMinimumFactor = std::max({m_scaleMinimumFactor,
                1.0 / start.rect.width(), 1.0 / start.rect.height(),
                0.000101 / start.scale});
        }
        m_scaleMinimumFactor = std::min<qreal>(1.0, m_scaleMinimumFactor);
        m_scaleGestureFactor = 1.0;
        m_resizeOriginalRect = active->sceneRect();
        m_scaleGestureActive = true;
    }
    factor = std::max(m_scaleGestureFactor * factor, m_scaleMinimumFactor);
    for (const auto& start : std::as_const(m_transformStarts)) {
        if (!std::isfinite(start.scale * factor)
            || !std::isfinite(start.rect.width() * factor)
            || !std::isfinite(start.rect.height() * factor)) {
            if (startingGesture) clearLiveResize();
            return;
        }
    }
    m_scaleGestureFactor = factor;
    const QSizeF size = m_resizeOriginalRect.size() * factor;
    m_pendingResizeRect = QRectF(m_resizeOriginalRect.center()
                                    - QPointF(size.width() / 2.0, size.height() / 2.0), size);
    m_pendingResizeAlt = false;

    // Mouse wheels have no end event. Native phased gestures normally finish
    // at ScrollEnd; the longer timeout only recovers a lost native end event.
    m_scaleGestureEndTimer->start(phased ? 1500 : 160);
    if (m_scalePreviewPending) return;
    m_scalePreviewPending = true;
    if (m_renderWindow) {
        m_renderWindow->update();
    } else {
        QMetaObject::invokeMethod(this,
            &QuickCanvasController::flushSelectionScalePreview, Qt::QueuedConnection);
    }
}

void QuickCanvasController::flushSelectionScalePreview()
{
    if (!m_scaleGestureActive || !m_scalePreviewPending) return;
    m_scalePreviewPending = false;
    previewResize();
}

void QuickCanvasController::finishSelectionScaleGesture()
{
    if (!m_scaleGestureActive) return;
    m_scaleGestureEndTimer->stop();
    flushSelectionScalePreview();
    // Clear ownership before notifying document observers: a synchronous
    // selection change or edit revocation may otherwise reenter this commit.
    m_scaleGestureActive = false;
    commitTransforms(true, false);
    clearLiveResize();
}

void QuickCanvasController::handleMediaResizeRequested(
    const QString& mediaId, const QString& handleId, qreal x, qreal y,
    bool snap, bool altPressed)
{
    finishSelectionScaleGesture();
    if (!editingEnabled()) return;
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    // Geometry edits do not depend on decoded content. Loading media shares
    // the same transform transaction as every other selected occurrence.
    if (!media || !media->clipActive() || editsLocked() || mediaId != primarySelectedMediaId()) return;
    if (m_resizeMediaId != mediaId) {
        captureTransformSelection(media);
        m_resizeMediaId = mediaId;
        m_resizeHandleId = handleId;
        m_resizeOriginalRect = media->sceneRect();
        m_resizeOriginalScale = std::max<qreal>(0.0001, media->scale());
        m_resizeSnapModeAlt = altPressed;
        resetResizeSnapState();
        rebuildSnapTargets(media);
    } else if (m_resizeHandleId != handleId || m_resizeSnapModeAlt != altPressed) {
        m_resizeHandleId = handleId;
        m_resizeSnapModeAlt = altPressed;
        resetResizeSnapState();
    }
    m_pendingResizeAlt = altPressed;
    m_pendingResizeRect = resizedRect(m_resizeOriginalRect, handleId, {x, y},
                                      !altPressed);
    QVariantList guides;
    if (snap) {
        m_pendingResizeRect = snappedResizeRect(m_pendingResizeRect,
                                                m_resizeOriginalRect,
                                                handleId, altPressed, &guides);
    } else {
        resetResizeSnapState();
    }
    if (altPressed) {
        // Free resize changes the text container. Fitted geometry is no longer
        // authoritative, but the existing uniform scale remains the user's
        // chosen text size and must never be baked back to 1.0.
        m_liveResizeActive = false;
        m_liveAltResizeActive = true;
        m_liveAltResizeMediaId = mediaId;
        m_liveAltResizeRect = QRectF(
            m_pendingResizeRect.topLeft(),
            QSizeF(m_pendingResizeRect.width() / m_resizeOriginalScale,
                   m_pendingResizeRect.height() / m_resizeOriginalScale));
        m_liveAltResizeScale = m_resizeOriginalScale;
    } else {
        const qreal scale = m_pendingResizeRect.width()
            / std::max<qreal>(1.0, media->baseSize().width());
        m_liveAltResizeActive = false;
        m_liveResizeActive = true;
        m_liveResizeMediaId = mediaId;
        m_liveResizeRect = m_pendingResizeRect;
        m_liveResizeScale = scale;
    }
    previewResize();
    publishSnapGuides(guides);
}

void QuickCanvasController::handleMediaResizeEnded(const QString& mediaId)
{
    if (mediaId != m_resizeMediaId) return;
    if (editingEnabled() && m_document->mediaById(mediaId)
        && !m_pendingResizeRect.isEmpty()) {
        commitTransforms(true, m_pendingResizeAlt);
    }
    clearLiveResize();
    publishMedia();
}

void QuickCanvasController::clearLiveResize()
{
    m_scaleGestureEndTimer->stop();
    m_scaleGestureActive = false;
    m_scalePreviewPending = false;
    m_scaleGestureFactor = 1.0;
    m_transformStarts.clear();
    m_liveTransforms.clear();
    emit liveTransformsChanged();
    m_liveResizeActive = false;
    m_liveResizeMediaId.clear();
    m_liveResizeRect = {};
    m_liveResizeScale = 1.0;
    m_liveAltResizeActive = false;
    m_liveAltResizeMediaId.clear();
    m_liveAltResizeRect = {};
    m_liveAltResizeScale = 1.0;
    m_resizeMediaId.clear();
    m_resizeHandleId.clear();
    m_resizeOriginalRect = {};
    m_resizeOriginalScale = 1.0;
    m_pendingResizeRect = {};
    m_pendingResizeAlt = false;
    m_resizeSnapModeAlt = false;
    resetResizeSnapState();
    clearSnapTargets();
    m_snapGuidesModel.clear();
    emit presentationChanged();
}

void QuickCanvasController::handleTextCommitRequested(const QString& mediaId,
                                                       const QString& text)
{
    handleTextLiveUpdateRequested(mediaId, text);
}

void QuickCanvasController::handleTextLiveUpdateRequested(const QString& mediaId,
                                                           const QString& text)
{
    if (!editingEnabled()) return;
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (media && media->isText() && !editsLocked()
        && mediaId == primarySelectedMediaId()) {
        media->beginElementEdit();
        media->setText(text);
    }
}

void QuickCanvasController::handleTextCreateRequested(qreal viewX, qreal viewY)
{
    if (!m_projectEditingEnabled || !m_document || editsLocked()) return;
    const qreal initialHeight = m_document->cameraSquareSceneSize()
        * (AppConfig::instance().canvasTextInitialHeightPercent() / 100.0);
    CanvasMedia* media = m_document->addText(mapViewPointToScene({viewX, viewY}),
                                           QStringLiteral("Text"), initialHeight);
    if (!media) return;
    setTextToolActive(false);
    emit textEditingRequested(media->mediaId());
}

void QuickCanvasController::handleOverlayVisibilityToggle(const QString& id, bool visible)
{
    if (!editingEnabled()) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && !editsLocked() && id == primarySelectedMediaId()) {
        media->beginElementEdit();
        media->setContentVisible(visible);
    }
    emit mediaVisibilityToggleRequested(id, visible);
}

void QuickCanvasController::copySelectedMedia()
{
    finishSelectionScaleGesture();
    if (!editingEnabled() || !m_document) return;
    const QStringList selected = m_document->selectedMediaIds();
    if (selected.isEmpty()) return;
    QJsonArray entries;
    QJsonObject paths;
    const QJsonArray all = m_document->serializeProjectState().value(QStringLiteral("media")).toArray();
    for (const QJsonValue& value : all) {
        const QString id = value.toObject().value(QStringLiteral("mediaId")).toString();
        if (!selected.contains(id)) continue;
        entries.append(value);
        if (const CanvasMedia* media = m_document->mediaById(id))
            paths.insert(id, media->sourcePath());
    }
    // Capture authoring values at copy time; never retain pointers to live media.
    const QJsonObject payload{{QStringLiteral("renderSchemaVersion"), SceneTimeline::RenderSchemaVersion},
                              {QStringLiteral("primaryMediaId"), primarySelectedMediaId()},
                              {QStringLiteral("timeline"), m_document->timelineSettings().toJson()},
                              {QStringLiteral("media"), entries},
                              {QStringLiteral("sourcePaths"), paths}};
    auto* mime = new QMimeData;
    mime->setData(kCanvasClipboardMime, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QGuiApplication::clipboard()->setMimeData(mime);
}

void QuickCanvasController::pasteMedia()
{
    if (!editingEnabled() || !m_document) return;
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(kCanvasClipboardMime)) return;
    const QByteArray encoded = mime->data(kCanvasClipboardMime);
    if (encoded.size() > 32 * 1024 * 1024) return;
    const QJsonObject payload = QJsonDocument::fromJson(encoded).object();
    const QJsonArray entries = payload.value(QStringLiteral("media")).toArray();
    if (entries.isEmpty() || entries.size() > 512) return;
    QHash<QString, QString> paths;
    const QJsonObject storedPaths = payload.value(QStringLiteral("sourcePaths")).toObject();
    for (auto it = storedPaths.begin(); it != storedPaths.end(); ++it)
        paths.insert(it.key(), it.value().toString());
    QStringList skipped;
    const QStringList inserted = m_document->pasteMediaState(payload, paths, &skipped);
    if (!inserted.isEmpty()) {
        TOAST_SUCCESS(inserted.size() == 1 ? QStringLiteral("Media pasted.")
            : QStringLiteral("%1 media pasted.").arg(inserted.size()));
    }
    if (!skipped.isEmpty())
        TOAST_WARNING(QStringLiteral("Some media could not be pasted. Check source files, project duration and matching timeline cadence."));
}

void QuickCanvasController::deleteSelectedMedia()
{
    if (!editingEnabled() || !m_document) return;
    const QStringList selected = m_document->selectedMediaIds();
    for (const QString& id : selected) handleOverlayDelete(id);
}

void QuickCanvasController::handleOverlayDelete(const QString& id)
{
    if (m_projectEditingEnabled && m_document && m_document->removeMedia(id)) {
        emit mediaDeleteRequested(id);
    }
}

void QuickCanvasController::handleOverlayMuteToggle(const QString& id)
{
    if (!editingEnabled()) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && media->residencyReady() && !editsLocked()
        && id == primarySelectedMediaId()) {
        media->beginElementEdit();
        media->setMuted(!media->muted());
    }
    emit mediaMuteToggleRequested(id);
}

void QuickCanvasController::handleOverlayVolumeChange(const QString& id, qreal value)
{
    if (!editingEnabled()) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && media->residencyReady() && !editsLocked()) {
        if (id != primarySelectedMediaId() || !std::isfinite(value)) return;
        media->beginElementEdit();
        const int percent = qRound(std::clamp<qreal>(value, 0.0, 1.0) * 100.0);
        MediaSettingsState settings = media->settings();
        settings.volumeOverrideEnabled = true;
        settings.volumeText = QString::number(percent);
        media->setSettings(settings);
        // The slider restores its authoritative binding on release. Publish
        // before returning so it cannot briefly restore the previous volume.
        publishVideoState();
    }
    emit mediaVolumeChangeRequested(id, value);
}

void QuickCanvasController::handleOverlayFitToTextToggle(const QString& id)
{
    if (!editingEnabled()) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isText() && !editsLocked() && id == primarySelectedMediaId()) {
        media->beginElementEdit();
        media->setFitToTextEnabled(!media->fitToTextEnabled());
    }
    emit mediaFitToTextToggleRequested(id);
}

void QuickCanvasController::handleOverlayHorizontalAlign(
    const QString& id, const QString& alignment)
{
    if (!editingEnabled()) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isText() && !editsLocked() && id == primarySelectedMediaId()) {
        media->beginElementEdit();
        media->setHorizontalAlignment(alignment);
    }
    emit mediaHorizontalAlignRequested(id, alignment);
}

void QuickCanvasController::handleOverlayVerticalAlign(
    const QString& id, const QString& alignment)
{
    if (!editingEnabled()) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isText() && !editsLocked() && id == primarySelectedMediaId()) {
        media->beginElementEdit();
        media->setVerticalAlignment(alignment);
    }
    emit mediaVerticalAlignRequested(id, alignment);
}

bool QuickCanvasController::beginLocalFileDrag(const QVariantList& urls,
                                               qreal viewX, qreal viewY)
{
    Q_UNUSED(viewX);
    Q_UNUSED(viewY);
    cancelLocalFileDrag();
    if (!m_projectEditingEnabled || editsLocked() || urls.size() != 1) return false;
    const QUrl url = urls.first().canConvert<QUrl>()
        ? urls.first().toUrl() : QUrl(urls.first().toString());
    if (!url.isLocalFile()) return false;
    const QFileInfo info(url.toLocalFile());
    if (!info.isFile() || info.isSymLink()) return false;
    // Drag acceptance does not read, decode, hash or allocate media content.
    m_dropPath = info.absoluteFilePath();
    return true;
}

bool QuickCanvasController::updateLocalFileDrag(qreal viewX, qreal viewY)
{
    Q_UNUSED(viewX);
    Q_UNUSED(viewY);
    return m_projectEditingEnabled && !m_dropPath.isEmpty() && !editsLocked();
}

bool QuickCanvasController::commitLocalFileDrop(qreal viewX, qreal viewY)
{
    if (!m_projectEditingEnabled || m_dropPath.isEmpty()
        || !m_document || editsLocked()) return false;
    const QString path = std::exchange(m_dropPath, {});
    const QPointF center = mapViewPointToScene({viewX, viewY});
    return !m_document->queueFileImport(path, center).isEmpty();
}

void QuickCanvasController::cancelLocalFileDrag()
{
    m_dropPath.clear();
}

void QuickCanvasController::publishSnapGuides(const QVariantList& guides)
{
    m_snapGuidesModel = guides;
    emit presentationChanged();
}
