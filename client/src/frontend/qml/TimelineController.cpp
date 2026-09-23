#include "frontend/qml/TimelineController.h"

#include "backend/config/AppConfig.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/scene/SceneTimeline.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMimeData>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

// Update rows in place so selecting a clip cannot destroy a pointer grab.
class TimelineClipModel final : public QAbstractListModel
{
public:
    explicit TimelineClipModel(QObject* parent) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_rows.size(); }
    QVariant data(const QModelIndex& index, int role) const override {
        return index.isValid() && index.row() >= 0 && index.row() < m_rows.size() && role == Qt::UserRole
            ? m_rows.at(index.row()) : QVariant{};
    }
    QHash<int, QByteArray> roleNames() const override { return {{Qt::UserRole, "modelData"}}; }
    void publish(const QVariantList& rows) {
        QSet<QString> desired;
        for (const auto& row : rows) desired.insert(row.toMap().value("id").toString());
        for (int i = m_rows.size() - 1; i >= 0; --i) {
            if (desired.contains(m_rows[i].toMap().value("id").toString())) continue;
            beginRemoveRows({}, i, i); m_rows.removeAt(i); endRemoveRows();
        }
        for (const auto& row : rows) {
            const auto id = row.toMap().value("id").toString();
            int at = -1;
            for (int i = 0; i < m_rows.size(); ++i)
                if (m_rows[i].toMap().value("id").toString() == id) { at = i; break; }
            if (at < 0) {
                const int end = m_rows.size();
                beginInsertRows({}, end, end); m_rows.append(row); endInsertRows();
            } else if (m_rows[at] != row) {
                m_rows[at] = row; emit dataChanged(index(at), index(at), {Qt::UserRole});
            }
        }
    }
private:
    QVariantList m_rows;
};

namespace {
constexpr auto ClipboardMime = "application/x-mouffette-timeline-v6";
QJsonObject clipboardObject()
{
    const auto* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(ClipboardMime)) return {};
    const auto bytes = mime->data(ClipboardMime);
    if (bytes.size() > 8 * 1024 * 1024) return {};
    return QJsonDocument::fromJson(bytes).object();
}
QVariantMap keyframeRow(const SceneTimeline::Keyframe& key, const CanvasMedia* media, const SceneTimeline::SceneSettings& grid)
{
    return {{QStringLiteral("id"), key.id}, {QStringLiteral("timeMs"), grid.timeMs(key.slot)}, {QStringLiteral("slot"), key.slot},
            {QStringLiteral("mediaId"), media->mediaId()},
            {QStringLiteral("mediaName"), media->displayName()}};
}
QVariantMap clipRow(const SceneTimeline::Clip& clip, const CanvasMedia* media, const SceneTimeline::SceneSettings& grid)
{
    return {{QStringLiteral("id"), clip.id},
        {QStringLiteral("mediaId"), media->mediaId()},
        {QStringLiteral("mediaName"), media->displayName()},
        {QStringLiteral("selected"), media->selected()},
        {QStringLiteral("contentReady"), media->contentReady()},
        {QStringLiteral("loadingState"), media->loadingState()},
        {QStringLiteral("loadingError"), media->loadingError()},
        {QStringLiteral("thumbnailOwnerId"), !media->isText() && !media->residencySuspended()
            ? media->residencyOwnerId() : QString()},
        {QStringLiteral("trackIndex"), media->timelineTrack().trackIndex},
        {QStringLiteral("isVideo"), clip.sourceStartSlot.has_value()},
        {QStringLiteral("startMs"), grid.timeMs(clip.startSlot)},
        {QStringLiteral("startSlot"), clip.startSlot},
        {QStringLiteral("endMs"), grid.timeMs(clip.endSlot())},
        {QStringLiteral("sourceInMs"), grid.timeMs(clip.sourceStartSlot.value_or(0))},
        {QStringLiteral("durationMs"), grid.timeMs(clip.durationSlots)},
        {QStringLiteral("durationSlots"), clip.durationSlots},
        {QStringLiteral("actualSourceDurationMs"), media->sourceDurationMs()}};
}

}

TimelineController::TimelineController(QObject* parent) : QObject(parent), m_clipModel(new TimelineClipModel(this))
{
    connect(this, &TimelineController::changed, this, &TimelineController::transportChanged);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
            this, &TimelineController::changed);
}

