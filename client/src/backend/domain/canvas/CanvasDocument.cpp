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
#include <QJsonDocument>
#include <QSignalBlocker>
#include <QPointer>
#include <QThreadPool>
#include <QUuid>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

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
    for (auto& pending : m_pendingImports) {
        if (pending.cancelled) pending.cancelled->store(true);
        if (pending.candidate) {
            auto* candidate = pending.candidate.data();
            pending.candidate.clear();
            candidate->retireResidency();
            candidate->deleteLater();
        }
    }
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
    const qint64 startSlot = m_timelineSettings.slotAt(m_timelinePositionMs);
    if (startSlot >= m_timelineSettings.maxSlot()) return {};
    if (m_editsLocked || m_media.size() + m_pendingImports.size() >= SceneTimeline::MaximumMediaCount || !std::isfinite(center.x()) || !std::isfinite(center.y())) return {};
    const QFileInfo info(sourcePath);
    const QString signature = sourceSignature(sourcePath);
    if (signature.isEmpty()) return {};
    PendingImport pending;
    pending.mediaId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending.sourcePath = info.canonicalFilePath();
    pending.sourceSignature = signature;
    pending.center = center;
    pending.startSlot = startSlot;
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
    if (stored.candidate) { finishPendingImport(mediaId); return; }
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
        if (probe.video) media->initializeVideoRuntime();
        media->setParent(this);
        m_pendingImports[pending.mediaId].candidate = media;
        connect(media, &CanvasMedia::runtimeStateChanged, this, [this, id=pending.mediaId] { finishPendingImport(id); });
        connect(media, &CanvasMedia::residencyChanged, this, [this, id=pending.mediaId] { finishPendingImport(id); });
        finishPendingImport(pending.mediaId);
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

