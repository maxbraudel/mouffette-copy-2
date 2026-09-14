#include "frontend/rendering/canvas/QuickCanvasController.h"

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacVideoThumbnailer.h"
#elif defined(Q_OS_WIN)
#include "backend/platform/windows/WindowsVideoThumbnailer.h"
#endif

#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qreal kSnapDistancePx = 10.0;
constexpr qreal kCornerSnapDistancePx = 20.0;
constexpr qreal kSnapReleaseFactor = 1.4;
constexpr qreal kSnapEpsilon = 1e-5;

QImage limitedPreviewImage(QImage image)
{
    constexpr int maximumEdge = 2048;
    if (image.isNull()
        || (image.width() <= maximumEdge && image.height() <= maximumEdge)) {
        return image;
    }
    return image.scaled(QSize(maximumEdge, maximumEdge),
                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QSize nativeVideoDimensions(const QString& path)
{
#ifdef Q_OS_MACOS
    return MacVideoThumbnailer::videoDimensions(path);
#elif defined(Q_OS_WIN)
    return WindowsVideoThumbnailer::videoDimensions(path);
#else
    Q_UNUSED(path);
    return {};
#endif
}

QImage nativeVideoFirstFrame(const QString& path)
{
#ifdef Q_OS_MACOS
    return limitedPreviewImage(MacVideoThumbnailer::firstFrame(path));
#elif defined(Q_OS_WIN)
    return limitedPreviewImage(WindowsVideoThumbnailer::firstFrame(path));
#else
    Q_UNUSED(path);
    return {};
#endif
}

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
    , m_document(document)
    , m_mediaListModel(new MediaListModel(this))
    , m_dropFrameSource(new RemoteVideoFrameSource(this))
    , m_videoStateTimer(new QTimer(this))
{
    Q_ASSERT(document);
    connect(document, &CanvasDocument::mediaAdded,
            this, &QuickCanvasController::publishMedia);
    connect(document, &CanvasDocument::mediaRemoved,
            this, &QuickCanvasController::publishMedia);
    connect(document, &CanvasDocument::mediaChanged,
            this, &QuickCanvasController::publishMedia);
    connect(document, &CanvasDocument::selectionChanged,
            this, [this]() {
        const QString id = m_document && m_document->selectedMedia()
            ? m_document->selectedMedia()->mediaId() : QString();
        if (id != m_lastSelectedId) {
            m_lastSelectedId = id;
            emit selectedMediaChanged();
        }
        publishSelection();
    });
    connect(document, &CanvasDocument::screensChanged,
            this, &QuickCanvasController::publishScreens);
    connect(document, &CanvasDocument::remoteCursorChanged,
            this, &QuickCanvasController::publishRemoteCursor);
    m_videoStateTimer->setInterval(50);
    connect(m_videoStateTimer, &QTimer::timeout,
            this, &QuickCanvasController::publishVideoState);
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
    m_renderWindow = window;
    ensureInitialFit();
}

QQuickWindow* QuickCanvasController::renderWindow() const
{
    return m_renderWindow;
}

CanvasMedia* QuickCanvasController::selectedMediaItem() const
{
    return m_document ? m_document->selectedMedia() : nullptr;
}

QObject* QuickCanvasController::mediaModel() const
{
    return m_mediaListModel;
}

QObject* QuickCanvasController::dropPreviewFrameSource() const
{
    return m_dropFrameSource;
}

bool QuickCanvasController::editsLocked() const
{
    return !m_document || m_document->editsLocked();
}

void QuickCanvasController::publishAll()
{
    m_viewScale = m_document->cameraScale();
    m_panX = m_document->cameraPanX();
    m_panY = m_document->cameraPanY();
    publishScreens();
    publishMedia();
    publishSelection();
    publishRemoteCursor();
    publishVideoState();
    publishDropPreview(false);
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
            if (item) list.append(item->toModelMap());
        }
    }
    m_mediaListModel->updateFromList(list);
    m_mediaSnapshot = list;
    publishSelection();
    emit presentationChanged();
}