TimelineController::~TimelineController()
{
    clearClipPreview();
}

void TimelineController::setHost(QuickCanvasHost* host)
{
    if (m_host == host) return;
    clearClipPreview();
    if (m_host) {
        m_host->timelineEndScrub(false);
        disconnect(m_host, nullptr, this, nullptr);
    }
    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
        for (auto* media : m_document->media()) disconnect(media, nullptr, this, nullptr);
    }
    m_activeTrackIndex = -1;
    m_selectionArea = SelectionArea::Clips;
    m_host = host;
    m_document = host ? host->document() : nullptr;
    m_keyframeId.clear();
    if (host) {
        connect(host, &QuickCanvasHost::timelineTransportChanged,
                this, &TimelineController::transportChanged);
        connect(host->controller(), &QuickCanvasController::editingEnabledChanged,
                this, &TimelineController::transportChanged);
        connect(host, &ICanvasHost::actionStateChanged,
                this, &TimelineController::transportChanged);
    }
    if (m_document) {
        const auto observe = [this](CanvasMedia* media) {
            connect(media, &CanvasMedia::draftChanged, this, &TimelineController::transportChanged);
            connect(media, &CanvasMedia::residencyChanged, this, &TimelineController::refresh);
            connect(media, &CanvasMedia::contentAvailabilityChanged, this, &TimelineController::refresh);
            connect(media, &CanvasMedia::runtimeStateChanged, this,
                    [this, media, duration = media->sourceDurationMs()]() mutable {
                if (duration == media->sourceDurationMs()) return;
                duration = media->sourceDurationMs();
                refresh();
            });
        };
        for (auto* media : m_document->media()) observe(media);
        connect(m_document, &CanvasDocument::mediaAdded, this, observe);
        connect(m_document, &CanvasDocument::documentChanged, this, &TimelineController::refresh);
        connect(m_document, &CanvasDocument::selectionChanged, this, &TimelineController::refresh);
        connect(m_document, &CanvasDocument::mediaRemoved, this, &TimelineController::refresh);
    }
    refresh();
}

CanvasMedia* TimelineController::primary() const
{ return m_document ? m_document->primarySelectedMedia() : nullptr; }
SceneTimeline::SceneSettings TimelineController::grid() const
{
    if (m_document) return m_document->timelineSettings();
    SceneTimeline::SceneSettings settings;
    settings.maxDurationMs = AppConfig::instance().timelineMaxDurationMs();
    settings.slotsPerSecond = AppConfig::instance().timelineSlotsPerSecond();
    return settings;
}
qint64 TimelineController::positionSlot() const
{ return grid().slotAt(m_document ? m_document->timelinePositionMs() : 0); }
int TimelineController::slotsPerSecond() const { return grid().slotsPerSecond; }
qreal TimelineController::playbackStartMs() const
{ return m_host ? m_host->timelinePlaybackStartMs() : 0; }
qreal TimelineController::positionMs() const { return grid().timeMs(positionSlot()); }
qreal TimelineController::maxDurationMs() const { return grid().timeMs(grid().maxSlot()); }
qreal TimelineController::stopTimeMs() const
{ return grid().stopSlot < 0 ? -1 : grid().timeMs(grid().stopSlot); }
qreal TimelineController::effectiveEndMs() const { return grid().effectiveStopMs(); }
qreal TimelineController::gridTime(qreal timeMs) const { return grid().timeMs(grid().nearestSlot(timeMs)); }
void TimelineController::stepSlots(int delta)
{
    if (editable()) seek(grid().timeMs(qBound<qint64>(0, positionSlot() + delta, grid().maxSlot())));
}
void TimelineController::seekClipBoundary(int direction)
{
    if (!editable() || !m_document || direction == 0) return;
    const qint64 current = positionSlot();
    qint64 target = current;
    for (const auto* media : m_document->media()) {
        const auto& clip = media->timelineTrack().clip;
        for (qint64 boundary : {clip.startSlot, clip.endSlot()}) {
            if (direction > 0 ? boundary > current && (target == current || boundary < target)
                              : boundary < current && (target == current || boundary > target))
                target = boundary;
        }
    }
    if (target != current) seek(grid().timeMs(target));
}
bool TimelineController::playing() const
{ return m_host && (m_host->timelinePlaying() || m_host->testSceneLaunched()); }
bool TimelineController::remoteActive() const
{ return m_host && (m_host->remoteSceneLaunched() || m_host->remoteSceneLaunching() || m_host->remoteSceneStopping()); }
bool TimelineController::editable() const
{ return m_host && m_host->controller()->editingEnabled() && !remoteActive() && !playing(); }
bool TimelineController::hasDraft() const { return primary() && primary()->hasElementDraft(); }
QString TimelineController::primaryMediaId() const { return primary() ? primary()->mediaId() : QString(); }
QString TimelineController::selectedClipId() const
{ return primary() ? primary()->timelineTrack().clip.id : QString(); }
bool TimelineController::hasActiveSelection() const
{ return primary() && (m_selectionArea == SelectionArea::Clips || !m_keyframeId.isEmpty()); }
QString TimelineController::primaryMediaName() const { return primary() ? primary()->displayName() : QString(); }
bool TimelineController::primaryIsVideo() const { return primary() && primary()->isVideo(); }
bool TimelineController::canCapture() const { return editable() && primary(); }

