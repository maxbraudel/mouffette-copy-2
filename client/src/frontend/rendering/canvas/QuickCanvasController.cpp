#include "frontend/rendering/canvas/QuickCanvasController.h"

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {
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
    m_dragMediaId = mediaId;
    m_lastMoveSnapped = false;
    publishSnapGuides({});
}

QPointF QuickCanvasController::snappedPosition(CanvasMedia* media,
                                               const QPointF& proposed,
                                               QVariantList* guides) const
{
    if (!media || !m_document) return proposed;
    const qreal threshold = 8.0 / currentViewScale();
    const QSizeF size(media->sceneRect().size());
    qreal bestDx = threshold + 1.0;
    qreal bestDy = threshold + 1.0;
    qreal outX = proposed.x();
    qreal outY = proposed.y();
    qreal guideX = 0.0;
    qreal guideY = 0.0;
    QList<QRectF> targets = m_document->screenRects().values();
    for (CanvasMedia* other : m_document->media()) {
        if (other != media) targets.append(other->sceneRect());
    }
    for (const QRectF& target : targets) {
        const qreal sourceX[] = {proposed.x(), proposed.x() + size.width() / 2.0,
                                 proposed.x() + size.width()};
        const qreal targetX[] = {target.left(), target.center().x(), target.right()};
        for (qreal sx : sourceX) for (qreal tx : targetX) {
            const qreal delta = tx - sx;
            if (std::abs(delta) < std::abs(bestDx)) {
                bestDx = delta; outX = proposed.x() + delta; guideX = tx;
            }
        }
        const qreal sourceY[] = {proposed.y(), proposed.y() + size.height() / 2.0,
                                 proposed.y() + size.height()};
        const qreal targetY[] = {target.top(), target.center().y(), target.bottom()};
        for (qreal sy : sourceY) for (qreal ty : targetY) {
            const qreal delta = ty - sy;
            if (std::abs(delta) < std::abs(bestDy)) {
                bestDy = delta; outY = proposed.y() + delta; guideY = ty;
            }
        }
    }
    const QRectF bounds = allScreenBounds(m_document).adjusted(-10000, -10000, 10000, 10000);
    if (std::abs(bestDx) <= threshold && guides) {
        guides->append(guide(guideX, bounds.top(), guideX, bounds.bottom()));
    }
    if (std::abs(bestDy) <= threshold && guides) {
        guides->append(guide(bounds.left(), guideY, bounds.right(), guideY));
    }
    if (std::abs(bestDx) > threshold) outX = proposed.x();
    if (std::abs(bestDy) > threshold) outY = proposed.y();
    return {outX, outY};
}

void QuickCanvasController::handleMediaMoveUpdated(const QString& mediaId,
                                                    qreal x, qreal y, bool snap)
{
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (!media || editsLocked()) return;
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
    }
    publishSnapGuides(guides);
}

void QuickCanvasController::handleMediaMoveEnded(const QString& mediaId,
                                                  qreal x, qreal y, bool snap)
{
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (media && !editsLocked()) {
        media->setPosition(snap && m_lastMoveSnapped
                               ? m_lastSnappedPosition : QPointF(x, y));
    }
    m_dragMediaId.clear();
    m_lastMoveSnapped = false;
    m_liveSnapDragMediaId.clear();
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
        if (moveLeft || moveRight) height = width / ratio;
        else width = height * ratio;
        QPointF origin = fixed;
        if (moveLeft) origin.rx() -= width;
        if (moveTop) origin.ry() -= height;
        if (!moveLeft && !moveRight) origin.rx() -= width / 2.0;
        if (!moveTop && !moveBottom) origin.ry() -= height / 2.0;
        rect = QRectF(origin, QSizeF(width, height)).normalized();
    }
    return rect;
}

void QuickCanvasController::handleMediaResizeRequested(
    const QString& mediaId, const QString& handleId, qreal x, qreal y,
    bool snap, bool altPressed)
{
    Q_UNUSED(snap)
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (!media || editsLocked()) return;
    if (m_resizeMediaId != mediaId) {
        m_resizeMediaId = mediaId;
        m_resizeOriginalRect = media->sceneRect();
    }
    m_pendingResizeAlt = altPressed;
    m_pendingResizeRect = resizedRect(m_resizeOriginalRect, handleId, {x, y},
                                      !altPressed);
    if (altPressed) {
        m_liveResizeActive = false;
        m_liveAltResizeActive = true;
        m_liveAltResizeMediaId = mediaId;
        m_liveAltResizeRect = m_pendingResizeRect;
    } else {
        const qreal scale = m_pendingResizeRect.width()
            / std::max<qreal>(1.0, media->baseSize().width());
        m_liveAltResizeActive = false;
        m_liveResizeActive = true;
        m_liveResizeMediaId = mediaId;
        m_liveResizeRect = m_pendingResizeRect;
        m_liveResizeScale = scale;
    }
    emit presentationChanged();
}