void QuickCanvasController::publishSelection()
{
    if (!m_document) return;
    QVariantList list;
    for (CanvasMedia* media : m_document->media()) {
        if (!media || !media->selected()) continue;
        const QRectF rect = media->sceneRect();
        list.append(QVariantMap{{QStringLiteral("mediaId"), media->mediaId()},
                                {QStringLiteral("x"), rect.x()},
                                {QStringLiteral("y"), rect.y()},
                                {QStringLiteral("width"), rect.width()},
                                {QStringLiteral("height"), rect.height()}});
    }
    m_selectionChromeModel = list;
    emit presentationChanged();
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
            const QString type = zone.type.toLower();
            zones.append(QVariantMap{
                {QStringLiteral("screenId"), screen.id},
                {QStringLiteral("type"), zone.type},
                {QStringLiteral("x"), zoneRect.x()},
                {QStringLiteral("y"), zoneRect.y()},
                {QStringLiteral("width"), zoneRect.width()},
                {QStringLiteral("height"), std::max<qreal>(3.0, zoneRect.height())},
                {QStringLiteral("fillColor"),
                    type == QLatin1String("taskbar") || type == QLatin1String("dock")
                        || type == QLatin1String("menu_bar")
                    ? QStringLiteral("#50000000") : QStringLiteral("#5A808080")}});
        }
    }
    m_screensModel = screens;
    m_uiZonesModel = zones;
    emit presentationChanged();
}

void QuickCanvasController::publishRemoteCursor()
{
    if (!m_document) return;
    m_remoteCursorVisible = m_document->remoteCursorVisible();
    m_remoteCursorX = m_document->remoteCursorPosition().x();
    m_remoteCursorY = m_document->remoteCursorPosition().y();
    emit presentationChanged();
}