QVariantList TimelineController::keyframes() const
{
    QVariantList rows;
    if (auto* media = primary())
        for (const auto& key : media->timelineTrack().keyframes) rows.append(keyframeRow(key, media, grid()));
    return rows;
}
QVariantList TimelineController::otherKeyframes() const
{
    QVariantList rows;
    if (m_document) for (auto* media : m_document->media()) {
        if (media == primary()) continue;
        for (const auto& key : media->timelineTrack().keyframes) rows.append(keyframeRow(key, media, grid()));
    }
    return rows;
}
QVariantList TimelineController::clips() const
{
    QVariantList rows;
    if (!m_document) return rows;
    const auto timing = grid();
    // Match neighbours in integer slots; visual proximity must never create a joint trim.
    QMap<std::pair<int, qint64>, const CanvasMedia*> starts, ends;
    const int firstTrack = m_document->timelineTrackAtRow(0);
    const auto interval = [&](const CanvasMedia* media) -> QVariantMap {
        if (!media) return {};
        const auto& track = media->timelineTrack();
        return {{"id", track.clip.id}, {"startMs", timing.timeMs(track.clip.startSlot)},
                {"endMs", timing.timeMs(track.clip.endSlot())}, {"displayTrackIndex", track.trackIndex - firstTrack}};
    };
    for (auto* media : m_document->media()) {
        const auto& track = media->timelineTrack();
        starts.insert({track.trackIndex, track.clip.startSlot}, media);
        ends.insert({track.trackIndex, track.clip.endSlot()}, media);
    }
    for (auto* media : m_document->media()) {
        const auto& track = media->timelineTrack();
        auto row = clipRow(track.clip, media, timing);
        row.insert(QStringLiteral("displayTrackIndex"), track.trackIndex - firstTrack);
        row.insert(QStringLiteral("startNeighbour"), interval(ends.value({track.trackIndex, track.clip.startSlot})));
        row.insert(QStringLiteral("endNeighbour"), interval(starts.value({track.trackIndex, track.clip.endSlot()})));
        rows.append(row);
    }
    return rows;
}
QAbstractItemModel* TimelineController::clipModel() const { return m_clipModel; }
int TimelineController::trackCount() const { return m_document ? m_document->timelineTrackCount() : 1; }
int TimelineController::firstTrackIndex() const { return m_document ? m_document->timelineTrackAtRow(0) : 0; }
int TimelineController::activeTrackIndex() const
{ return m_activeTrackIndex < 0 ? (m_document ? m_document->timelineRow(0) : 0) : qMin(m_activeTrackIndex, trackCount() - 1); }
void TimelineController::setActiveTrackIndex(int index)
{
    if (!editable() || index < 0 || index >= trackCount()) return;
    m_activeTrackIndex = index;
    clearSelection();
}
bool TimelineController::hasKeyframeAtPosition() const
{
    if (auto* media = primary()) for (const auto& key : media->timelineTrack().keyframes)
        if (key.slot == positionSlot()) return true;
    return false;
}
bool TimelineController::canSplit() const
{
    if (!editable() || !primary()) return false;
    const auto& clip = primary()->timelineTrack().clip;
    return clip.startSlot < positionSlot() && clip.endSlot() > positionSlot();
}
bool TimelineController::canPaste() const
{
    if (!editable() || !m_document) return false;
    const auto value = clipboardObject();
    if (value.value("projectId").toString() != m_document->projectId()) return false;
    const auto kind = value.value("kind").toString();
    if (kind == QLatin1String("clip")) return value.value("media").isObject() && positionSlot() < grid().maxSlot();
    return kind == QLatin1String("keyframe") && primary()
        && value.value("mediaId").toString() == primaryMediaId();
}

