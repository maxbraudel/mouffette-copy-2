'use strict';
const assert = require('node:assert/strict');
const { MAXIMUM_DURATION_MS, isCanonicalElement, isCanonicalTimelineSettings,
    isCanonicalMediaTrack, isCanonicalTimelineSnapshot } = require('./scene_timeline_validation');
const image = { type: 'image', x: 0, y: 0, width: 1920, height: 1080,
    baseWidth: 1920, baseHeight: 1080, scale: 1, visible: true, z: 1,
    contentOpacity: 1, opacityOverrideEnabled: false, rawOpacity: 1 };
const settings = { maxDurationMs: 180000, stopSlot: -1, slotsPerSecond: 30 };
const key = { id: 'key-1', slot: 15, state: image };
const track = { keyframes: [key], clips: [], clipsInitialized: false };
assert.equal(isCanonicalElement(image), true);
assert.equal(isCanonicalElement({ ...image, autoDisplay: true }), false);
assert.equal(isCanonicalElement({ ...image, contentOpacity: NaN }), false);
assert.equal(isCanonicalElement({ ...image, width: 0 }), false);
assert.equal(isCanonicalTimelineSettings(settings), true);
for (const stopSlot of [0, 5400])
    assert.equal(isCanonicalTimelineSettings({ ...settings, stopSlot }), true);
for (const maxDurationMs of [0, 33, MAXIMUM_DURATION_MS + 1, 1.5, '180000'])
    assert.equal(isCanonicalTimelineSettings({ ...settings, maxDurationMs }), false);
for (const stopSlot of [-2, 5401, 1.5, '500'])
    assert.equal(isCanonicalTimelineSettings({ ...settings, stopSlot }), false);
for (const slotsPerSecond of [0, 241, 29.97, '30', NaN, Infinity])
    assert.equal(isCanonicalTimelineSettings({ ...settings, slotsPerSecond }), false);
assert.equal(isCanonicalTimelineSettings({ maxDurationMs: 180000, stopTimeMs: -1 }), false);
assert.equal(isCanonicalTimelineSettings({ ...settings, maxDurationMs: 999, stopSlot: 30 }), false);
assert.equal(isCanonicalTimelineSettings({ ...settings, maxDurationMs: 999, stopSlot: 29 }), true);
const oneSecond = { ...settings, maxDurationMs: 1000 };
assert.equal(isCanonicalMediaTrack(track, 'image', oneSecond), true);
assert.equal(isCanonicalMediaTrack(track, 'video', oneSecond), false);
for (const keyframes of [[key, { ...key, id: 'key-2' }], [key, { ...key, slot: 16 }],
    [{ ...key, slot: 31 }], [{ ...key, slot: 1.5 }], [{ id: 'old', timeMs: 1, state: image }]])
    assert.equal(isCanonicalMediaTrack({ ...track, keyframes }, 'image', oneSecond), false);
assert.equal(isCanonicalMediaTrack({ ...track, keyframes: [{ ...key, slot: 30 }] }, 'image', oneSecond), true);
assert.equal(isCanonicalMediaTrack({ ...track, extra: true }, 'image', oneSecond), false);
const clip = { id: 'clip-1', startSlot: 6, sourceStartSlot: 3, durationSlots: 5 };
const videoTrack = { clipsInitialized: true, keyframes: [], clips: [clip] };
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', oneSecond, 250), true); // 8 source slots
assert.equal(isCanonicalMediaTrack(videoTrack, 'image', oneSecond, 250), false);
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', oneSecond, 233), false); // Only 7 source slots
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', { ...oneSecond, maxDurationMs: 333 }, 250), false);
for (const [duration, length] of [[1, 1], [250, 8], [300, 9]]) {
    const short = { ...videoTrack, clips: [{ ...clip, startSlot: 0, sourceStartSlot: 0, durationSlots: length }] };
    assert.equal(isCanonicalMediaTrack(short, 'video', oneSecond, duration), true);
    short.clips[0].durationSlots++;
    assert.equal(isCanonicalMediaTrack(short, 'video', oneSecond, duration), false);
}
for (const [startSlot, accepted] of [[11, true], [10, false]])
    assert.equal(isCanonicalMediaTrack({ ...videoTrack, clips: [clip, { ...clip, id: 'clip-2', startSlot }] }, 'video', oneSecond, 250), accepted);
const stop = { ...settings, stopSlot: 8 };
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 0 }, stop), true);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 0 }, { ...settings, stopSlot: 0 }), true);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 1 }, { ...settings, stopSlot: 0 }), false);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 8000/30 }, stop), true);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 266.7 }, stop), false);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 250.123 }, stop), true); // Continuous network clock
for (const time of [-1, NaN, Infinity, '0'])
    assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: time }, stop), false);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 0, media: [] }, settings), false);
assert.equal(isCanonicalTimelineSnapshot({ timelinePositionMs: 180000 }, settings), true);
console.log('scene timeline grid validation tests passed');
