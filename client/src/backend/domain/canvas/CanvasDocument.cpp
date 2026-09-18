#include "backend/domain/canvas/CanvasDocument.h"

#include "backend/domain/media/CanvasMedia.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/media/MediaSettingsState.h"
#include "backend/files/FileManager.h"
#include "backend/media/MediaDecoder.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QPointer>
#include <QThreadPool>
#include <QUuid>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

QThreadPool* importMetadataPool()
{
    // A new shell must not queue behind full-file validation, hashing or other
    // bulk QtConcurrent work. Bound this short metadata lane across documents.
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance());
        pool->setMaxThreadCount(2);
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                         pool, [] { if (pool) pool->waitForDone(); });
    }
    return pool;
}

QString sourceSignature(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || !info.isReadable()) return {};
    // Metadata only: saving a pending import must not read or hash its content.
    return QStringLiteral("%1:%2:%3:%4")
        .arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch())
        .arg(info.birthTime().toMSecsSinceEpoch())
        .arg(info.fileTime(QFileDevice::FileMetadataChangeTime).toMSecsSinceEpoch());
}

QJsonObject spanForIntersection(int screenId, const QRectF& screen,
                                const QRectF& media)
{
    const QRectF intersection = screen.intersected(media);
    if (intersection.isEmpty() || screen.isEmpty() || media.isEmpty()) return {};
    return {
        {QStringLiteral("screenId"), screenId},
        {QStringLiteral("normX"), (media.x() - screen.x()) / screen.width()},
        {QStringLiteral("normY"), (media.y() - screen.y()) / screen.height()},
        {QStringLiteral("normW"), media.width() / screen.width()},
        {QStringLiteral("normH"), media.height() / screen.height()},
        {QStringLiteral("spanDestNormX"), (intersection.x() - screen.x()) / screen.width()},
        {QStringLiteral("spanDestNormY"), (intersection.y() - screen.y()) / screen.height()},
        {QStringLiteral("spanDestNormW"), intersection.width() / screen.width()},
        {QStringLiteral("spanDestNormH"), intersection.height() / screen.height()},
        {QStringLiteral("spanSourceNormX"), (intersection.x() - media.x()) / media.width()},
        {QStringLiteral("spanSourceNormY"), (intersection.y() - media.y()) / media.height()},
        {QStringLiteral("spanSourceNormW"), intersection.width() / media.width()},
        {QStringLiteral("spanSourceNormH"), intersection.height() / media.height()}
    };
}
}

CanvasDocument::CanvasDocument(QObject* parent)
    : QObject(parent)
{
    m_timelineSettings.maxDurationMs=AppConfig::instance().timelineMaxDurationMs();
    m_timelineSettings.slotsPerSecond=AppConfig::instance().timelineSlotsPerSecond();
}

CanvasDocument::~CanvasDocument()
{
    cancelPendingImportTasks();
}

void CanvasDocument::cancelPendingImportTasks()
{
    ++m_importGeneration;
    for (const PendingImport& pending : std::as_const(m_pendingImports))
        if (pending.cancelled) pending.cancelled->store(true);
    m_activeImports.clear();
}

void CanvasDocument::setClientWorkspaceId(const QString& id)
{
    if (m_projectId == id) return;
    cancelPendingImportTasks();
    m_projectId = id;
    for (const QString& mediaId : m_pendingImports.keys()) startPendingImport(mediaId);
}

QString CanvasDocument::queueFileImport(const QString& sourcePath,
                                       const QPointF& center)
{
    if (m_editsLocked || !std::isfinite(center.x()) || !std::isfinite(center.y())) return {};
    const QFileInfo info(sourcePath);
    const QString signature = sourceSignature(sourcePath);
    if (signature.isEmpty()) return {};
    PendingImport pending;
    pending.mediaId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending.sourcePath = info.canonicalFilePath();
    pending.sourceSignature = signature;
    pending.center = center;
    const QString id = pending.mediaId;
    m_pendingImports.insert(id, pending);
    emit pendingImportsChanged();
    emit documentChanged();
    startPendingImport(id);
    return id;
}