int TimelineController::canvasMinHeightPercent() const { return AppConfig::instance().canvasMinHeightPercent(); }
int TimelineController::timelineMinHeightPercent() const { return AppConfig::instance().timelineMinHeightPercent(); }
int TimelineController::timelineSplitterHitHeightPx() const { return AppConfig::instance().timelineSplitterHitHeightPx(); }
int TimelineController::timelineHeightPx() const { return AppConfig::instance().timelineHeightPx(); }
int TimelineController::rulerHeightPx() const { return AppConfig::instance().timelineRulerHeightPx(); }
int TimelineController::clipTrackHeightPx() const { return AppConfig::instance().timelineClipTrackHeightPx(); }
int TimelineController::clipResizeHandleWidthPx() const { return AppConfig::instance().timelineClipResizeHandleWidthPx(); }
int TimelineController::clipJointResizeHandleWidthPx() const { return AppConfig::instance().timelineClipJointResizeHandleWidthPx(); }
int TimelineController::clipJointMinResizeWidthPx() const { return AppConfig::instance().timelineClipJointMinResizeWidthPx(); }
int TimelineController::clipMinResizeWidthPx() const { return AppConfig::instance().timelineClipMinResizeWidthPx(); }
int TimelineController::keyframeSizePx() const { return AppConfig::instance().timelineKeyframeSizePx(); }
qreal TimelineController::otherKeyframeOpacity() const { return AppConfig::instance().timelineOtherKeyframeOpacityPercent() / 100.0; }
int TimelineController::snapDistancePx() const { return AppConfig::instance().timelineSnapDistancePx(); }
int TimelineController::autoScrollSpeedPxPerSecond() const { return AppConfig::instance().timelineAutoScrollSpeedPxPerSecond(); }
int TimelineController::initialViewDurationMs() const { return AppConfig::instance().timelineInitialViewDurationMs(); }