void QuickCanvasController::handleMediaResizeEnded(const QString& mediaId)
{
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (media && mediaId == m_resizeMediaId && !m_pendingResizeRect.isEmpty()) {
        media->setPosition(m_pendingResizeRect.topLeft());
        if (m_pendingResizeAlt) {
            media->setBaseSize(m_pendingResizeRect.size().toSize());
            media->setScale(1.0);
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
    m_resizeMediaId.clear();
    m_resizeOriginalRect = {};
    m_pendingResizeRect = {};
    m_pendingResizeAlt = false;
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
    CanvasMedia* media = m_document ? m_document->mediaById(mediaId) : nullptr;
    if (media && media->isText() && !editsLocked()) media->setText(text);
}

void QuickCanvasController::handleTextCreateRequested(qreal viewX, qreal viewY)
{
    if (!m_document || editsLocked()) return;
    m_document->addText(mapViewPointToScene({viewX, viewY}));
    setTextToolActive(false);
}

void QuickCanvasController::handleOverlayVisibilityToggle(const QString& id, bool visible)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && !editsLocked()) media->setContentVisible(visible);
    emit mediaVisibilityToggleRequested(id, visible);
}

void QuickCanvasController::handleOverlayBringForward(const QString& id)
{
    if (m_document) m_document->moveForward(id);
    emit mediaBringForwardRequested(id);
}

void QuickCanvasController::handleOverlayBringBackward(const QString& id)
{
    if (m_document) m_document->moveBackward(id);
    emit mediaBringBackwardRequested(id);
}

void QuickCanvasController::handleOverlayDelete(const QString& id)
{
    if (m_document && m_document->removeMedia(id)) emit mediaDeleteRequested(id);
}

void QuickCanvasController::handleOverlayPlayPause(const QString& id)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->togglePlayPause();
    emit mediaPlayPauseRequested(id);
}

void QuickCanvasController::handleOverlayStop(const QString& id)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->stopToBeginning();
    emit mediaStopRequested(id);
}

void QuickCanvasController::handleOverlayRepeatToggle(const QString& id)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) {
        media->setRepeatEnabled(!media->repeatEnabled());
    }
    emit mediaRepeatToggleRequested(id);
}

void QuickCanvasController::handleOverlayMuteToggle(const QString& id)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->setMuted(!media->muted());
    emit mediaMuteToggleRequested(id);
}

void QuickCanvasController::handleOverlayVolumeChange(const QString& id, qreal value)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->setVolume(value);
    emit mediaVolumeChangeRequested(id, value);
}

void QuickCanvasController::handleOverlaySeekBegin(const QString& id, qreal ratio)
{
    handleOverlaySeekUpdate(id, ratio);
}

void QuickCanvasController::handleOverlaySeekUpdate(const QString& id, qreal ratio)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isVideo() && !editsLocked()) media->seekToRatio(ratio);
}

void QuickCanvasController::handleOverlaySeekEnd(const QString& id, qreal ratio)
{
    handleOverlaySeekUpdate(id, ratio);
    emit mediaSeekRequested(id, ratio);
}

void QuickCanvasController::handleOverlayFitToTextToggle(const QString& id)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isText() && !editsLocked()) {
        media->setFitToTextEnabled(!media->fitToTextEnabled());
    }
    emit mediaFitToTextToggleRequested(id);
}

void QuickCanvasController::handleOverlayHorizontalAlign(
    const QString& id, const QString& alignment)
{
    if (CanvasMedia* media = m_document ? m_document->mediaById(id) : nullptr;
        media && media->isText() && !editsLocked()) {
        media->setHorizontalAlignment(alignment);
    }
    emit mediaHorizontalAlignRequested(id, alignment);
}

void QuickCanvasController::handleOverlayVerticalAlign(
    const QString& id, const QString& alignment)
{
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
    if (editsLocked() || urls.size() != 1) return false;
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
        m_dropNativeSize = QSize(1920, 1080);
        m_dropFrame = QImage(640, 360, QImage::Format_RGB32);
        m_dropFrame.fill(Qt::black);
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
    if (m_dropPath.isEmpty() || editsLocked()) return false;
    m_dropCenter = mapViewPointToScene({viewX, viewY});
    publishDropPreview(true);
    return true;
}

bool QuickCanvasController::commitLocalFileDrop(qreal viewX, qreal viewY)
{
    if (m_dropPath.isEmpty() || !m_document || editsLocked()) return false;
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