void CanvasDocument::startPendingImport(const QString& mediaId)
{
    if (m_editsLocked || m_mediaResidencySuspended || !m_pendingImports.contains(mediaId)
        || m_activeImports.contains(mediaId)) return;
    auto& stored = m_pendingImports[mediaId];
    stored.cancelled = std::make_shared<std::atomic_bool>(false);
    const PendingImport pending = stored;
    const quint64 generation = m_importGeneration;
    m_activeImports.insert(mediaId);
    auto* watcher = new QFutureWatcher<MediaDecoder::Geometry>(this);
    connect(watcher, &QFutureWatcher<MediaDecoder::Geometry>::finished, this,
            [this, watcher, pending, generation] {
        const auto probe = watcher->result();
        watcher->deleteLater();
        if (generation != m_importGeneration || pending.cancelled->load()
            || !m_pendingImports.contains(pending.mediaId)) return;
        m_activeImports.remove(pending.mediaId);
        // A locked document retains the durable intent and retries after unlock.
        if (m_editsLocked || m_mediaResidencySuspended) return;
        const QString error = sourceSignature(pending.sourcePath) != pending.sourceSignature
            ? QStringLiteral("The source file changed or disappeared during import.")
            : probe.error;
        if (!error.isEmpty() || !probe.accepted()) {
            m_pendingImports.remove(pending.mediaId);
            emit pendingImportsChanged();
            emit documentChanged();
            emit mediaImportFailed(pending.mediaId, pending.sourcePath,
                                   error.isEmpty() ? QStringLiteral("Unsupported media.") : error);
            return;
        }
        auto* media = new CanvasMedia(probe.video ? CanvasMedia::Type::Video
                                                 : CanvasMedia::Type::Image,
                                      probe.displaySize);
        media->restoreMediaId(pending.mediaId);
        media->restoreSourceDurationMs((probe.durationUs + 999) / 1000);
        media->setSourcePath(pending.sourcePath);
        media->setPosition(pending.center - QPointF(probe.displaySize.width() / 2.0,
                                                    probe.displaySize.height() / 2.0));
        media->setZ(nextZ());
        if (probe.video) media->initializeVideoRuntime();
        // Keep the pending gate until adoption so no observer can launch an
        // incomplete scene between removing the intent and creating its node.
        adoptMedia(media);
        select(media->mediaId());
        m_pendingImports.remove(pending.mediaId);
        emit pendingImportsChanged();
        emit documentChanged();
    });
    watcher->setFuture(QtConcurrent::run(importMetadataPool(), [pending] {
        MediaDecoder::Geometry probe;
        if (pending.cancelled->load()) return probe;
        if (sourceSignature(pending.sourcePath) != pending.sourceSignature) {
            probe.error = QStringLiteral("The source file changed or disappeared before import.");
            return probe;
        }
        return MediaDecoder::inspectGeometry(pending.sourcePath,
            [cancelled = pending.cancelled] { return cancelled->load(); });
    }));
}

CanvasMedia* CanvasDocument::mediaById(const QString& mediaId) const
{
    for (CanvasMedia* item : m_media) {
        if (item && item->mediaId() == mediaId) return item;
    }
    return nullptr;
}

void CanvasDocument::adoptMedia(CanvasMedia* media)
{
    if (!media) return;
    media->setParent(this);
    media->setResidencySuspended(m_mediaResidencySuspended);
    m_media.append(media);
    connect(media, &CanvasMedia::changed, this, [this, media]() {
        media->setClipActive(SceneTimeline::activeClip(media->timelineTrack(), m_timelineSettings.slotAt(m_timelinePositionMs)) != nullptr);
        emit mediaChanged(media->mediaId());
        emit documentChanged();
    });
    connect(media, &CanvasMedia::presentationChanged, this, [this, media]() {
        if (!m_evaluatingTimeline) emit mediaChanged(media->mediaId());
    });
    connect(media, &CanvasMedia::residencyChanged, this, [this, media]() {
        media->ensureDefaultClip(m_timelineSettings);
        emit mediaChanged(media->mediaId());
    });
    connect(media, &CanvasMedia::runtimeStateChanged, this, [this, media]() {
        media->ensureDefaultClip(m_timelineSettings);
    });
    connect(media, &CanvasMedia::identityReady, this, [this, media](const QString& fileId) {
        if (m_fileManager) {
            m_fileManager->registerVerifiedLocalFile(fileId, media->sourcePath());
            m_fileManager->associateMediaWithFile(media->mediaId(), fileId);
            if (!m_projectId.isEmpty())
                m_fileManager->associateFileWithProject(fileId, m_projectId);
        }
        emit documentChanged();
    });
    connect(media, &CanvasMedia::sourceInvalidated, this, [this, media](const QString& reason) {
        const QString id = media->mediaId();
        emit mediaSourceInvalidated(id, reason);
        QMetaObject::invokeMethod(this, [this, id] { removeMedia(id); }, Qt::QueuedConnection);
    });
    if (m_fileManager && media->residencyReady() && !media->fileId().isEmpty()) {
        m_fileManager->registerVerifiedLocalFile(media->fileId(), media->sourcePath());
        m_fileManager->associateMediaWithFile(media->mediaId(), media->fileId());
        if (!m_projectId.isEmpty())
            m_fileManager->associateFileWithProject(media->fileId(), m_projectId);
    }
    media->ensureDefaultClip(m_timelineSettings);
    media->setClipActive(SceneTimeline::activeClip(media->timelineTrack(), m_timelineSettings.slotAt(m_timelinePositionMs)) != nullptr);
    emit mediaAdded(media);
    emit documentChanged();
}