void TimelineController::refresh()
{
    const bool primaryChanged = m_primaryId != primaryMediaId();
    std::optional<int> previousTrack;
    for (const auto& value : m_publishedClips) {
        const auto row = value.toMap();
        if (row.value("mediaId").toString() == primaryMediaId()) {
            previousTrack = row.value("trackIndex").toInt(); break;
        }
    }
    if (primaryChanged) {
        m_primaryId = primaryMediaId();
        m_selectionArea = SelectionArea::Clips;
        m_keyframeId.clear();
        m_error.clear();
    }
    if (auto* media = primary()) {
        const auto& track = media->timelineTrack();
        m_activeTrackIndex = m_document->timelineRow(track.trackIndex);
        if (std::none_of(track.keyframes.cbegin(), track.keyframes.cend(),
                        [this](const auto& key) { return key.id == m_keyframeId; })) m_keyframeId.clear();
    }
    if (m_activeTrackIndex >= trackCount()) m_activeTrackIndex = trackCount() - 1;
    const auto keys = keyframes(), others = otherKeyframes(), segments = clips();
    m_clipModel->publish(segments);
    if (keys != m_publishedKeys || others != m_publishedOtherKeys || segments != m_publishedClips) {
        m_publishedKeys = keys;
        m_publishedOtherKeys = others;
        m_publishedClips = segments;
        emit tracksChanged();
    }
    emit changed();
    emit transportChanged();
    // Inserting or selecting a clip never moves the viewport under the pointer.
    // Only an actual track change can reveal an existing clip.
    if (primary() && previousTrack && *previousTrack != primary()->timelineTrack().trackIndex)
        emit revealTrack(activeTrackIndex());
}
void TimelineController::error(const QString& text) { m_error = text; emit changed(); }
bool TimelineController::commitTrack(CanvasMedia* media, const SceneTimeline::MediaTrack& track)
{
    if (!media || !editable()) return false;
    SceneTimeline::MediaTrack validated;
    QString reason;
    if (!SceneTimeline::MediaTrack::fromJson(track.toJson(), &validated, grid().maxSlot(), &reason)) {
        error(reason);
        return false;
    }
    if (!SceneTimeline::validateMediaTrack(validated, media->typeName(), media->sourceDurationMs(), &reason)) {
        error(reason);
        return false;
    }
    auto scene = m_document->serializeSceneState();
    auto items = scene.value(QStringLiteral("media")).toArray();
    for (qsizetype i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        if (item.value(QStringLiteral("mediaId")).toString() != media->mediaId()) continue;
        item.insert(QStringLiteral("timeline"), track.toJson());
        items[i] = item;
        break;
    }
    scene.insert(QStringLiteral("media"), items);
    if (QJsonDocument(scene).toJson(QJsonDocument::Compact).size() > 8 * 1024 * 1024) {
        error(QStringLiteral("This operation would exceed the scene size limit (8 MiB)."));
        return false;
    }
    media->setTimelineTrack(validated);
    return true;
}
void TimelineController::reevaluate()
{
    m_error.clear();
    if (m_host) m_host->timelineSeek(positionMs());
    refresh();
}
void TimelineController::seek(qreal timeMs)
{
    if (!m_host || remoteActive()) return;
    m_host->controller()->discardPendingEdits();
    m_host->timelineSeek(gridTime(timeMs));
    m_error.clear();
    emit transportChanged();
}
void TimelineController::togglePlayback()
{
    if (!m_host || remoteActive()) return;
    m_host->controller()->discardPendingEdits();
    if (playing()) m_host->timelinePause(); else m_host->timelinePlay();
    emit transportChanged();
}
void TimelineController::beginScrub()
{
    if (!m_host || remoteActive()) return;
    m_host->controller()->discardPendingEdits();
    m_host->timelineBeginScrub();
}
void TimelineController::endScrub(bool resume)
{
    if (m_host) m_host->timelineEndScrub(resume);
}
void TimelineController::goToStart() { seek(0); }
void TimelineController::goToEnd() { seek(effectiveEndMs()); }

