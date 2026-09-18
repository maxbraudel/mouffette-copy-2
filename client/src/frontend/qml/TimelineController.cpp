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
#include <QMimeData>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr auto ClipboardMime = "application/x-mouffette-timeline-v5";
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
        {QStringLiteral("isVideo"), clip.sourceStartSlot.has_value()},
        {QStringLiteral("startMs"), grid.timeMs(clip.startSlot)},
        {QStringLiteral("startSlot"), clip.startSlot},
        {QStringLiteral("sourceInMs"), grid.timeMs(clip.sourceStartSlot.value_or(0))},
        {QStringLiteral("durationMs"), grid.timeMs(clip.durationSlots)},
        {QStringLiteral("durationSlots"), clip.durationSlots},
        {QStringLiteral("actualSourceDurationMs"), media->sourceDurationMs()}};
}

}

TimelineController::TimelineController(QObject* parent) : QObject(parent)
{
    connect(this, &TimelineController::changed, this, &TimelineController::transportChanged);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
            this, &TimelineController::changed);
}

void TimelineController::setHost(QuickCanvasHost* host)
{
    if (m_host == host) return;
    if (m_host) disconnect(m_host, nullptr, this, nullptr);
    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
        for (auto* media : m_document->media()) disconnect(media, nullptr, this, nullptr);
    }
    m_host = host;
    m_document = host ? host->document() : nullptr;
    m_keyframeId.clear();
    m_clipId.clear();
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
            connect(media, &CanvasMedia::residencyChanged, this, &TimelineController::changed);
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
bool TimelineController::playing() const
{ return m_host && (m_host->timelinePlaying() || m_host->testSceneLaunched()); }
bool TimelineController::remoteActive() const
{ return m_host && (m_host->remoteSceneLaunched() || m_host->remoteSceneLaunching() || m_host->remoteSceneStopping()); }
bool TimelineController::editable() const
{ return m_host && m_host->controller()->editingEnabled() && !remoteActive() && !playing(); }
bool TimelineController::hasDraft() const { return primary() && primary()->hasElementDraft(); }
QString TimelineController::primaryMediaId() const { return primary() ? primary()->mediaId() : QString(); }
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
    if (auto* media = primary())
        for (const auto& clip : media->timelineTrack().clips) rows.append(clipRow(clip, media, grid()));
    return rows;
}
QVariantList TimelineController::otherClips() const
{
    QVariantList rows;
    if (m_document) for (auto* media : m_document->media()) {
        if (media == primary()) continue;
        for (const auto& clip : media->timelineTrack().clips) rows.append(clipRow(clip, media, grid()));
    }
    return rows;
}
bool TimelineController::canInsertClip() const
{
    if (!editable() || !primary() || positionSlot() >= grid().maxSlot()) return false;
    return primaryIsVideo() ? primary()->sourceDurationMs() > 0
        : !SceneTimeline::activeClip(primary()->timelineTrack(), positionSlot());
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
    for (const auto& clip : primary()->timelineTrack().clips)
        if ((m_clipId.isEmpty() || clip.id == m_clipId)
            && clip.startSlot < positionSlot() && clip.endSlot() > positionSlot()) return true;
    return false;
}
bool TimelineController::canPaste() const
{
    if (!canCapture()) return false;
    const auto value = clipboardObject();
    const auto kind = value.value(QStringLiteral("kind")).toString();
    return value.value(QStringLiteral("mediaId")).toString() == primaryMediaId()
        && value.value(QStringLiteral("projectId")).toString() == m_document->projectId()
        && (kind == QLatin1String("keyframe")
            || (kind == QLatin1String("clip") && positionMs() < maxDurationMs()));
}

int TimelineController::timelineHeightPx() const { return AppConfig::instance().timelineHeightPx(); }
int TimelineController::rulerHeightPx() const { return AppConfig::instance().timelineRulerHeightPx(); }
int TimelineController::clipTrackHeightPx() const { return AppConfig::instance().timelineClipTrackHeightPx(); }
int TimelineController::keyframeSizePx() const { return AppConfig::instance().timelineKeyframeSizePx(); }
qreal TimelineController::otherKeyframeOpacity() const { return AppConfig::instance().timelineOtherKeyframeOpacityPercent() / 100.0; }
int TimelineController::snapDistancePx() const { return AppConfig::instance().timelineSnapDistancePx(); }
int TimelineController::initialViewDurationMs() const { return AppConfig::instance().timelineInitialViewDurationMs(); }