CanvasMedia* CanvasDocument::addText(const QPointF& position,
                                     const QString& text,
                                     qreal initialSceneHeight)
{
    if (m_editsLocked || !std::isfinite(initialSceneHeight)
        || initialSceneHeight < 0.0) return nullptr;
    auto* media = new CanvasMedia(CanvasMedia::Type::Text, QSize(400, 200));
    media->setText(text.isEmpty() ? QStringLiteral("Text") : text);
    media->fitTextToContent();
    if (initialSceneHeight > 0.0) {
        media->setScale(initialSceneHeight / media->baseSize().height());
        // Refuse a height outside the media transform's representable range;
        // never publish an element at an unrelated default scale instead.
        if (!qFuzzyCompare(media->sceneRect().height(), initialSceneHeight)) {
            delete media;
            return nullptr;
        }
    }
    media->setPosition(position - QPointF(media->sceneRect().width() * 0.5,
                                          media->sceneRect().height() * 0.5));
    media->setZ(nextZ());
    adoptMedia(media);
    select(media->mediaId());
    return media;
}

CanvasMedia* CanvasDocument::addPreparedFile(
    const QString& sourcePath, const QSize& nativeSize, bool video,
    const QPointF& position, qint64 sourceDurationMs)
{
    if (m_editsLocked || sourcePath.isEmpty()) return nullptr;
    auto* media = new CanvasMedia(video ? CanvasMedia::Type::Video
                                        : CanvasMedia::Type::Image,
                                  nativeSize.expandedTo(QSize(1, 1)));
    media->setResidencySuspended(m_mediaResidencySuspended);
    media->restoreSourceDurationMs(sourceDurationMs);
    media->setSourcePath(sourcePath);
    media->setPosition(position);
    media->setZ(nextZ());
    if (video) media->initializeVideoRuntime();
    adoptMedia(media);
    select(media->mediaId());
    return media;
}

bool CanvasDocument::removeMedia(const QString& mediaId)
{
    if (m_editsLocked) return false;
    if (auto found = m_pendingImports.find(mediaId); found != m_pendingImports.end()) {
        if (found->cancelled) found->cancelled->store(true);
        m_pendingImports.erase(found);
        m_activeImports.remove(mediaId);
        emit pendingImportsChanged();
        emit documentChanged();
        return true;
    }
    for (qsizetype i = 0; i < m_media.size(); ++i) {
        CanvasMedia* media = m_media.at(i);
        if (!media || media->mediaId() != mediaId) continue;
        const bool selected = media->selected();
        const bool primary = mediaId == m_primarySelectedMediaId;
        emit mediaAboutToBeRemoved(media);
        m_media.removeAt(i);
        m_selectionActivationOrder.removeAll(mediaId);
        if (primary) {
            evaluateTimeline();
            m_primarySelectedMediaId = m_selectionActivationOrder.isEmpty() ? QString() : m_selectionActivationOrder.last();
            emit primarySelectedMediaChanged();
        }
        if (m_fileManager && !media->isText()) {
            m_fileManager->removeMediaAssociation(mediaId);
        }
        media->retireResidency();
        media->deleteLater();
        emit mediaRemoved(mediaId);
        if (selected) emit selectionChanged();
        emit documentChanged();
        return true;
    }
    return false;
}

void CanvasDocument::clear()
{
    cancelPendingImportTasks();
    const bool hadPendingImports = hasPendingImports();
    m_pendingImports.clear();
    const QList<CanvasMedia*> previous = m_media;
    m_media.clear();
    m_primarySelectedMediaId.clear(); m_selectionActivationOrder.clear();
    emit primarySelectedMediaChanged();
    for (CanvasMedia* media : previous) {
        if (!media) continue;
        emit mediaAboutToBeRemoved(media);
        emit mediaRemoved(media->mediaId());
        if (m_fileManager && !media->isText()) {
            m_fileManager->removeMediaAssociation(media->mediaId());
        }
        media->retireResidency();
        media->deleteLater();
    }
    if (hadPendingImports) emit pendingImportsChanged();
    emit selectionChanged();
    emit documentChanged();
}