void TimelineController::placeKeyframe()
{
    if (!canCapture()) return;
    m_host->controller()->finishSelectionScaleGesture();
    auto* media = primary();
    auto track = media->timelineTrack();
    SceneTimeline::Keyframe key{SceneTimeline::newId(), positionSlot(),
        SceneTimeline::materialize(media->displayedElementState())};
    for (const auto& existing : track.keyframes) if (existing.slot == key.slot) key.id = existing.id;
    if (!SceneTimeline::upsertKeyframe(track, key, grid().maxSlot())) return;
    if (!commitTrack(media, track)) return;
    m_keyframeId = key.id;
    m_selectionArea = SelectionArea::Keyframes;
    reevaluate();
}
void TimelineController::selectKeyframe(const QString& id)
{
    if (!primary() || !editable()) return;
    for (const auto& key : primary()->timelineTrack().keyframes) if (key.id == id) {
        const qreal time = grid().timeMs(key.slot);
        m_keyframeId = id;
        m_selectionArea = SelectionArea::Keyframes;
        seek(time);
        emit changed();
        return;
    }
}
void TimelineController::moveKeyframe(const QString& id, qreal timeMs)
{
    if (!canCapture()) return;
    auto track = primary()->timelineTrack();
    timeMs = gridTime(timeMs);
    if (!SceneTimeline::moveKeyframe(track, id, grid().nearestSlot(timeMs), grid().maxSlot())) return;
    if (!commitTrack(primary(), track)) return;
    m_keyframeId = id;
    m_selectionArea = SelectionArea::Keyframes;
    seek(timeMs);
    refresh();
}
void TimelineController::selectClip(const QString& id)
{
    if (!editable() || !m_document) return;
    auto* media = m_document->mediaForTimelineClip(id);
    if (!media) return;
    m_document->select(media->mediaId());
    m_activeTrackIndex = m_document->timelineRow(media->timelineTrack().trackIndex);
    m_selectionArea = SelectionArea::Clips;
    emit changed();
}
void TimelineController::moveClip(const QString& id, qreal startMs, int row, bool overwrite)
{
    if (!editable() || !m_document) return;
    auto* media = m_document->mediaForTimelineClip(id);
    if (!media) return;
    QString reason;
    if (row >= trackCount()) return;
    const int destination = row < 0 ? media->timelineTrack().trackIndex : m_document->timelineTrackAtRow(row);
    const auto mode = overwrite ? CanvasDocument::PlacementMode::Overwrite : CanvasDocument::PlacementMode::Avoid;
    if (!m_document->moveTimelineClip(id, grid().nearestSlot(startMs), destination, &reason, mode)) {
        if (!reason.isEmpty()) error(reason);
        return;
    }
    m_selectionArea = SelectionArea::Clips;
    m_activeTrackIndex = m_document->timelineRow(media->timelineTrack().trackIndex);
    reevaluate();
    emit revealTrack(activeTrackIndex());
}
void TimelineController::trimClip(const QString& id, qreal startMs, qreal endMs, bool overwrite, bool rolling)
{
    if (!editable() || !m_document) return;
    QString reason;
    const auto mode = overwrite ? CanvasDocument::PlacementMode::Overwrite : CanvasDocument::PlacementMode::Avoid;
    const auto trimMode = rolling ? CanvasDocument::TrimMode::Rolling : CanvasDocument::TrimMode::Independent;
    if (!m_document->trimTimelineClip(id, grid().nearestSlot(startMs), grid().nearestSlot(endMs), &reason, mode, trimMode)) {
        if (!reason.isEmpty()) error(reason);
        return;
    }
    m_selectionArea = SelectionArea::Clips;
    reevaluate();
}
QVariantMap TimelineController::previewClipEdit(const QString& id, qreal startMs, qreal endMs, int row,
    int edge, qreal lastStartMs, qreal lastEndMs, int lastRow, bool overwrite, const QVariantMap& snap, bool rolling) const
{
    if (!m_document || !m_document->mediaForTimelineClip(id)) return {};
    const auto timing = grid();
    const CanvasDocument::ClipPlacement requested{timing.nearestSlot(startMs), timing.nearestSlot(endMs),
        m_document->timelineTrackAtRow(qBound(0, row, trackCount() - 1))};
    const CanvasDocument::ClipPlacement last{timing.nearestSlot(lastStartMs), timing.nearestSlot(lastEndMs),
        m_document->timelineTrackAtRow(qBound(0, lastRow, trackCount() - 1))};
    const auto result = m_document->previewTimelineClip(id, requested, edge, last,
        overwrite ? CanvasDocument::PlacementMode::Overwrite : CanvasDocument::PlacementMode::Avoid,
        rolling ? CanvasDocument::TrimMode::Rolling : CanvasDocument::TrimMode::Independent);
    QVariantMap preview{{"startMs", timing.timeMs(result.startSlot)}, {"endMs", timing.timeMs(result.endSlot)},
            {"row", m_document->timelineRow(result.trackIndex)},
            {"free", m_document->timelinePlacementFree(id, result)}};
    if (const auto* neighbour = rolling && !overwrite ? m_document->adjacentTimelineClip(id, edge) : nullptr) {
        const auto& clip = neighbour->timelineTrack().clip;
        preview.insert("adjacentClip", QVariantMap{{"id", clip.id},
            {"startMs", timing.timeMs(edge < 0 ? clip.startSlot : result.endSlot)},
            {"endMs", timing.timeMs(edge < 0 ? result.startSlot : clip.endSlot())}});
        preview.insert("free", true);
    }
    // Validate the actual snapped edge after every placement constraint. Other
    // coordinates (or a rejected track change) cannot invalidate this alignment.
    QVariantMap guide;
    const int snappedEdge = edge == 0 ? snap.value("edge").toInt() : edge;
    if (snap.value("snapped").toBool() && snap.contains("targetSlot")
        && (snappedEdge == -1 || snappedEdge == 1)
        && snap.value("targetSlot").toLongLong() == (snappedEdge < 0 ? result.startSlot : result.endSlot))
        guide = snap;
    preview.insert("snap", guide);
    return preview;
}

