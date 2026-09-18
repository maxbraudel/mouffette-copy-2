#pragma once

#include "backend/domain/scene/SceneTimeline.h"

// Splitting an uninterrupted source range must not flush the decoder/audio
// queue when the playhead crosses the new editing boundary.
inline bool contiguousTimelineClips(const SceneTimeline::MediaTrack& track,
                                   const QString& previousId, const QString& nextId)
{
    if (previousId.isEmpty() || nextId.isEmpty()) return false;
    const SceneTimeline::VideoClip* previous = nullptr;
    const SceneTimeline::VideoClip* next = nullptr;
    for (const auto& clip : track.clips) {
        if (clip.id == previousId) previous = &clip;
        if (clip.id == nextId) next = &clip;
    }
    return previous && next && previous->endMs() == next->startMs
        && previous->sourceOutMs == next->sourceInMs;
}