void CanvasDocument::finishPendingImport(const QString& mediaId)
{
    auto found = m_pendingImports.find(mediaId);
    if (found == m_pendingImports.end() || !found->candidate || m_editsLocked || m_mediaResidencySuspended) return;
    auto* media = found->candidate.data();
    const bool unknownDuration = media->isVideo() && media->sourceDurationMs() <= 0;
    const bool sourceChanged = sourceSignature(found->sourcePath) != found->sourceSignature;
    if (!sourceChanged && unknownDuration && !media->residencyReady() && media->residencyState() != "error") return;
    if (sourceChanged || unknownDuration || media->sourceDurationMs() > SceneTimeline::MaximumSupportedDurationMs
        || m_media.size() >= SceneTimeline::MaximumMediaCount) {
        const QString path = found->sourcePath;
        m_pendingImports.erase(found);
        media->retireResidency(); media->deleteLater();
        emit pendingImportsChanged(); emit documentChanged();
        emit mediaImportFailed(mediaId, path, sourceChanged
            ? QStringLiteral("The source file changed or disappeared during import.")
            : QStringLiteral("The video duration is unavailable or the scene instance limit was reached."));
        return;
    }
    media->ensureDefaultClip(m_timelineSettings, found->startSlot);
    // Clear the durable gate before publication so callbacks cannot adopt twice.
    found->candidate.clear();
    m_pendingImports.remove(mediaId);
    QString error;
    if (!insertMediaAbove(media, &error)) {
        const QString path = media->sourcePath();
        media->retireResidency(); media->deleteLater();
        emit mediaImportFailed(mediaId, path, error);
    }
    emit pendingImportsChanged(); emit documentChanged();
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
        if (!m_media.contains(media)) return;
        media->setClipActive(SceneTimeline::activeClip(media->timelineTrack(), m_timelineSettings.slotAt(m_timelinePositionMs)) != nullptr);
        if (m_publishingTimelineEdit) return;
        emit mediaChanged(media->mediaId());
        emit documentChanged();
    });
    connect(media, &CanvasMedia::presentationChanged, this, [this, media]() {
        if (m_media.contains(media) && !m_evaluatingTimeline && !m_publishingTimelineEdit) emit mediaChanged(media->mediaId());
    });
    connect(media, &CanvasMedia::residencyChanged, this, [this, media]() {
        if (!m_media.contains(media)) return;
        media->ensureDefaultClip(m_timelineSettings, m_timelineSettings.slotAt(m_timelinePositionMs));
        if (!m_publishingTimelineEdit) emit mediaChanged(media->mediaId());
    });
    connect(media, &CanvasMedia::runtimeStateChanged, this, [this, media]() {
        media->ensureDefaultClip(m_timelineSettings, m_timelineSettings.slotAt(m_timelinePositionMs));
    });
    connect(media, &CanvasMedia::identityReady, this, [this, media](const QString& fileId) {
        if (!m_media.contains(media)) return;
        if (m_fileManager) {
            m_fileManager->registerVerifiedLocalFile(fileId, media->sourcePath());
            m_fileManager->associateMediaWithFile(media->mediaId(), fileId);
            if (!m_projectId.isEmpty())
                m_fileManager->associateFileWithProject(fileId, m_projectId);
        }
        emit documentChanged();
    });
    connect(media, &CanvasMedia::sourceInvalidated, this, [this, media](const QString& reason) {
        if (!m_media.contains(media)) return;
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
    media->ensureDefaultClip(m_timelineSettings, m_timelineSettings.slotAt(m_timelinePositionMs));
    media->setClipActive(SceneTimeline::activeClip(media->timelineTrack(), m_timelineSettings.slotAt(m_timelinePositionMs)) != nullptr);
    if (!m_publishingTimelineEdit) {
        emit mediaAdded(media);
        emit documentChanged();
    }
}

CanvasMedia* CanvasDocument::addText(const QPointF& position,
                                     const QString& text,
                                     qreal initialSceneHeight)
{
    const qint64 startSlot = m_timelineSettings.slotAt(m_timelinePositionMs);
    if (startSlot >= m_timelineSettings.maxSlot()) return nullptr;
    if (m_editsLocked || m_media.size() + m_pendingImports.size() >= SceneTimeline::MaximumMediaCount || !std::isfinite(initialSceneHeight)
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
    media->ensureDefaultClip(m_timelineSettings, startSlot);
    if (!insertMediaAbove(media)) { delete media; return nullptr; }
    return media;
}

CanvasMedia* CanvasDocument::addPreparedFile(
    const QString& sourcePath, const QSize& nativeSize, bool video,
    const QPointF& position, qint64 sourceDurationMs)
{
    const qint64 startSlot = m_timelineSettings.slotAt(m_timelinePositionMs);
    if (startSlot >= m_timelineSettings.maxSlot()) return nullptr;
    if (m_editsLocked || m_media.size() + m_pendingImports.size() >= SceneTimeline::MaximumMediaCount || sourcePath.isEmpty()) return nullptr;
    if (video && sourceDurationMs <= 0) {
        const auto metadata = MediaDecoder::inspectGeometry(sourcePath);
        sourceDurationMs = (metadata.durationUs + 999) / 1000;
        if (!metadata.accepted() || sourceDurationMs <= 0) return nullptr;
    }
    if (video && sourceDurationMs > SceneTimeline::MaximumSupportedDurationMs) return nullptr;
    auto* media = new CanvasMedia(video ? CanvasMedia::Type::Video
                                        : CanvasMedia::Type::Image,
                                  nativeSize.expandedTo(QSize(1, 1)));
    media->setResidencySuspended(m_mediaResidencySuspended);
    media->restoreSourceDurationMs(sourceDurationMs);
    media->setSourcePath(sourcePath);
    media->setPosition(position);
    if (video) media->initializeVideoRuntime();
    media->ensureDefaultClip(m_timelineSettings, startSlot);
    if (!insertMediaAbove(media)) { delete media; return nullptr; }
    return media;
}

bool CanvasDocument::removeMedia(const QString& mediaId)
{
    if (m_editsLocked) return false;
    if (auto found = m_pendingImports.find(mediaId); found != m_pendingImports.end()) {
        if (found->cancelled) found->cancelled->store(true);
        auto candidate = found->candidate;
        m_pendingImports.erase(found);
        if (candidate) { candidate->retireResidency(); candidate->deleteLater(); }
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
    for (const auto& pending : m_pendingImports) {
        if (validated.slotsPerSecond != m_timelineSettings.slotsPerSecond
            || pending.startSlot >= validated.maxSlot()) return false;
    }
    for (CanvasMedia* item:m_media) {
        const auto& t=item->timelineTrack();
        if (validated.slotsPerSecond != m_timelineSettings.slotsPerSecond
            && (!t.keyframes.isEmpty() || !t.clip.id.isEmpty())) return false;
        for(const auto& k:t.keyframes) if(k.slot>validated.maxSlot()) return false;
        if(t.clip.endSlot()>validated.maxSlot()) return false;
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
    if (!m_publishingTimelineEdit) emit timelineEvaluated();
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
    std::sort(ordered.begin(),ordered.end(),[](CanvasMedia* a,CanvasMedia* b){
        if (a->timelineTrack().trackIndex != b->timelineTrack().trackIndex)
            return a->timelineTrack().trackIndex > b->timelineTrack().trackIndex;
        if (a->timelineTrack().clip.startSlot != b->timelineTrack().clip.startSlot)
            return a->timelineTrack().clip.startSlot < b->timelineTrack().clip.startSlot;
        return a->mediaId() < b->mediaId();
    });
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
                {QStringLiteral("centerY"), pending.center.y()},
                {QStringLiteral("startSlot"), pending.startSlot}
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
        // Older pending imports were always placed at zero.
        const qreal startSlot = item.contains(QStringLiteral("startSlot"))
            ? item.value(QStringLiteral("startSlot")).toDouble(invalid) : 0;
        // A synchronous snapshot during adoption can contain both forms of
        // the same import. The concrete media is already authoritative.
        if (mediaById(pending.mediaId) || m_pendingImports.contains(pending.mediaId)) continue;
        if (m_media.size() + m_pendingImports.size() >= SceneTimeline::MaximumMediaCount
            || pending.mediaId.isEmpty() || !std::isfinite(pending.center.x())
            || !std::isfinite(startSlot) || std::floor(startSlot) != startSlot
            || startSlot < 0 || startSlot >= m_timelineSettings.maxSlot()
            || !std::isfinite(pending.center.y()) || pending.sourceSignature.isEmpty()
            || !QFileInfo(pending.sourcePath).isAbsolute()
            || sourceSignature(pending.sourcePath) != pending.sourceSignature) {
            if (skippedMediaIds && !pending.mediaId.isEmpty()
                && !skippedMediaIds->contains(pending.mediaId))
                skippedMediaIds->append(pending.mediaId);
            continue;
        }
        pending.startSlot = qint64(startSlot);
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
    if (m_editsLocked || state.value("renderSchemaVersion").toInt(-1) != SceneTimeline::RenderSchemaVersion) return {};
    const auto copiedItems = state.value("media").toArray();
    const auto reject = [&]() {
        if (skippedMediaIds) for (const auto& value : copiedItems)
            skippedMediaIds->append(value.toObject().value("mediaId").toString());
        return QStringList{};
    };
    SceneTimeline::SceneSettings settings;
    if (copiedItems.isEmpty() || !SceneTimeline::SceneSettings::fromJson(state.value("timeline").toObject(), &settings)
        || settings.slotsPerSecond != m_timelineSettings.slotsPerSecond) return reject();
    int firstCopiedTrack = SceneTimeline::MaximumTrackIndex, lastCopiedTrack = SceneTimeline::MinimumTrackIndex;
    for (const auto& value : copiedItems) {
        SceneTimeline::MediaTrack track;
        if (!SceneTimeline::MediaTrack::fromJson(value.toObject().value("timeline").toObject(), &track, m_timelineSettings.maxSlot())) return reject();
        firstCopiedTrack = qMin(firstCopiedTrack, track.trackIndex);
        lastCopiedTrack = qMax(lastCopiedTrack, track.trackIndex);
    }
    const int span = lastCopiedTrack - firstCopiedTrack + 1;
    const int offset = (m_media.isEmpty() ? 0 : qMin(0, firstTimelineTrack()) - span) - firstCopiedTrack;
    QJsonArray items;
    for (auto* media : m_media) items.append(timelineMediaSnapshot(media->mediaId()));
    QHash<QString, QString> paths, newIds;
    QStringList inserted;
    for (const auto& value : copiedItems) {
        auto item = value.toObject();
        const QString oldId = item.value("mediaId").toString();
        SceneTimeline::MediaTrack track;
        SceneTimeline::MediaTrack::fromJson(item.value("timeline").toObject(), &track, m_timelineSettings.maxSlot());
        const QString newId = SceneTimeline::newId();
        item.insert("mediaId", newId);
        track.trackIndex += offset;
        track.clip.id = SceneTimeline::newId();
        for (auto& key : track.keyframes) key.id = SceneTimeline::newId();
        item.insert("timeline", track.toJson());
        items.append(item); inserted.append(newId); newIds.insert(oldId, newId);
        paths.insert(newId, sourcePaths.value(oldId));
    }
    const QString primary = newIds.value(state.value("primaryMediaId").toString(), inserted.first());
    if (!applyMediaPlan(items, paths, primary, true, nullptr, inserted)) return reject();
    return inserted;
}

QStringList CanvasDocument::insertProjectMedia(
    const QJsonObject& state, const QHash<QString, QString>& sourcePaths,
    QStringList* skippedMediaIds, bool freshIds, QHash<QString,QString>* insertedIds)
{
    Q_UNUSED(freshIds);
    QJsonArray items;
    QStringList accepted;
    for (const auto& value : state.value("media").toArray()) {
        const auto item = value.toObject();
        const QString id = item.value("mediaId").toString();
        SceneTimeline::ElementState element; SceneTimeline::MediaTrack track;
        const double duration = item.value("durationMs").toDouble(-1);
        const bool video = item.value("type").toString() == "video";
        if (id.isEmpty() || mediaById(id) || accepted.contains(id)
            || !SceneTimeline::ElementState::fromMediaJson(item, &element)
            || !SceneTimeline::MediaTrack::fromJson(item.value("timeline").toObject(), &track, m_timelineSettings.maxSlot())
            || (video && (!std::isfinite(duration) || duration <= 0 || duration > SceneTimeline::MaximumSupportedDurationMs || std::floor(duration) != duration))
            || !SceneTimeline::validateMediaTrack(track, element.type, video ? qint64(duration) : 0)
            || (element.type != "text" && !QFileInfo(sourcePaths.value(id)).isFile())) {
            if (skippedMediaIds) skippedMediaIds->append(id);
            continue;
        }
        items.append(item); accepted.append(id);
    }
    if (items.isEmpty()) return {};
    if (!applyMediaPlan(items, sourcePaths, {}, false, nullptr)) {
        if (skippedMediaIds) skippedMediaIds->append(accepted);
        return {};
    }
    if (insertedIds) for (const auto& id : accepted) insertedIds->insert(id, id);
    return accepted;
}

namespace {
bool timelineFailure(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
QJsonObject withTimeline(QJsonObject item, const SceneTimeline::MediaTrack& track)
{
    item.insert(QStringLiteral("timeline"), track.toJson());
    return item;
}
QJsonObject freshTimelineInstance(QJsonObject item, SceneTimeline::MediaTrack track)
{
    item.insert(QStringLiteral("mediaId"), SceneTimeline::newId());
    track.clip.id = SceneTimeline::newId();
    for (auto& key : track.keyframes) key.id = SceneTimeline::newId();
    return withTimeline(item, track);
}
}

CanvasMedia* CanvasDocument::mediaForTimelineClip(const QString& clipId) const
{
    if (clipId.isEmpty()) return nullptr;
    for (auto* media : m_media)
        if (media->timelineTrack().clip.id == clipId) return media;
    return nullptr;
}

int CanvasDocument::firstTimelineTrack() const
{
    int first = SceneTimeline::MaximumTrackIndex;
    for (auto* media : m_media) first = qMin(first, media->timelineTrack().trackIndex);
    return m_media.isEmpty() ? 0 : first;
}

int CanvasDocument::timelineTrackCount() const
{
    if (m_media.isEmpty()) return 1;
    int last = 0;
    for (auto* media : m_media) last = qMax(last, media->timelineTrack().trackIndex);
    const int firstRowTrack = qMax(SceneTimeline::MinimumTrackIndex, qMin(0, firstTimelineTrack() - 1));
    const int lastRowTrack = qMin(SceneTimeline::MaximumTrackIndex, last + 1);
    return lastRowTrack - firstRowTrack + 1;
}

int CanvasDocument::timelineRow(int trackIndex) const
{ return trackIndex - timelineTrackAtRow(0); }

int CanvasDocument::timelineTrackAtRow(int row) const
{ return m_media.isEmpty() ? 0 : qMax(SceneTimeline::MinimumTrackIndex, qMin(0, firstTimelineTrack() - 1)) + row; }

bool CanvasDocument::timelinePlacementFree(const QString& clipId, const ClipPlacement& placement) const
{
    if (placement.startSlot < 0 || placement.endSlot > m_timelineSettings.maxSlot()
        || placement.endSlot <= placement.startSlot || placement.trackIndex < SceneTimeline::MinimumTrackIndex
        || placement.trackIndex > SceneTimeline::MaximumTrackIndex) return false;
    for (auto* media : m_media) {
        const auto& track = media->timelineTrack();
        if (track.clip.id != clipId && track.trackIndex == placement.trackIndex
            && track.clip.startSlot < placement.endSlot && placement.startSlot < track.clip.endSlot()) return false;
    }
    return true;
}

CanvasDocument::ClipPlacement CanvasDocument::previewTimelineClip(const QString& clipId,
    ClipPlacement requested, int edge, const ClipPlacement& lastValid, PlacementMode mode) const
{
    const auto* media = mediaForTimelineClip(clipId);
    if (!media) return lastValid;
    const auto& original = media->timelineTrack();
    const auto maximum = m_timelineSettings.maxSlot();
    if (edge == 0) {
        requested.startSlot = qBound<qint64>(0, requested.startSlot, maximum - original.clip.durationSlots);
        requested.endSlot = requested.startSlot + original.clip.durationSlots;
    } else {
        requested.trackIndex = original.trackIndex;
        if (edge < 0) {
            requested.endSlot = original.clip.endSlot();
            requested.startSlot = qBound<qint64>(0, requested.startSlot, requested.endSlot - 1);
        } else {
            requested.startSlot = original.clip.startSlot;
            requested.endSlot = qBound(requested.startSlot + 1, requested.endSlot, maximum);
        }
    }
    if (mode == PlacementMode::Overwrite) return requested;
    // The UI retains this anchor independently of any overlapping Control preview.
    auto anchor = lastValid;
    if (!timelinePlacementFree(clipId, anchor))
        anchor = {original.clip.startSlot, original.clip.endSlot(), original.trackIndex};
    if (requested.trackIndex != anchor.trackIndex)
        return timelinePlacementFree(clipId, requested) ? requested : anchor;

    qint64 left = 0, right = maximum;
    for (auto* other : m_media) {
        const auto& track = other->timelineTrack();
        if (track.clip.id == clipId || track.trackIndex != anchor.trackIndex) continue;
        if (track.clip.endSlot() <= anchor.startSlot) left = qMax(left, track.clip.endSlot());
        else if (track.clip.startSlot >= anchor.endSlot) right = qMin(right, track.clip.startSlot);
    }
    // Clamp to the connected free interval, even if one pointer event jumps past a neighbour.
    if (edge == 0) {
        requested.startSlot = qBound(left, requested.startSlot, right - original.clip.durationSlots);
        requested.endSlot = requested.startSlot + original.clip.durationSlots;
    } else if (edge < 0) requested.startSlot = qMax(left, requested.startSlot);
    else requested.endSlot = qMin(right, requested.endSlot);
    return timelinePlacementFree(clipId, requested) ? requested : anchor;
}

bool CanvasDocument::insertMediaAbove(CanvasMedia* media, QString* error)
{
    QJsonArray items;
    for (auto* existing : m_media) items.append(timelineMediaSnapshot(existing));
    auto track = media->timelineTrack();
    track.trackIndex = m_media.isEmpty() ? 0 : qMin(0, firstTimelineTrack()) - 1;
    items.append(withTimeline(timelineMediaSnapshot(media), track));
    return applyMediaPlan(items, {{media->mediaId(), media->sourcePath()}}, media->mediaId(), true,
                          error, {}, media);
}

QJsonObject CanvasDocument::timelineMediaSnapshot(const QString& mediaId) const
{
    return timelineMediaSnapshot(mediaById(mediaId));
}

QJsonObject CanvasDocument::timelineMediaSnapshot(const CanvasMedia* media) const
{
    if (!media) return {};
    auto item = media->authorElementState().toJson();
    item.insert(QStringLiteral("mediaId"), media->mediaId());
    item.insert(QStringLiteral("fileId"), media->fileId());
    item.insert(QStringLiteral("fileName"), QFileInfo(media->sourcePath()).fileName());
    item.insert(QStringLiteral("timeline"), media->timelineTrack().toJson());
    if (media->isVideo()) item.insert(QStringLiteral("durationMs"), double(media->sourceDurationMs()));
    QJsonArray spans;
    const auto author = media->authorElementState();
    for (auto it = m_screenRects.cbegin(); it != m_screenRects.cend(); ++it) {
        const auto span = spanForIntersection(it.key(), it.value(), {author.position, author.size});
        if (!span.isEmpty()) spans.append(span);
    }
    item.insert(QStringLiteral("spans"), spans);
    return item;
}

CanvasMedia* CanvasDocument::createMediaFromSnapshot(const QJsonObject& item, const QString& path) const
{
    SceneTimeline::ElementState state;
    SceneTimeline::MediaTrack track;
    if (!SceneTimeline::ElementState::fromMediaJson(item, &state)
        || !SceneTimeline::MediaTrack::fromJson(item.value("timeline").toObject(), &track, m_timelineSettings.maxSlot())) return nullptr;
    QSize nativeSize = state.baseSize.toSize();
    if (state.type != "text") {
        const QString canonicalPath = QFileInfo(path).canonicalFilePath();
        for (auto* existing : m_media) {
            if (!existing->isText() && existing->nativeSourceSize().isValid()
                && QFileInfo(existing->sourcePath()).canonicalFilePath() == canonicalPath) {
                nativeSize = existing->nativeSourceSize();
                break;
            }
        }
    }
    auto* media = new CanvasMedia(state.type == "text" ? CanvasMedia::Type::Text
        : state.type == "video" ? CanvasMedia::Type::Video : CanvasMedia::Type::Image, nativeSize);
    media->restoreMediaId(item.value("mediaId").toString());
    media->setElementState(state);
    media->setTimelineTrack(track);
    media->setResidencySuspended(m_mediaResidencySuspended);
    if (media->isVideo()) media->restoreSourceDurationMs(qint64(item.value("durationMs").toDouble()));
    if (!media->isText()) media->setSourcePath(path, item.value("fileId").toString());
    if (media->isVideo()) media->initializeVideoRuntime();
    return media;
}

bool CanvasDocument::applyMediaPlan(const QJsonArray& items,
    const QHash<QString, QString>& paths, const QString& primaryId, bool selectOnly, QString* error,
    const QStringList& selectedIds, CanvasMedia* preparedMedia)
{
    if (m_editsLocked || m_publishingTimelineEdit) return timelineFailure(error, "Timeline editing is locked.");
    if (items.size() + m_pendingImports.size() > SceneTimeline::MaximumMediaCount)
        return timelineFailure(error, "A scene cannot contain more than 512 instances.");
    auto scene = serializeSceneState();
    scene.insert("media", items);
    if (QJsonDocument(scene).toJson(QJsonDocument::Compact).size() > 8 * 1024 * 1024)
        return timelineFailure(error, "This operation would exceed the scene size limit (8 MiB).");

    QSet<QString> mediaIds, identities;
    QHash<QString, SceneTimeline::MediaTrack> tracks;
    for (const auto& value : items) {
        const auto item = value.toObject();
        const auto id = item.value("mediaId").toString();
        SceneTimeline::ElementState state;
        SceneTimeline::MediaTrack track;
        if (id.isEmpty() || id.size() > 128 || identities.contains(id)
            || !SceneTimeline::ElementState::fromMediaJson(item, &state, error)
            || !SceneTimeline::MediaTrack::fromJson(item.value("timeline").toObject(), &track, m_timelineSettings.maxSlot(), error))
            return timelineFailure(error, "Invalid timeline instance.");
        const double duration = item.value("durationMs").toDouble(-1);
        if (state.type == "video" && (!std::isfinite(duration) || duration <= 0
            || duration > SceneTimeline::MaximumSupportedDurationMs || std::floor(duration) != duration))
            return timelineFailure(error, "Invalid video duration.");
        if (!SceneTimeline::validateMediaTrack(track, state.type, state.type == "video" ? qint64(duration) : 0, error)) return false;
        identities.insert(id);
        if (identities.contains(track.clip.id)) return timelineFailure(error, "Duplicate clip identity.");
        identities.insert(track.clip.id);
        for (const auto& key : track.keyframes) {
            if (identities.contains(key.id)) return timelineFailure(error, "Duplicate keyframe identity.");
            identities.insert(key.id);
        }
        for (auto it = tracks.cbegin(); it != tracks.cend(); ++it) {
            const auto& other = it.value();
            if (other.trackIndex == track.trackIndex && other.clip.startSlot < track.clip.endSlot()
                && track.clip.startSlot < other.clip.endSlot()) return timelineFailure(error, "Overlapping clips on one track.");
        }
        if (!mediaById(id) && state.type != "text" && !QFileInfo(paths.value(id)).isFile())
            return timelineFailure(error, "The source file is unavailable.");
        const QString fileId = item.value("fileId").toString();
        if (!mediaById(id) && state.type != "text" && m_fileManager && m_fileManager->hasFileId(fileId)) {
            const QString canonical = QFileInfo(paths.value(id)).canonicalFilePath();
            bool knownPath = false;
            for (const QString& known : m_fileManager->getRecordedFilePathsForId(fileId))
                if (QFileInfo(known).canonicalFilePath() == canonical) { knownPath = true; break; }
            if (!knownPath) return timelineFailure(error, "The copied source identity does not match a verified source path.");
        }
        mediaIds.insert(id); tracks.insert(id, track);
    }

    QList<CanvasMedia*> created;
    for (const auto& value : items) {
        const auto item = value.toObject();
        if (mediaById(item.value("mediaId").toString())) continue;
        auto* media = preparedMedia && item.value("mediaId").toString() == preparedMedia->mediaId()
            ? preparedMedia : createMediaFromSnapshot(item, paths.value(item.value("mediaId").toString()));
        if (!media) {
            for (auto* candidate : created) if (candidate != preparedMedia) delete candidate;
            return timelineFailure(error, "Could not create timeline instance.");
        }
        created.append(media);
    }
    const auto oldSelected = selectedMediaIds();
    const auto oldPrimary = m_primarySelectedMediaId;
    QList<CanvasMedia*> removed;
    m_publishingTimelineEdit = true;
    std::vector<std::unique_ptr<QSignalBlocker>> mediaSignals;
    for (auto* media : m_media) mediaSignals.emplace_back(std::make_unique<QSignalBlocker>(media));
    for (auto* media : created) mediaSignals.emplace_back(std::make_unique<QSignalBlocker>(media));
    if (m_fileManager) m_fileManager->beginMediaAssociationTransaction();
    // Install replacement references before releasing the previous last owner.
    for (auto* media : created) {
        if (m_fileManager && !media->isText() && !media->fileId().isEmpty() && m_fileManager->hasFileId(media->fileId()))
            m_fileManager->associateMediaWithFile(media->mediaId(), media->fileId());
        adoptMedia(media);
    }
    for (auto* media : std::as_const(m_media)) {
        if (!mediaIds.contains(media->mediaId())) { removed.append(media); continue; }
        media->setTimelineTrack(tracks.value(media->mediaId()));
    }
    for (auto* media : removed) {
        m_media.removeAll(media);
        m_selectionActivationOrder.removeAll(media->mediaId());
    }
    if (selectOnly) m_selectionActivationOrder = selectedIds;
    for (auto* media : std::as_const(m_media)) {
        const bool selected = selectOnly ? media->mediaId() == primaryId || selectedIds.contains(media->mediaId()) : media->selected() || media->mediaId() == primaryId;
        media->setSelected(selected);
    }
    m_primarySelectedMediaId = mediaIds.contains(primaryId) ? primaryId
        : m_selectionActivationOrder.isEmpty() ? QString() : m_selectionActivationOrder.last();
    if (!m_primarySelectedMediaId.isEmpty()) {
        m_selectionActivationOrder.removeAll(m_primarySelectedMediaId);
        m_selectionActivationOrder.append(m_primarySelectedMediaId);
    }
    evaluateTimeline();
    // Direct media observers, as well as document projections, only see the
    // completed graph. Replay property notifications before document publication.
    mediaSignals.clear();
    for (auto* media : m_media) if (!created.contains(media)) emit media->changed();
    // Lifecycle observers can now inspect only the completed authoring graph.
    for (auto* media : removed) {
        emit mediaAboutToBeRemoved(media);
        if (m_fileManager && !media->isText()) m_fileManager->removeMediaAssociation(media->mediaId());
    }
    if (m_fileManager) m_fileManager->endMediaAssociationTransaction();
    m_publishingTimelineEdit = false;
    for (auto* media : created) emit mediaAdded(media);
    for (auto* media : removed) {
        emit mediaRemoved(media->mediaId());
        // New owners queued their acquires during preparation. Let those run
        // before retiring the previous last lease of a shared decoded source.
        QMetaObject::invokeMethod(media, [media] {
            media->retireResidency();
            media->deleteLater();
        }, Qt::QueuedConnection);
    }
    for (auto* media : m_media) if (!created.contains(media)) emit mediaChanged(media->mediaId());
    emit timelineEvaluated();
    if (oldPrimary != m_primarySelectedMediaId) emit primarySelectedMediaChanged();
    if (oldPrimary != m_primarySelectedMediaId || oldSelected != selectedMediaIds()) emit selectionChanged();
    emit documentChanged();
    return true;
}

bool CanvasDocument::applyTimelinePlacement(const QJsonObject& incoming, const QString& sourcePath,
                                          bool freshInstance, QString* error, PlacementMode mode)
{
    SceneTimeline::MediaTrack placed;
    const auto timeline = incoming.value("timeline").toObject();
    if (!SceneTimeline::MediaTrack::fromJson(timeline, &placed, m_timelineSettings.maxSlot(), error)) return false;
    const QString incomingId = incoming.value("mediaId").toString();
    if (!freshInstance && !m_editsLocked && !m_publishingTimelineEdit && incoming == timelineMediaSnapshot(incomingId)) return true;
    QJsonArray result;
    QHash<QString, QString> paths;
    paths.insert(incomingId, sourcePath);
    for (auto* media : m_media) {
        if (media->mediaId() == incomingId) continue;
        const auto original = timelineMediaSnapshot(media->mediaId());
        const auto& old = media->timelineTrack();
        const auto& cut = placed.clip;
        if (old.trackIndex != placed.trackIndex || old.clip.endSlot() <= cut.startSlot || old.clip.startSlot >= cut.endSlot()) {
            result.append(original); continue;
        }
        if (mode == PlacementMode::Avoid) return timelineFailure(error, "There is not enough space on this track.");
        const bool left = old.clip.startSlot < cut.startSlot;
        const bool right = old.clip.endSlot() > cut.endSlot();
        if (left) {
            auto fragment = old; fragment.clip.durationSlots = cut.startSlot - old.clip.startSlot;
            result.append(withTimeline(original, fragment));
        }
        if (right) {
            auto fragment = old;
            fragment.clip.startSlot = cut.endSlot();
            fragment.clip.durationSlots = old.clip.endSlot() - cut.endSlot();
            if (fragment.clip.sourceStartSlot) *fragment.clip.sourceStartSlot += cut.endSlot() - old.clip.startSlot;
            auto item = left ? freshTimelineInstance(original, fragment) : withTimeline(original, fragment);
            paths.insert(item.value("mediaId").toString(), media->sourcePath());
            result.append(item);
        }
    }
    result.append(incoming);
    return applyMediaPlan(result, paths, incomingId, freshInstance, error);
}

bool CanvasDocument::moveTimelineClip(const QString& clipId, qint64 startSlot, int trackIndex, QString* error, PlacementMode mode)
{
    auto* media = mediaForTimelineClip(clipId);
    if (!media) return timelineFailure(error, "Unknown clip.");
    auto track = media->timelineTrack();
    track.clip.startSlot = qBound<qint64>(0, startSlot, m_timelineSettings.maxSlot() - track.clip.durationSlots);
    track.trackIndex = trackIndex;
    return applyTimelinePlacement(withTimeline(timelineMediaSnapshot(media->mediaId()), track), media->sourcePath(), false, error, mode);
}

bool CanvasDocument::trimTimelineClip(const QString& clipId, qint64 startSlot, qint64 endSlot, QString* error, PlacementMode mode)
{
    auto* media = mediaForTimelineClip(clipId);
    if (!media) return timelineFailure(error, "Unknown clip.");
    auto track = media->timelineTrack();
    startSlot = qBound<qint64>(0, startSlot, m_timelineSettings.maxSlot() - 1);
    endSlot = qBound(startSlot + 1, endSlot, m_timelineSettings.maxSlot());
    if (track.clip.sourceStartSlot) *track.clip.sourceStartSlot += startSlot - track.clip.startSlot;
    track.clip.startSlot = startSlot; track.clip.durationSlots = endSlot - startSlot;
    return applyTimelinePlacement(withTimeline(timelineMediaSnapshot(media->mediaId()), track), media->sourcePath(), false, error, mode);
}

bool CanvasDocument::splitTimelineClip(const QString& clipId, qint64 slot, QString* error)
{
    auto* media = mediaForTimelineClip(clipId);
    if (!media) return timelineFailure(error, "Unknown clip.");
    const auto old = media->timelineTrack();
    if (slot <= old.clip.startSlot || slot >= old.clip.endSlot()) return timelineFailure(error, "The cut must be inside the clip.");
    auto left = old, right = old;
    left.clip.durationSlots = slot - old.clip.startSlot;
    right.clip.startSlot = slot; right.clip.durationSlots = old.clip.endSlot() - slot;
    if (right.clip.sourceStartSlot) *right.clip.sourceStartSlot += left.clip.durationSlots;
    const auto snapshot = timelineMediaSnapshot(media->mediaId());
    const auto clone = freshTimelineInstance(snapshot, right);
    QJsonArray items;
    for (auto* item : m_media) items.append(item == media ? withTimeline(snapshot, left) : timelineMediaSnapshot(item->mediaId()));
    items.append(clone);
    return applyMediaPlan(items, {{clone.value("mediaId").toString(), media->sourcePath()}}, media->mediaId(), false, error);
}

QString CanvasDocument::pasteTimelineClip(const QJsonObject& snapshot, const QHash<QString, QString>& paths,
                                         qint64 startSlot, int trackIndex, QString* error)
{
    SceneTimeline::MediaTrack track;
    if (startSlot < 0 || startSlot >= m_timelineSettings.maxSlot()
        || !SceneTimeline::MediaTrack::fromJson(snapshot.value("timeline").toObject(), &track, m_timelineSettings.maxSlot(), error)) return {};
    track.clip.startSlot = startSlot;
    track.clip.durationSlots = qMin(track.clip.durationSlots, m_timelineSettings.maxSlot() - startSlot);
    track.trackIndex = trackIndex;
    const auto item = freshTimelineInstance(snapshot, track);
    if (!applyTimelinePlacement(item, paths.value(snapshot.value("mediaId").toString()), true, error, PlacementMode::Overwrite)) return {};
    return item.value("mediaId").toString();
}