void TimelineController::applyClipPreview(const QString& id, const QVariantMap& preview,
                                         int edge, bool overwrite, bool rolling)
{
    if (!editable() || !m_document || !preview.contains("startMs") || !preview.contains("endMs")) return;
    const auto timing = grid();
    m_document->setTimelineClipPreview(id,
        {timing.nearestSlot(preview.value("startMs").toReal()),
         timing.nearestSlot(preview.value("endMs").toReal()),
         m_document->timelineTrackAtRow(preview.value("row").toInt())}, edge,
        overwrite ? CanvasDocument::PlacementMode::Overwrite : CanvasDocument::PlacementMode::Avoid,
        rolling ? CanvasDocument::TrimMode::Rolling : CanvasDocument::TrimMode::Independent);
}

void TimelineController::clearClipPreview()
{
    if (m_document) m_document->clearTimelineClipPreview();
}

void TimelineController::splitClip()
{
    if (!canSplit()) return;
    const auto id = primary()->timelineTrack().clip.id;
    QString reason;
    if (!m_document->splitTimelineClip(id, positionSlot(), &reason)) {
        if (!reason.isEmpty()) error(reason);
        return;
    }
    m_selectionArea = SelectionArea::Clips;
    reevaluate();
}
void TimelineController::deleteSelected()
{
    if (!canCapture() || !hasActiveSelection()) return;
    auto* media = primary();
    if (m_selectionArea == SelectionArea::Keyframes) {
        auto track = media->timelineTrack();
        const auto displayed = SceneTimeline::materialize(media->displayedElementState());
        if (!SceneTimeline::removeKeyframe(track, m_keyframeId) || !commitTrack(media, track)) return;
        if (track.keyframes.isEmpty()) media->setElementState(displayed);
        clearKeyframeSelection();
    } else {
        if (!m_document->removeMedia(media->mediaId())) return;
        clearSelection();
    }
    reevaluate();
}
void TimelineController::copySelected()
{
    if (!editable() || !hasActiveSelection()) return;
    QJsonObject value{{"mediaId", primaryMediaId()}, {"projectId", m_document->projectId()}};
    if (m_selectionArea == SelectionArea::Keyframes) {
        for (const auto& key : primary()->timelineTrack().keyframes) if (key.id == m_keyframeId) {
            value.insert("kind", "keyframe"); value.insert("state", key.state.toJson()); break;
        }
    } else {
        value.insert("kind", "clip");
        value.insert("media", m_document->timelineMediaSnapshot(primaryMediaId()));
        value.insert("sourcePath", primary()->sourcePath());
    }
    if (!value.contains("kind")) return;
    auto* mime = new QMimeData;
    mime->setData(ClipboardMime, QJsonDocument(value).toJson(QJsonDocument::Compact));
    QGuiApplication::clipboard()->setMimeData(mime);
}
void TimelineController::paste()
{
    if (!canPaste()) return;
    const auto value = clipboardObject();
    if (value.value("kind") == QLatin1String("keyframe")) {
        SceneTimeline::ElementState state;
        if (!SceneTimeline::ElementState::fromJson(value.value("state").toObject(), &state)) return;
        auto track = primary()->timelineTrack();
        SceneTimeline::Keyframe key{SceneTimeline::newId(), positionSlot(), state};
        if (!SceneTimeline::upsertKeyframe(track, key, grid().maxSlot()) || !commitTrack(primary(), track)) return;
        m_keyframeId = key.id;
        m_selectionArea = SelectionArea::Keyframes;
    } else {
        const auto snapshot = value.value("media").toObject();
        QHash<QString, QString> paths;
        paths.insert(snapshot.value("mediaId").toString(), value.value("sourcePath").toString());
        QString reason;
        const auto id = m_document->pasteTimelineClip(snapshot, paths, positionSlot(), m_document->timelineTrackAtRow(activeTrackIndex()), &reason);
        if (id.isEmpty()) { if (!reason.isEmpty()) error(reason); return; }
        if (auto* media = m_document->mediaById(id)) {
            m_document->select(id);
            m_activeTrackIndex = m_document->timelineRow(media->timelineTrack().trackIndex);
            m_keyframeId.clear();
            m_selectionArea = SelectionArea::Clips;
            seek(grid().timeMs(media->timelineTrack().clip.endSlot()));
            emit revealPlayhead();
        }
    }
    reevaluate();
}
void TimelineController::placeStop() { setStopTime(positionMs()); }
void TimelineController::setStopTime(qreal timeMs)
{
    if (!editable()) return;
    auto settings = m_document->timelineSettings();
    settings.stopSlot = grid().nearestSlot(timeMs);
    m_document->setTimelineSettings(settings);
    refresh();
}
void TimelineController::removeStop()
{
    if (!editable()) return;
    auto settings = m_document->timelineSettings();
    settings.stopSlot = -1;
    m_document->setTimelineSettings(settings);
    refresh();
}
void TimelineController::clearSelection()
{
    if (!editable()) return;
    m_selectionArea = SelectionArea::Clips;
    m_keyframeId.clear();
    if (m_document) m_document->clearSelection();
    refresh();
}