void CanvasDocument::moveForward(const QString& mediaId)
{
    if (m_editsLocked) return;
    CanvasMedia* target = mediaById(mediaId);
    if (!target) return;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (CanvasMedia* item : m_media) {
        if (item != target && item->z() > target->z()) {
            nearest = std::min(nearest, item->z());
        }
    }
    target->setZ(nearest == std::numeric_limits<qreal>::max()
                     ? nextZ() : nearest + 0.5);
}

void CanvasDocument::moveBackward(const QString& mediaId)
{
    if (m_editsLocked) return;
    CanvasMedia* target = mediaById(mediaId);
    if (!target) return;
    qreal nearest = -std::numeric_limits<qreal>::max();
    for (CanvasMedia* item : m_media) {
        if (item != target && item->z() < target->z()) {
            nearest = std::max(nearest, item->z());
        }
    }
    target->setZ(nearest == -std::numeric_limits<qreal>::max()
                     ? 1.0 : std::max<qreal>(1.0, nearest - 0.5));
}

QStringList CanvasDocument::selectedMediaIds() const
{
    QStringList ids;
    for (CanvasMedia* item : m_media) {
        if (item && item->selected()) ids.append(item->mediaId());
    }
    return ids;
}

CanvasMedia* CanvasDocument::selectedMedia() const
{
    return primarySelectedMedia();
}
CanvasMedia* CanvasDocument::primarySelectedMedia() const
{
    CanvasMedia* media=mediaById(m_primarySelectedMediaId);
    return media && media->selected() ? media : nullptr;
}
bool CanvasDocument::setPrimarySelectedMedia(const QString& id)
{
    if (m_editsLocked) return false;
    CanvasMedia* media=mediaById(id);
    if (!media || !media->selected()) return false;
    if (id == m_primarySelectedMediaId) return true;
    evaluateTimeline();
    m_primarySelectedMediaId=id;
    m_selectionActivationOrder.removeAll(id); m_selectionActivationOrder.append(id);
    emit primarySelectedMediaChanged(); emit selectionChanged();
    return true;
}
void CanvasDocument::select(const QString& mediaId, bool additive)
{
    if (m_editsLocked) return;
    CanvasMedia* target=mediaById(mediaId);
    if (!target) return;
    // A selected secondary is promoted without destroying its group.
    if (target->selected()) { setPrimarySelectedMedia(mediaId); return; }
    evaluateTimeline();
    if (!additive) m_selectionActivationOrder.clear();
    for (CanvasMedia* item : m_media) item->setSelected(item==target || (additive && item->selected()));
    m_primarySelectedMediaId=mediaId;
    m_selectionActivationOrder.removeAll(mediaId); m_selectionActivationOrder.append(mediaId);
    emit primarySelectedMediaChanged(); emit selectionChanged();
}
void CanvasDocument::clearSelection()
{
    const bool hadSelection=!m_primarySelectedMediaId.isEmpty();
    evaluateTimeline();
    for (CanvasMedia* item : m_media) if (item) item->setSelected(false);
    m_primarySelectedMediaId.clear(); m_selectionActivationOrder.clear();
    if (hadSelection) { emit primarySelectedMediaChanged(); emit selectionChanged(); }
}
bool CanvasDocument::setTimelineSettings(const SceneTimeline::SceneSettings& settings)
{
    SceneTimeline::SceneSettings validated;
    if (m_editsLocked || !SceneTimeline::SceneSettings::fromJson(settings.toJson(),&validated)) return false;
    if (validated.toJson()==m_timelineSettings.toJson()) return true;
    for (CanvasMedia* item:m_media) {
        const auto& t=item->timelineTrack();
        if (validated.slotsPerSecond != m_timelineSettings.slotsPerSecond
            && (!t.keyframes.isEmpty() || t.clipsInitialized)) return false;
        for(const auto& k:t.keyframes) if(k.slot>validated.maxSlot()) return false;
        for(const auto& c:t.clips) if(c.endSlot()>validated.maxSlot()) return false;
    }
    m_timelineSettings=validated;
    m_timelinePositionMs=qMin(m_timelinePositionMs,validated.timeMs(validated.maxSlot()));
    evaluateTimeline(); emit timelineChanged(); emit timelinePositionChanged(); emit documentChanged();
    return true;
}
void CanvasDocument::setTimelinePosition(qreal timeMs)
{
    if (!std::isfinite(timeMs)) return;
    const qreal next=qBound<qreal>(0,timeMs,m_timelineSettings.timeMs(m_timelineSettings.maxSlot()));
    const bool changed=next!=m_timelinePositionMs;
    m_timelinePositionMs=next; evaluateTimeline();
    if(changed) emit timelinePositionChanged();
}
void CanvasDocument::evaluateTimeline()
{
    if (m_evaluatingTimeline) return;
    m_evaluatingTimeline = true;
    for(CanvasMedia* media:m_media) {
        media->setClipActive(SceneTimeline::activeClip(media->timelineTrack(), m_timelineSettings.slotAt(m_timelinePositionMs)) != nullptr);
        if(media->timelineTrack().keyframes.isEmpty()) media->clearEvaluatedElementState();
        else media->setEvaluatedElementState(SceneTimeline::evaluate(media->authorElementState(),media->timelineTrack(),m_timelineSettings.slotAt(m_timelinePositionMs)));
    }
    m_evaluatingTimeline = false;
    emit timelineEvaluated();
}