void TimelineController::refresh()
{
    if (m_primaryId != primaryMediaId()) {
        m_primaryId = primaryMediaId();
        m_keyframeId.clear();
        m_clipId.clear();
        m_error.clear();
    }
    if (auto* media = primary()) {
        const auto& track = media->timelineTrack();
        if (std::none_of(track.keyframes.cbegin(), track.keyframes.cend(),
                        [this](const auto& key) { return key.id == m_keyframeId; })) m_keyframeId.clear();
        if (std::none_of(track.clips.cbegin(), track.clips.cend(),
                        [this](const auto& clip) { return clip.id == m_clipId; })) m_clipId.clear();
    }
    const auto keys = keyframes(), others = otherKeyframes(), segments = clips(), otherSegments = otherClips();
    if (keys != m_publishedKeys || others != m_publishedOtherKeys || segments != m_publishedClips
        || otherSegments != m_publishedOtherClips) {
        m_publishedKeys = keys;
        m_publishedOtherKeys = others;
        m_publishedClips = segments;
        m_publishedOtherClips = otherSegments;
        emit tracksChanged();
    }
    emit changed();
    emit transportChanged();
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
    if (playing()) m_host->timelinePause();
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
    m_clipId.clear();
    reevaluate();
}
void TimelineController::selectKeyframe(const QString& id)
{
    if (!primary() || !editable()) return;
    for (const auto& key : primary()->timelineTrack().keyframes) if (key.id == id) {
        const qreal time = grid().timeMs(key.slot);
        m_keyframeId = id;
        m_clipId.clear();
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
    m_clipId.clear();
    seek(timeMs);
    refresh();
}
void TimelineController::selectClip(const QString& id)
{
    if (!primary() || !editable()) return;
    for (const auto& clip : primary()->timelineTrack().clips) if (clip.id == id) {
        m_clipId = id;
        m_keyframeId.clear();
        emit changed();
        emit transportChanged();
        return;
    }
}
void TimelineController::moveClip(const QString& id, qreal startMs)
{
    if (!editable() || !primary()) return;
    auto track = primary()->timelineTrack();
    if (!SceneTimeline::moveClip(track, id, grid().nearestSlot(startMs), grid().maxSlot())) return;
    if (!commitTrack(primary(), track)) return;
    m_clipId = id;
    reevaluate();
}
void TimelineController::trimClip(const QString& id, qreal startMs, qreal endMs)
{
    if (!editable() || !primary()) return;
    auto track = primary()->timelineTrack();
    if (!SceneTimeline::trimClip(track, id, grid().nearestSlot(startMs), grid().nearestSlot(endMs), grid().maxSlot())) return;
    if (!commitTrack(primary(), track)) return;
    m_clipId = id;
    reevaluate();
}
void TimelineController::splitClip()
{
    if (!canSplit()) return;
    auto track = primary()->timelineTrack();
    QString id = m_clipId;
    if (id.isEmpty()) for (const auto& clip : track.clips)
        if (clip.startSlot < positionSlot() && positionSlot() < clip.endSlot()) { id = clip.id; break; }
    if (!SceneTimeline::splitClip(track, id, positionSlot())) return;
    if (!commitTrack(primary(), track)) return;
    reevaluate();
}
void TimelineController::insertClip()
{
    if (!canInsertClip()) return;
    auto track = primary()->timelineTrack();
    qint64 end = grid().maxSlot();
    if (primaryIsVideo()) end = qMin(end, positionSlot() + grid().sourceSlots(primary()->sourceDurationMs()));
    else for (const auto& clip : track.clips)
        if (clip.startSlot > positionSlot()) { end = clip.startSlot; break; }
    SceneTimeline::Clip clip{SceneTimeline::newId(), positionSlot(),
        primaryIsVideo() ? std::optional<qint64>(0) : std::nullopt, end - positionSlot()};
    if (!SceneTimeline::insertClip(track, clip, grid().maxSlot())) return;
    if (!commitTrack(primary(), track)) return;
    m_clipId = clip.id;
    m_keyframeId.clear();
    reevaluate();
}
void TimelineController::deleteSelected()
{
    if (!canCapture()) return;
    auto* media = primary();
    auto track = media->timelineTrack();
    const auto displayed = SceneTimeline::materialize(media->displayedElementState());
    const bool wasAnimated = !track.keyframes.isEmpty();
    bool removed = false;
    if (!m_keyframeId.isEmpty()) removed = SceneTimeline::removeKeyframe(track, m_keyframeId);
    else if (!m_clipId.isEmpty()) removed = SceneTimeline::removeClip(track, m_clipId);
    if (!removed) return;
    if (!commitTrack(media, track)) return;
    if (wasAnimated && track.keyframes.isEmpty()) media->setElementState(displayed);
    clearSelection();
    reevaluate();
}
void TimelineController::copySelected()
{
    if (!primary() || !editable()) return;
    QJsonObject value{{QStringLiteral("mediaId"), primaryMediaId()},
                      {QStringLiteral("projectId"), m_document->projectId()}};
    const auto& track = primary()->timelineTrack();
    if (!m_keyframeId.isEmpty()) {
        for (const auto& key : track.keyframes) if (key.id == m_keyframeId) {
            value.insert(QStringLiteral("kind"), QStringLiteral("keyframe"));
            value.insert(QStringLiteral("state"), key.state.toJson());
            break;
        }
    } else if (!m_clipId.isEmpty()) {
        for (const auto& clip : track.clips) if (clip.id == m_clipId) {
            value.insert(QStringLiteral("kind"), QStringLiteral("clip"));
            value.insert(QStringLiteral("sourceStartSlot"), clip.sourceStartSlot ? QJsonValue(double(*clip.sourceStartSlot)) : QJsonValue(QJsonValue::Null));
            value.insert(QStringLiteral("durationSlots"), clip.durationSlots);
            break;
        }
    }
    if (!value.contains(QStringLiteral("kind"))) return;
    auto* mime = new QMimeData;
    mime->setData(ClipboardMime, QJsonDocument(value).toJson(QJsonDocument::Compact));
    QGuiApplication::clipboard()->setMimeData(mime);
}
void TimelineController::paste()
{
    if (!canPaste()) return;
    const auto value = clipboardObject();
    auto track = primary()->timelineTrack();
    if (value.value(QStringLiteral("kind")) == QLatin1String("keyframe")) {
        SceneTimeline::ElementState state;
        if (!SceneTimeline::ElementState::fromJson(value.value(QStringLiteral("state")).toObject(), &state)) return;
        SceneTimeline::Keyframe key{SceneTimeline::newId(), positionSlot(), state};
        if (!SceneTimeline::upsertKeyframe(track, key, grid().maxSlot())) return;
        if (!commitTrack(primary(), track)) return;
        m_keyframeId = key.id;
        m_clipId.clear();
    } else {
        // Reuse the canonical parser, including signed offsets and static nulls.
        auto object = value;
        object.remove(QStringLiteral("kind"));
        object.remove(QStringLiteral("mediaId"));
        object.remove(QStringLiteral("projectId"));
        object.insert(QStringLiteral("id"), SceneTimeline::newId());
        object.insert(QStringLiteral("startSlot"), 0);
        SceneTimeline::MediaTrack copied;
        if (!SceneTimeline::MediaTrack::fromJson({{"keyframes", QJsonArray{}},
                {"clips", QJsonArray{object}}, {"clipsInitialized", true}}, &copied, grid().maxSlot())
            || !SceneTimeline::validateMediaTrack(copied, primary()->typeName(), primary()->sourceDurationMs())) return;
        auto clip = copied.clips.first();
        clip.startSlot = positionSlot();
        clip.durationSlots = qMin(clip.durationSlots, grid().maxSlot() - positionSlot());
        if (!SceneTimeline::insertClip(track, clip, grid().maxSlot())) return;
        if (!commitTrack(primary(), track)) return;
        m_clipId = clip.id;
        m_keyframeId.clear();
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
    m_keyframeId.clear();
    m_clipId.clear();
    emit changed();
    emit transportChanged();
}

QVariantMap TimelineController::snapTime(qreal timeMs, qreal pixelsPerMs,
                                        const QString& excludeId, qreal clipDurationMs) const
{
    QVariantMap result{{QStringLiteral("timeMs"), gridTime(timeMs)}, {QStringLiteral("snapped"), false},
                      {QStringLiteral("targetTimeMs"), -1}, {QStringLiteral("mediaName"), QString()}};
    if (!m_document || !std::isfinite(timeMs) || !std::isfinite(clipDurationMs) || !std::isfinite(pixelsPerMs) || pixelsPerMs <= 0 || clipDurationMs < 0) return result;
    qreal bestDistance = snapDistancePx() + 1e-6;
    qreal bestTarget = std::numeric_limits<qreal>::max();
    const auto consider = [&](qreal target, const CanvasMedia* media) {
        for (qreal offset : {qreal(0), gridTime(clipDurationMs)}) {
            const qreal proposed = gridTime(target - offset);
            if (target - offset < -1e-9 || proposed < 0 || proposed > maxDurationMs() - clipDurationMs + 1e-9) continue;
            const qreal distance = std::abs(qreal(proposed) - timeMs) * pixelsPerMs;
            if (distance > snapDistancePx()
                || (distance > bestDistance - 1e-6 && !(std::abs(distance - bestDistance) < 1e-6 && target < bestTarget))) continue;
            bestDistance = distance;
            bestTarget = target;
            result = {{QStringLiteral("timeMs"), proposed}, {QStringLiteral("snapped"), true},
                      {QStringLiteral("targetTimeMs"), target}, {QStringLiteral("mediaName"), media->displayName()}};
        }
    };
    for (auto* media : m_document->media()) {
        for (const auto& key : media->timelineTrack().keyframes)
            if (key.id != excludeId) consider(grid().timeMs(key.slot), media);
        for (const auto& clip : media->timelineTrack().clips) if (clip.id != excludeId) {
            consider(grid().timeMs(clip.startSlot), media);
            consider(grid().timeMs(clip.endSlot()), media);
        }
    }
    return result;
}