void QuickCanvasController::publishVideoState()
{
    if (!m_document) return;
    QVariantMap states;
    for (CanvasMedia* media : m_document->media()) {
        if (!media || !media->isVideo()) continue;
        const qint64 duration = media->player() ? media->player()->duration() : 0;
        const qreal progress = duration > 0
            ? std::clamp<qreal>(media->positionMs() / qreal(duration), 0.0, 1.0) : 0.0;
        states.insert(media->mediaId(), QVariantMap{
            {QStringLiteral("mediaId"), media->mediaId()},
            {QStringLiteral("isPlaying"), media->isPlaying()},
            {QStringLiteral("isMuted"), media->muted()},
            {QStringLiteral("isLooping"), media->repeatEnabled()},
            {QStringLiteral("progress"), progress},
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

void QuickCanvasController::updateRemoteCursor(int globalX, int globalY)
{
    if (!m_document) return;
    QPointF mapped;
    if (m_document->mapRemoteCursor(globalX, globalY, &mapped)) {
        m_document->setRemoteCursor(true, mapped);
    }
}

void QuickCanvasController::hideRemoteCursor()
{
    if (m_document) m_document->setRemoteCursor(false, m_document->remoteCursorPosition());
}

void QuickCanvasController::resetView()
{
    if (!m_document) return;
    m_document->resetCamera();
    updateCamera(1.0, 0.0, 0.0);
}

void QuickCanvasController::recenterView()
{
    const QRectF bounds = allScreenBounds(m_document);
    if (bounds.isEmpty()) return;
    const qreal width = m_renderWindow ? m_renderWindow->width() : bounds.width() + 106.0;
    const qreal height = m_renderWindow ? m_renderWindow->height() : bounds.height() + 106.0;
    const qreal scale = std::clamp(std::min((width - 106.0) / bounds.width(),
                                            (height - 106.0) / bounds.height()),
                                   0.2, 10.0);
    updateCamera(scale, width / 2.0 - bounds.center().x() * scale,
                 height / 2.0 - bounds.center().y() * scale);
}

void QuickCanvasController::setTextToolActive(bool active)
{
    if (m_textToolActive == active) return;
    m_textToolActive = active;
    emit presentationChanged();
}

void QuickCanvasController::setProjectEditingEnabled(bool enabled)
{
    if (m_projectEditingEnabled == enabled) return;
    m_projectEditingEnabled = enabled;
    if (enabled) return;

    // Revocation is synchronous: discard every provisional edit so a delayed
    // QML release event cannot commit it after the project has been deleted.
    cancelLocalFileDrag();
    m_dragMediaId.clear();
    m_lastMoveSnapped = false;
    m_liveSnapDragMediaId.clear();
    clearLiveResize();
    setTextToolActive(false);
    publishSnapGuides({});
}

qreal QuickCanvasController::currentViewScale() const
{
    return m_viewScale > 0.0001 ? m_viewScale : 1.0;
}

void QuickCanvasController::ensureInitialFit(int marginPx)
{
    if (m_initialFitDone || !m_renderWindow || !m_document
        || !m_document->hasActiveScreens()) return;
    const QRectF bounds = allScreenBounds(m_document);
    if (bounds.isEmpty()) return;
    const qreal availableWidth = std::max<qreal>(1.0, m_renderWindow->width() - 2.0 * marginPx);
    const qreal availableHeight = std::max<qreal>(1.0, m_renderWindow->height() - 2.0 * marginPx);
    const qreal scale = std::clamp(std::min(availableWidth / bounds.width(),
                                            availableHeight / bounds.height()),
                                   0.2, 10.0);
    updateCamera(scale,
        m_renderWindow->width() / 2.0 - bounds.center().x() * scale,
        m_renderWindow->height() / 2.0 - bounds.center().y() * scale);
    m_initialFitDone = true;
}

void QuickCanvasController::updateCamera(qreal scale, qreal panX, qreal panY)
{
    scale = std::clamp(scale, 0.2, 10.0);
    if (qFuzzyCompare(1.0 + m_viewScale, 1.0 + scale)
        && qFuzzyCompare(1.0 + m_panX, 1.0 + panX)
        && qFuzzyCompare(1.0 + m_panY, 1.0 + panY)) return;
    m_viewScale = scale;
    m_panX = panX;
    m_panY = panY;
    if (m_document) m_document->setCamera(scale, panX, panY);
    emit presentationChanged();
}

QPointF QuickCanvasController::mapViewPointToScene(const QPointF& point) const
{
    const qreal scale = std::max<qreal>(0.0001, m_viewScale);
    return {(point.x() - m_panX) / scale,
            (point.y() - m_panY) / scale};
}

void QuickCanvasController::handleMediaSelectRequested(const QString& mediaId,
                                                        bool additive)
{
    if (m_document && !m_document->editsLocked()) m_document->select(mediaId, additive);
}

void QuickCanvasController::handleClearSelectionRequested()
{
    if (m_document && !m_document->editsLocked()) m_document->clearSelection();
}

void QuickCanvasController::handleMediaMoveStarted(const QString& mediaId,
                                                    qreal, qreal, bool)
{
    if (!m_projectEditingEnabled || editsLocked()) return;
    m_dragMediaId = mediaId;
    m_lastMoveSnapped = false;
    m_liveSnapDragMediaId.clear();
    if (CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr) {
        rebuildSnapTargets(media);
    } else {
        clearSnapTargets();
    }
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
        if (!media || media == activeMedia) continue;
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
    const qreal viewScale = std::max<qreal>(0.0001, currentViewScale());
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
    if (!m_projectEditingEnabled) return;
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (!media || editsLocked()) return;
    if (m_dragMediaId != mediaId) {
        m_dragMediaId = mediaId;
        rebuildSnapTargets(media);
    }
    if (!snap) {
        m_lastMoveSnapped = false;
        m_liveSnapDragMediaId.clear();
        publishSnapGuides({});
        return;
    }
    QVariantList guides;
    m_lastSnappedPosition = snappedPosition(media, {x, y}, &guides);
    m_lastMoveSnapped = !guides.isEmpty();
    if (m_lastMoveSnapped) {
        m_liveSnapDragMediaId = mediaId;
        m_liveSnapDragX = m_lastSnappedPosition.x();
        m_liveSnapDragY = m_lastSnappedPosition.y();
        emit presentationChanged();
    } else {
        // Leaving a target while Shift remains pressed must immediately release
        // the previous frozen live position.
        m_liveSnapDragMediaId.clear();
    }
    publishSnapGuides(guides);
}

void QuickCanvasController::handleMediaMoveEnded(const QString& mediaId,
                                                  qreal x, qreal y, bool snap)
{
    if (!m_projectEditingEnabled) {
        m_dragMediaId.clear();
        m_lastMoveSnapped = false;
        m_liveSnapDragMediaId.clear();
        clearSnapTargets();
        publishSnapGuides({});
        return;
    }
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (media && !editsLocked()) {
        media->setPosition(snap && m_lastMoveSnapped
                               ? m_lastSnappedPosition : QPointF(x, y));
    }
    m_dragMediaId.clear();
    m_lastMoveSnapped = false;
    m_liveSnapDragMediaId.clear();
    clearSnapTargets();
    publishSnapGuides({});
    publishMedia();
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

    const qreal viewScale = std::max<qreal>(0.0001, currentViewScale());
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

    const qreal viewScale = std::max<qreal>(0.0001, currentViewScale());
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

void QuickCanvasController::handleMediaResizeRequested(
    const QString& mediaId, const QString& handleId, qreal x, qreal y,
    bool snap, bool altPressed)
{
    if (!m_projectEditingEnabled) return;
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (!media || editsLocked()) return;
    if (m_resizeMediaId != mediaId) {
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
        if (media->isText() && media->fitToTextEnabled()) {
            media->setFitToTextEnabled(false);
        }
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
    publishSnapGuides(guides);
}

void QuickCanvasController::handleMediaResizeEnded(const QString& mediaId)
{
    if (!m_projectEditingEnabled) {
        clearLiveResize();
        return;
    }
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (media && mediaId == m_resizeMediaId && !m_pendingResizeRect.isEmpty()) {
        media->setPosition(m_pendingResizeRect.topLeft());
        if (m_pendingResizeAlt) {
            media->setBaseSize(QSize(
                std::max(1, qRound(m_pendingResizeRect.width()
                                   / m_resizeOriginalScale)),
                std::max(1, qRound(m_pendingResizeRect.height()
                                   / m_resizeOriginalScale))));
            media->setScale(m_resizeOriginalScale);
        } else {
            media->setScale(m_pendingResizeRect.width()
                            / std::max<qreal>(1.0, media->baseSize().width()));
        }
    }
    clearLiveResize();
    publishMedia();
}

void QuickCanvasController::clearLiveResize()
{
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
    if (!m_projectEditingEnabled) return;
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (media && media->isText() && !editsLocked()) media->setText(text);
}

void QuickCanvasController::handleTextCreateRequested(qreal viewX, qreal viewY)
{
    if (!m_projectEditingEnabled || !m_document || editsLocked()) return;
    m_document->addText(mapViewPointToScene({viewX, viewY}));
    setTextToolActive(false);
}

void QuickCanvasController::handleOverlayVisibilityToggle(const QString& id, bool visible)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && !editsLocked()) media->setContentVisible(visible);
    emit mediaVisibilityToggleRequested(id, visible);
}

void QuickCanvasController::handleOverlayBringForward(const QString& id)
{
    if (!m_projectEditingEnabled) return;
    if (m_document) m_document->moveForward(id);
    emit mediaBringForwardRequested(id);
}

void QuickCanvasController::handleOverlayBringBackward(const QString& id)
{
    if (!m_projectEditingEnabled) return;
    if (m_document) m_document->moveBackward(id);
    emit mediaBringBackwardRequested(id);
}

void QuickCanvasController::handleOverlayDelete(const QString& id)
{
    if (m_projectEditingEnabled && m_document && m_document->removeMedia(id)) {
        emit mediaDeleteRequested(id);
    }
}

void QuickCanvasController::handleOverlayPlayPause(const QString& id)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->togglePlayPause();
    emit mediaPlayPauseRequested(id);
}

void QuickCanvasController::handleOverlayStop(const QString& id)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->stopToBeginning();
    emit mediaStopRequested(id);
}

void QuickCanvasController::handleOverlayRepeatToggle(const QString& id)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) {
        media->setRepeatEnabled(!media->repeatEnabled());
    }
    emit mediaRepeatToggleRequested(id);
}