void CanvasDocument::setScreens(const QList<ScreenInfo>& screens)
{
    if (m_screens == screens) return;
    m_screens = screens;
    rebuildScreenRects();
    if (m_remoteCursorScreenId >= 0)
        updateRemoteCursor(m_remoteCursorScreenId, m_remoteCursorScreenPosition);
    emit screensChanged();
    emit documentChanged();
}

void CanvasDocument::rebuildScreenRects()
{
    m_screenRects.clear();
    if (m_screens.isEmpty()) return;
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const ScreenInfo& screen : m_screens) {
        minX = std::min(minX, screen.x);
        minY = std::min(minY, screen.y);
    }
    for (const ScreenInfo& screen : m_screens) {
        m_screenRects.insert(screen.id,
            QRectF(screen.x - minX, screen.y - minY,
                   screen.width, screen.height));
    }
}

void CanvasDocument::setCamera(qreal scale, qreal panX, qreal panY)
{
    if (!std::isfinite(scale) || scale <= 0.0
        || !std::isfinite(panX) || !std::isfinite(panY)) return;
    if (m_hasCamera && !m_hasNormalizedCamera
        && qFuzzyCompare(m_cameraScale, scale)
        && qFuzzyCompare(1.0 + m_cameraPanX, 1.0 + panX)
        && qFuzzyCompare(1.0 + m_cameraPanY, 1.0 + panY)) return;
    m_hasCamera = true;
    m_hasNormalizedCamera = false;
    setCameraProjection(scale, panX, panY);
    emit cameraChanged();
}

void CanvasDocument::setCameraProjection(qreal scale, qreal panX, qreal panY)
{
    if (!std::isfinite(scale) || scale <= 0.0
        || !std::isfinite(panX) || !std::isfinite(panY)) return;
    m_cameraScale = scale;
    m_cameraPanX = panX;
    m_cameraPanY = panY;
}

void CanvasDocument::setCameraView(const QPointF& center, qreal squareSceneSize)
{
    if (!std::isfinite(center.x()) || !std::isfinite(center.y())
        || !std::isfinite(squareSceneSize) || squareSceneSize <= 0.0) return;
    if (m_hasNormalizedCamera && m_cameraCenter == center
        && qFuzzyCompare(m_cameraSquareSceneSize, squareSceneSize)) return;
    m_hasCamera = true;
    m_hasNormalizedCamera = true;
    m_cameraCenter = center;
    m_cameraSquareSceneSize = squareSceneSize;
    emit cameraChanged();
}

void CanvasDocument::resetCamera()
{
    setCameraView({}, 1000.0);
}

void CanvasDocument::setRemoteCursor(bool visible,
                                     const QPointF& scenePosition)
{
    if (m_remoteCursorVisible == visible
        && m_remoteCursorPosition == scenePosition) return;
    m_remoteCursorVisible = visible;
    m_remoteCursorPosition = scenePosition;
    emit remoteCursorChanged();
}

void CanvasDocument::updateRemoteCursor(int screenId, const QPointF& screenPosition)
{
    // Retain the sample if it precedes the screen snapshot. Screen changes
    // reproject it; cursor state never enters the saved project or autosave.
    m_remoteCursorScreenId = screenId;
    m_remoteCursorScreenPosition = screenPosition;
    QPointF mapped;
    const bool visible = mapRemoteCursor(screenId, screenPosition, &mapped);
    setRemoteCursor(visible, visible ? mapped : m_remoteCursorPosition);
}

void CanvasDocument::hideRemoteCursor()
{
    m_remoteCursorScreenId = -1;
    setRemoteCursor(false, m_remoteCursorPosition);
}