void TimelineController::clearKeyframeSelection()
{
    if (!editable()) return;
    m_selectionArea = SelectionArea::Keyframes;
    m_keyframeId.clear();
    emit changed();
}

QVariantMap TimelineController::snapTime(qreal timeMs, qreal pixelsPerMs,
                                        const QString& excludeId, qreal clipDurationMs, bool includePlayhead, int rollingEdge) const
{
    const auto timing = grid();
    QVariantMap result{{QStringLiteral("timeMs"), gridTime(timeMs)}, {QStringLiteral("snapped"), false},
                      {QStringLiteral("targetTimeMs"), -1}, {QStringLiteral("mediaName"), QString()}};
    if (!m_document || !std::isfinite(timeMs) || !std::isfinite(clipDurationMs) || !std::isfinite(pixelsPerMs)
        || pixelsPerMs <= 0 || clipDurationMs < 0 || clipDurationMs > maxDurationMs()) return result;
    const qint64 durationSlots = timing.nearestSlot(clipDurationMs);
    const auto* neighbour = rollingEdge == -1 || rollingEdge == 1
        ? m_document->adjacentTimelineClip(excludeId, rollingEdge) : nullptr;
    const QString adjacentId = neighbour ? neighbour->timelineTrack().clip.id : QString();
    qreal bestDistance = snapDistancePx() + 1e-6;
    qint64 bestTarget = std::numeric_limits<qint64>::max();
    const auto consider = [&](qint64 targetSlot, const QString& label, const QString& kind) {
        for (qint64 offset : {qint64(0), durationSlots}) {
            const qint64 proposedSlot = targetSlot - offset;
            if (proposedSlot < 0 || proposedSlot > timing.maxSlot() - durationSlots) continue;
            const qreal proposed = timing.timeMs(proposedSlot);
            const qreal distance = std::abs(qreal(proposed) - timeMs) * pixelsPerMs;
            const bool tied = std::abs(distance - bestDistance) < 1e-6;
            const bool preferred = tied && (targetSlot < bestTarget
                || (targetSlot == bestTarget && kind == QLatin1String("playhead")
                    && result.value("targetKind").toString() != QLatin1String("playhead")));
            if (distance > snapDistancePx() || (distance > bestDistance - 1e-6 && !preferred)) continue;
            bestDistance = distance;
            bestTarget = targetSlot;
            result = {{QStringLiteral("timeMs"), proposed}, {QStringLiteral("snapped"), true},
                      {QStringLiteral("targetTimeMs"), timing.timeMs(targetSlot)}, {QStringLiteral("mediaName"), label},
                      {QStringLiteral("targetSlot"), targetSlot}, {QStringLiteral("targetKind"), kind},
                      {QStringLiteral("edge"), offset == 0 ? -1 : 1}};
        }
    };
    for (auto* media : m_document->media()) {
        for (const auto& key : media->timelineTrack().keyframes)
            if (key.id != excludeId) consider(key.slot, media->displayName(), QStringLiteral("keyframe"));
        const auto& clip = media->timelineTrack().clip;
        if (clip.id != excludeId && clip.id != adjacentId) {
            consider(clip.startSlot, media->displayName(), QStringLiteral("clip"));
            consider(clip.endSlot(), media->displayName(), QStringLiteral("clip"));
        }
    }
    // Clip gestures opt in; the scrubber must never snap to its own position.
    if (includePlayhead) consider(positionSlot(), tr("Playhead"), QStringLiteral("playhead"));
    return result;
}