void QuickCanvasController::handleOverlayMuteToggle(const QString& id)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->setMuted(!media->muted());
    emit mediaMuteToggleRequested(id);
}

void QuickCanvasController::handleOverlayVolumeChange(const QString& id, qreal value)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) {
        const int percent = qRound(std::clamp<qreal>(value, 0.0, 1.0) * 100.0);
        MediaSettingsState settings = media->settings();
        settings.volumeOverrideEnabled = true;
        settings.volumeText = QString::number(percent);
        media->setSettings(settings);
    }
    emit mediaVolumeChangeRequested(id, value);
}

void QuickCanvasController::handleOverlaySeekBegin(const QString& id, qreal ratio)
{
    if (!m_projectEditingEnabled) return;
    handleOverlaySeekUpdate(id, ratio);
}

void QuickCanvasController::handleOverlaySeekUpdate(const QString& id, qreal ratio)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->seekToRatio(ratio);
}

void QuickCanvasController::handleOverlaySeekEnd(const QString& id, qreal ratio)
{
    if (!m_projectEditingEnabled) return;
    handleOverlaySeekUpdate(id, ratio);
    emit mediaSeekRequested(id, ratio);
}

void QuickCanvasController::handleOverlayFitToTextToggle(const QString& id)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isText() && !editsLocked()) {
        media->setFitToTextEnabled(!media->fitToTextEnabled());
    }
    emit mediaFitToTextToggleRequested(id);
}

void QuickCanvasController::handleOverlayHorizontalAlign(
    const QString& id, const QString& alignment)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isText() && !editsLocked()) {
        media->setHorizontalAlignment(alignment);
    }
    emit mediaHorizontalAlignRequested(id, alignment);
}

void QuickCanvasController::handleOverlayVerticalAlign(
    const QString& id, const QString& alignment)
{
    if (!m_projectEditingEnabled) return;
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isText() && !editsLocked()) {
        media->setVerticalAlignment(alignment);
    }
    emit mediaVerticalAlignRequested(id, alignment);
}

