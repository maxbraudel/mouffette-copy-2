#pragma once

#include "backend/domain/scene/SceneTimeline.h"

// Each occurrence owns a lightweight cursor into the shared decoder pool.
// Prepare a future clip at its source in-point
// so its first visible frame is already decoded before the timeline reaches it.
inline qint64 timelineVideoPreparationSourceMs(const SceneTimeline::MediaTrack& track,
                                               qreal positionMs, qint64 durationMs,
                                               const SceneTimeline::SceneSettings& settings)
{
    const qreal firstMs = settings.timeMs(track.clip.startSlot);
    return SceneTimeline::evaluateVideo(track, qMax(positionMs, firstMs), durationMs, settings).sourceTimeMs;
}