bool CanvasDocument::mapRemoteCursor(int screenId, const QPointF& screenPosition,
                                     QPointF* scenePosition) const
{
    const QRectF screen = m_screenRects.value(screenId);
    if (screen.isEmpty() || !std::isfinite(screenPosition.x())
        || !std::isfinite(screenPosition.y())
        || screenPosition.x() < 0 || screenPosition.y() < 0
        || screenPosition.x() >= screen.width()
        || screenPosition.y() >= screen.height()) return false;
    // Screen-local pixels avoid ambiguous global rectangles with mixed DPI
    // displays, and preserve exact positions at adjoining screen edges.
    if (scenePosition) *scenePosition = screen.topLeft() + screenPosition;
    return true;
}

void CanvasDocument::setEditsLocked(bool locked)
{
    if (m_editsLocked == locked) return;
    m_editsLocked = locked;
    emit editsLockedChanged();
    if (!locked)
        for (const QString& id : m_pendingImports.keys()) startPendingImport(id);
}

void CanvasDocument::setContentAvailable(bool available)
{
    if (m_contentAvailable == available) return;
    m_contentAvailable = available;
    emit contentAvailabilityChanged();
}

void CanvasDocument::setMediaResidencySuspended(bool suspended)
{
    if (m_mediaResidencySuspended == suspended) return;
    m_mediaResidencySuspended = suspended;
    if (suspended) cancelPendingImportTasks();
    for (CanvasMedia* media : std::as_const(m_media))
        if (media) media->setResidencySuspended(suspended);
    if (!suspended)
        for (const QString& id : m_pendingImports.keys()) startPendingImport(id);
    emit mediaResidencySuspendedChanged();
}

QJsonObject CanvasDocument::serializeSceneState() const
{
    QJsonObject root{{QStringLiteral("renderSchemaVersion"), SceneTimeline::RenderSchemaVersion},
                     {QStringLiteral("timeline"), m_timelineSettings.toJson()}};
    QJsonArray screens;
    for (const ScreenInfo& screen : m_screens) screens.append(screen.toJson());
    root.insert(QStringLiteral("screens"), screens);
    QJsonArray serializedMedia;
    QList<CanvasMedia*> ordered=m_media;
    std::sort(ordered.begin(),ordered.end(),[](CanvasMedia* a,CanvasMedia* b){return a->authorElementState().z<b->authorElementState().z;});
    for(CanvasMedia* media:ordered) {
        const auto author=media->authorElementState();
        QJsonObject item=author.toJson();
        item.insert(QStringLiteral("mediaId"),media->mediaId());
        item.insert(QStringLiteral("fileId"),media->fileId());
        item.insert(QStringLiteral("fileName"),QFileInfo(media->sourcePath()).fileName());
        item.insert(QStringLiteral("timeline"),media->timelineTrack().toJson());
        if(media->isVideo()) item.insert(QStringLiteral("durationMs"),double(media->sourceDurationMs()));
        QJsonArray spans;
        const QRectF bounds(author.position,author.size);
        for(auto it=m_screenRects.cbegin();it!=m_screenRects.cend();++it) {
            const auto span=spanForIntersection(it.key(),it.value(),bounds);
            if(!span.isEmpty()) spans.append(span);
        }
        item.insert(QStringLiteral("spans"),spans); serializedMedia.append(item);
    }
    root.insert(QStringLiteral("media"),serializedMedia); return root;
}

QJsonObject CanvasDocument::serializeProjectState() const
{
    QJsonObject root = serializeSceneState();
    // Screen topology has an explicit ProjectRecord field. Keeping a second
    // copy in the document made session-only discovery leak into persistence.
    root.remove(QStringLiteral("screens"));
    if (!m_pendingImports.isEmpty()) {
        QJsonArray pendingImports;
        QStringList ids = m_pendingImports.keys();
        ids.sort();
        for (const QString& id : ids) {
            const PendingImport& pending = m_pendingImports[id];
            pendingImports.append(QJsonObject{
                {QStringLiteral("mediaId"), pending.mediaId},
                {QStringLiteral("sourcePath"), pending.sourcePath},
                {QStringLiteral("sourceSignature"), pending.sourceSignature},
                {QStringLiteral("centerX"), pending.center.x()},
                {QStringLiteral("centerY"), pending.center.y()}
            });
        }
        root.insert(QStringLiteral("pendingImports"), pendingImports);
    }
    QJsonObject viewport{
        {QStringLiteral("m11"), m_cameraScale},
        {QStringLiteral("m12"), 0.0},
        {QStringLiteral("m21"), 0.0},
        {QStringLiteral("m22"), m_cameraScale},
        {QStringLiteral("dx"), m_cameraPanX},
        {QStringLiteral("dy"), m_cameraPanY},
        {QStringLiteral("centerX"), 0.0},
        {QStringLiteral("centerY"), 0.0}
    };
    if (m_hasNormalizedCamera) {
        viewport.insert(QStringLiteral("cameraVersion"), 2);
        viewport.insert(QStringLiteral("centerX"), m_cameraCenter.x());
        viewport.insert(QStringLiteral("centerY"), m_cameraCenter.y());
        viewport.insert(QStringLiteral("squareSceneSize"), m_cameraSquareSceneSize);
    }
    // A new project can be saved before its screens or viewport are available.
    // Such a project still needs its first fit when it is opened.
    if (m_hasCamera) root.insert(QStringLiteral("viewport"), viewport);
    return root;
}