bool QuickCanvasController::beginLocalFileDrag(const QVariantList& urls,
                                               qreal viewX, qreal viewY)
{
    cancelLocalFileDrag();
    if (!m_projectEditingEnabled || editsLocked() || urls.size() != 1) return false;
    const QUrl url = urls.first().canConvert<QUrl>()
        ? urls.first().toUrl() : QUrl(urls.first().toString());
    if (!url.isLocalFile()) return false;
    const QFileInfo info(url.toLocalFile());
    const QString path = info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    const auto validation = MediaFilePolicy::validateLocalFile(path);
    if (!validation.accepted()) {
        TOAST_WARNING(QStringLiteral("Import refused: %1 — %2")
            .arg(info.fileName(), MediaFilePolicy::validationErrorDescription(validation)));
        return false;
    }
    m_dropPath = path;
    m_dropVideo = validation.kind == MediaFilePolicy::Kind::Mp4Video;
    m_dropCenter = mapViewPointToScene({viewX, viewY});
    if (m_dropVideo) {
        // Validation has already decoded one frame with the same Qt/FFmpeg
        // backend used for playback. Reuse it: immediately opening the file a
        // second time through Media Foundation is timing-dependent on Windows.
        m_dropNativeSize = validation.videoSize;
        m_dropFrame = limitedPreviewImage(validation.videoFirstFrame);
        if (m_dropNativeSize.isEmpty() || m_dropFrame.isNull()) {
            m_dropNativeSize = nativeVideoDimensions(path);
            m_dropFrame = nativeVideoFirstFrame(path);
        }
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        m_dropFrame = reader.read();
        m_dropNativeSize = validation.imageSize.isEmpty()
            ? m_dropFrame.size() : validation.imageSize;
    }
    if (m_dropNativeSize.isEmpty() || m_dropFrame.isNull()) {
        cancelLocalFileDrag();
        return false;
    }
    m_dropFrameSource->setFrame(m_dropFrame);
    publishDropPreview(true);
    return true;
}

bool QuickCanvasController::updateLocalFileDrag(qreal viewX, qreal viewY)
{
    if (!m_projectEditingEnabled || m_dropPath.isEmpty() || editsLocked()) return false;
    m_dropCenter = mapViewPointToScene({viewX, viewY});
    publishDropPreview(true);
    return true;
}

bool QuickCanvasController::commitLocalFileDrop(qreal viewX, qreal viewY)
{
    if (!m_projectEditingEnabled || m_dropPath.isEmpty()
        || !m_document || editsLocked()) return false;
    m_dropCenter = mapViewPointToScene({viewX, viewY});
    const QPointF topLeft = m_dropCenter
        - QPointF(m_dropNativeSize.width() / 2.0,
                  m_dropNativeSize.height() / 2.0);
    CanvasMedia* media = m_document->addPreparedFile(
        m_dropPath, m_dropNativeSize, m_dropVideo, topLeft);
    if (!media) return false;
    publishDropPreview(true, media->mediaId());
    return true;
}

void QuickCanvasController::cancelLocalFileDrag()
{
    m_dropPath.clear();
    m_dropNativeSize = {};
    m_dropVideo = false;
    m_dropFrame = {};
    m_dropFrameSource->clear();
    publishDropPreview(false);
}

void QuickCanvasController::publishDropPreview(bool visible,
                                               const QString& handoffId)
{
    const QPointF topLeft = m_dropCenter
        - QPointF(m_dropNativeSize.width() / 2.0,
                  m_dropNativeSize.height() / 2.0);
    m_dropPreviewModel = QVariantMap{
        {QStringLiteral("visible"), visible},
        {QStringLiteral("frameReady"), !m_dropFrame.isNull()},
        {QStringLiteral("x"), topLeft.x()}, {QStringLiteral("y"), topLeft.y()},
        {QStringLiteral("width"), m_dropNativeSize.width()},
        {QStringLiteral("height"), m_dropNativeSize.height()},
        {QStringLiteral("handoffMediaId"), handoffId},
        {QStringLiteral("displayName"), QFileInfo(m_dropPath).fileName()}};
    emit presentationChanged();
}

void QuickCanvasController::handleDropPreviewContentReady(const QString& mediaId)
{
    if (m_dropPreviewModel.value(QStringLiteral("handoffMediaId")).toString()
        != mediaId) return;
    cancelLocalFileDrag();
}

void QuickCanvasController::publishSnapGuides(const QVariantList& guides)
{
    m_snapGuidesModel = guides;
    emit presentationChanged();
}
