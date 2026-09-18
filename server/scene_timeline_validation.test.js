'use strict';
const assert = require('node:assert/strict');
const { MAXIMUM_DURATION_MS, isCanonicalElement, isCanonicalTimelineSettings,
    isCanonicalMediaTrack, isCanonicalTimelineSnapshot } = require('./scene_timeline_validation');
const image = { type: 'image', x: 0, y: 0, width: 1920, height: 1080,
    baseWidth: 1920, baseHeight: 1080, scale: 1, visible: true, z: 1,
    contentOpacity: 1, opacityOverrideEnabled: false, rawOpacity: 1 };
const key = { id: 'key-1', timeMs: 500, state: image };
const track = { keyframes: [key], clips: [], clipsInitialized: false };
assert.equal(isCanonicalElement(image), true);
assert.equal(isCanonicalElement({ ...image, autoDisplay: true }), false);
assert.equal(isCanonicalElement({ ...image, contentOpacity: NaN }), false);
assert.equal(isCanonicalElement({ ...image, width: 0 }), false);
assert.equal(isCanonicalTimelineSettings({ maxDurationMs: 180000, stopTimeMs: -1 }), true);
assert.equal(isCanonicalTimelineSettings({ maxDurationMs: 180000, stopTimeMs: 180000 }), true);
assert.equal(isCanonicalTimelineSettings({ maxDurationMs: 180000, stopTimeMs: 0 }), true);
for (const maximum of [0, MAXIMUM_DURATION_MS + 1, 1.5, '180000'])
    assert.equal(isCanonicalTimelineSettings({ maxDurationMs: maximum, stopTimeMs: -1 }), false);
for (const stop of [-2, 180001, 1.5, '500'])
    assert.equal(isCanonicalTimelineSettings({ maxDurationMs: 180000, stopTimeMs: stop }), false);
assert.equal(isCanonicalMediaTrack(track, 'image', 1000), true);
assert.equal(isCanonicalMediaTrack(track, 'video', 1000), false);
assert.equal(isCanonicalMediaTrack({ ...track, keyframes: [key, { ...key, id: 'key-2' }] }, 'image', 1000), false);
assert.equal(isCanonicalMediaTrack({ ...track, keyframes: [key, { ...key, timeMs: 600 }] }, 'image', 1000), false);
assert.equal(isCanonicalMediaTrack({ ...track, keyframes: [{ ...key, timeMs: 1001 }] }, 'image', 1000), false);
assert.equal(isCanonicalMediaTrack({ ...track, keyframes: [{ ...key, timeMs: 1000 }] }, 'image', 1000), true);
assert.equal(isCanonicalMediaTrack({ ...track, extra: true }, 'image', 1000), false);
const clip = { id: 'clip-1', startMs: 200, sourceInMs: 300, sourceOutMs: 500 };
const videoTrack = { clipsInitialized: true, keyframes: [], clips: [clip] };
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', 1000, 500), true);
assert.equal(isCanonicalMediaTrack(videoTrack, 'image', 1000, 500), false);
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', 1000, 499), false);
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', 399, 500), false);
assert.equal(isCanonicalMediaTrack({ ...videoTrack, clips: [clip, { ...clip, id: 'clip-2', startMs: 400 }] }, 'video', 1000, 500), true);
assert.equal(isCanonicalMediaTrack({ ...videoTrack, clips: [clip, { ...clip, id: 'clip-2', startMs: 399 }] }, 'video', 1000, 500), false);
const settings = { maxDurationMs: 180000, stopTimeMs: 5000 };
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 0 }, settings), true);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 0 }, { ...settings, stopTimeMs: 0 }), true);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 1 }, { ...settings, stopTimeMs: 0 }), false);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 5000 }, settings), true);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 5001 }, settings), false);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 5000.1 }, settings), false);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 4000, media: [] }, settings), false);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 180000 }, { ...settings, stopTimeMs: -1 }), true);
console.log('scene timeline validation tests passed');