bool CanvasDocument::restoreProjectState(
    const QJsonObject& state,
    const QHash<QString, QString>& sourcePathByMediaId,
    QStringList* skippedMediaIds)
{
    if (!m_media.isEmpty() || hasPendingImports()
        || state.value(QStringLiteral("renderSchemaVersion")).toInt(-1) != SceneTimeline::RenderSchemaVersion) {
        return false;
    }
    SceneTimeline::SceneSettings settings;
    if (!SceneTimeline::SceneSettings::fromJson(state.value(QStringLiteral("timeline")).toObject(),&settings)) return false;
    m_timelineSettings=settings; m_timelinePositionMs=0;
    // Install the saved camera before publishing topology: screensChanged can
    // trigger the initial fit when a controller already has a viewport.
    const QJsonObject viewport = state.value(QStringLiteral("viewport")).toObject();
    const qreal invalid = std::numeric_limits<qreal>::quiet_NaN();
    const QPointF center(viewport.value(QStringLiteral("centerX")).toDouble(invalid),
                         viewport.value(QStringLiteral("centerY")).toDouble(invalid));
    const qreal squareSize = viewport.value(QStringLiteral("squareSceneSize")).toDouble(invalid);
    if (viewport.value(QStringLiteral("cameraVersion")).toInt() == 2
        && std::isfinite(center.x()) && std::isfinite(center.y())
        && std::isfinite(squareSize) && squareSize > 0.0) {
        setCameraProjection(viewport.value(QStringLiteral("m11")).toDouble(1.0),
                            viewport.value(QStringLiteral("dx")).toDouble(),
                            viewport.value(QStringLiteral("dy")).toDouble());
        setCameraView(center, squareSize);
    } else if (!viewport.isEmpty()) {
        setCamera(viewport.value(QStringLiteral("m11")).toDouble(1.0),
                  viewport.value(QStringLiteral("dx")).toDouble(),
                  viewport.value(QStringLiteral("dy")).toDouble());
    }
    QList<ScreenInfo> restoredScreens;
    for (const QJsonValue& value : state.value(QStringLiteral("screens")).toArray()) {
        if (value.isObject()) restoredScreens.append(ScreenInfo::fromJson(value.toObject()));
    }
    if (!restoredScreens.isEmpty()) setScreens(restoredScreens);

    insertProjectMedia(state, sourcePathByMediaId, skippedMediaIds, false);
    clearSelection();
    for (const QJsonValue& value : state.value(QStringLiteral("pendingImports")).toArray()) {
        const QJsonObject item = value.toObject();
        PendingImport pending;
        pending.mediaId = item.value(QStringLiteral("mediaId")).toString();
        pending.sourcePath = item.value(QStringLiteral("sourcePath")).toString();
        pending.sourceSignature = item.value(QStringLiteral("sourceSignature")).toString();
        pending.center = {item.value(QStringLiteral("centerX")).toDouble(invalid),
                          item.value(QStringLiteral("centerY")).toDouble(invalid)};
        // A synchronous snapshot during adoption can contain both forms of
        // the same import. The concrete media is already authoritative.
        if (mediaById(pending.mediaId) || m_pendingImports.contains(pending.mediaId)) continue;
        if (pending.mediaId.isEmpty() || !std::isfinite(pending.center.x())
            || !std::isfinite(pending.center.y()) || pending.sourceSignature.isEmpty()
            || !QFileInfo(pending.sourcePath).isAbsolute()
            || sourceSignature(pending.sourcePath) != pending.sourceSignature) {
            if (skippedMediaIds && !pending.mediaId.isEmpty()
                && !skippedMediaIds->contains(pending.mediaId))
                skippedMediaIds->append(pending.mediaId);
            continue;
        }
        m_pendingImports.insert(pending.mediaId, pending);
    }
    if (hasPendingImports()) {
        emit pendingImportsChanged();
        emit documentChanged();
        for (const QString& id : m_pendingImports.keys()) startPendingImport(id);
    }
    return true;
}

QStringList CanvasDocument::pasteMediaState(
    const QJsonObject& state, const QHash<QString, QString>& sourcePaths,
    QStringList* skippedMediaIds)
{
    if (m_editsLocked || state.value(QStringLiteral("renderSchemaVersion")).toInt(-1) != SceneTimeline::RenderSchemaVersion)
        return {};
    SceneTimeline::SceneSettings sourceSettings;
    if (!SceneTimeline::SceneSettings::fromJson(state.value(QStringLiteral("timeline")).toObject(), &sourceSettings)
        || sourceSettings.slotsPerSecond != m_timelineSettings.slotsPerSecond) {
        if (skippedMediaIds) for (const auto& item : state.value(QStringLiteral("media")).toArray())
            skippedMediaIds->append(item.toObject().value(QStringLiteral("mediaId")).toString());
        return {};
    }
    QHash<QString,QString> insertedIds;
    const QStringList inserted = insertProjectMedia(state, sourcePaths, skippedMediaIds, true, &insertedIds);
    if (!inserted.isEmpty()) {
        clearSelection();
        for (const QString& id : inserted) select(id, true);
        const QString copiedPrimary=insertedIds.value(state.value(QStringLiteral("primaryMediaId")).toString());
        setPrimarySelectedMedia(copiedPrimary.isEmpty()?inserted.first():copiedPrimary);
    }
    return inserted;
}

QStringList CanvasDocument::insertProjectMedia(
    const QJsonObject& state, const QHash<QString, QString>& sourcePathByMediaId,
    QStringList* skippedMediaIds, bool freshIds, QHash<QString,QString>* insertedIds)
{
    QStringList inserted;
    for(const QJsonValue& value:state.value(QStringLiteral("media")).toArray()) {
        const auto source=value.toObject();
        const QString id=source.value(QStringLiteral("mediaId")).toString();
        const auto skip=[&]{if(skippedMediaIds && !id.isEmpty() && !skippedMediaIds->contains(id)) skippedMediaIds->append(id);};
        SceneTimeline::ElementState element; SceneTimeline::MediaTrack track;
        if(id.isEmpty() || (!freshIds && mediaById(id))
            || !SceneTimeline::ElementState::fromMediaJson(source,&element)
            || !SceneTimeline::MediaTrack::fromJson(source.value(QStringLiteral("timeline")).toObject(),&track,m_timelineSettings.maxSlot())) {skip();continue;}
        bool compatible=true;
        for(const auto& key:track.keyframes) if(key.state.type!=element.type) compatible=false;
        const bool text=element.type==QLatin1String("text"),video=element.type==QLatin1String("video");
        qint64 sourceDuration=0;
        if(video) {
            const auto value=source.value(QStringLiteral("durationMs"));
            const double duration=value.toDouble(-1);
            if(!value.isDouble() || !std::isfinite(duration) || duration<0
                || duration>SceneTimeline::MaximumSupportedDurationMs || std::floor(duration)!=duration) compatible=false;
            else sourceDuration=static_cast<qint64>(duration);
        }
        if(!compatible || !SceneTimeline::validateMediaTrack(track, element.type, sourceDuration)){skip();continue;}
        const QString path=sourcePathByMediaId.value(id);
        if(!text && (path.isEmpty() || !QFileInfo::exists(path))){skip();continue;}
        auto* media=new CanvasMedia(text?CanvasMedia::Type::Text:(video?CanvasMedia::Type::Video:CanvasMedia::Type::Image),element.baseSize.toSize());
        if(!freshIds) media->restoreMediaId(id);
        else {
            for(auto& key:track.keyframes) key.id=SceneTimeline::newId();
            for(auto& clip:track.clips) clip.id=SceneTimeline::newId();
        }
        media->setElementState(element); media->setTimelineTrack(track);
        if(video) media->restoreSourceDurationMs(sourceDuration);
        if(!text) {
            media->setResidencySuspended(m_mediaResidencySuspended);
            media->setSourcePath(path,source.value(QStringLiteral("fileId")).toString());
            if(video) media->initializeVideoRuntime();
        }
        media->setUploadNotUploaded(); adoptMedia(media); inserted.append(media->mediaId());
        if(insertedIds) insertedIds->insert(id,media->mediaId());
    }
    evaluateTimeline(); return inserted;
}

qreal CanvasDocument::nextZ() const
{
    qreal z = 1.0;
    for (CanvasMedia* media : m_media) {
        if (media) z = std::max(z, media->z() + 1.0);
    }
    return z;
}
