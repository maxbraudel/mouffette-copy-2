'use strict';
const assert = require('node:assert/strict');
const { MAXIMUM_DURATION_MS, MAXIMUM_TRACK_INDEX, isCanonicalElement, isCanonicalTimelineSettings,
    isCanonicalMediaTrack, isCanonicalSceneTracks, isCanonicalTimelineSnapshot } = require('./scene_timeline_validation');
const image = { type: 'image', x: 0, y: 0, width: 1920, height: 1080,
    baseWidth: 1920, baseHeight: 1080, scale: 1, visible: true,
    contentOpacity: 1, opacityOverrideEnabled: false, rawOpacity: 1 };
const settings = { maxDurationMs: 180000, stopSlot: -1, slotsPerSecond: 30 };
const key = { id: 'key-1', slot: 15, state: image };
const track = { keyframes: [key], trackIndex: 0,
    clip: { id: 'static-clip', startSlot: 0, sourceStartSlot: null, durationSlots: 30 } };
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
const videoTrack = { trackIndex: 0, keyframes: [], clip };
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', oneSecond, 250), true); // 8 source slots
assert.equal(isCanonicalMediaTrack(videoTrack, 'image', oneSecond, 250), false);
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', oneSecond, 233), true); // The tail holds the final source frame
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', { ...oneSecond, maxDurationMs: 333 }, 250), false);
for (const [duration, length] of [[1, 1], [250, 8], [300, 9]]) {
    const short = { ...videoTrack, clip: { ...clip, startSlot: 0, sourceStartSlot: 0, durationSlots: length } };
    assert.equal(isCanonicalMediaTrack(short, 'video', oneSecond, duration), true);
    short.clip.durationSlots++;
    assert.equal(isCanonicalMediaTrack(short, 'video', oneSecond, duration), true);
}
for (const [startSlot, accepted] of [[11, true], [10, false]])
    assert.equal(isCanonicalSceneTracks([{ mediaId: 'media-a', timeline: videoTrack }, { mediaId: 'media-b', timeline:
        { ...videoTrack, clip: { ...clip, id: 'clip-2', startSlot } } }]), accepted);
assert.equal(isCanonicalSceneTracks([{ mediaId: 'media-a', timeline: videoTrack }, { mediaId: 'media-b', timeline:
    { ...videoTrack, trackIndex: 2, clip: { ...clip, id: 'clip-2' } } }]), true);
assert.equal(isCanonicalSceneTracks([{ mediaId: 'media-a', timeline: videoTrack }, { mediaId: 'media-b', timeline:
    { ...videoTrack, trackIndex: 2 } }]), false);
assert.equal(isCanonicalSceneTracks([{ mediaId: 'media-a', timeline: track }, { mediaId: 'media-b', timeline:
    { ...track, trackIndex: 2, clip: { ...track.clip, id: 'second-static' } } }]), false);
assert.equal(isCanonicalSceneTracks([{ mediaId: 'media-a', timeline: track }, { mediaId: 'media-b', timeline:
    { ...track, keyframes: [], trackIndex: 2, clip: { ...track.clip, id: key.id } } }]), false);
assert.equal(isCanonicalSceneTracks([{ mediaId: clip.id, timeline: videoTrack }]), false);
assert.equal(isCanonicalSceneTracks([{ mediaId: key.id, timeline: track }]), false);
const crossKindCollision = [{ mediaId: 'media-a', timeline: videoTrack },
    { mediaId: clip.id, timeline: { ...videoTrack, trackIndex: 2, clip: { ...clip, id: 'clip-2' } } }];
assert.equal(isCanonicalSceneTracks(crossKindCollision), false);
assert.equal(isCanonicalSceneTracks([...crossKindCollision].reverse()), false);
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

// Presence clips have the same schema; only source offsets differ by media type.
const staticTrack = { ...videoTrack, clip: { ...clip, sourceStartSlot: null, durationSlots: 20 } };
for (const type of ['image', 'text']) {
    assert.equal(isCanonicalMediaTrack(staticTrack, type, oneSecond), true);
    assert.equal(isCanonicalMediaTrack(videoTrack, type, oneSecond), false);
}
assert.equal(isCanonicalMediaTrack(staticTrack, 'video', oneSecond, 250), false);
for (const sourceStartSlot of [-100, -3, 100, 100000])
    assert.equal(isCanonicalMediaTrack({ ...videoTrack, clip: { ...clip, sourceStartSlot } }, 'video', oneSecond, 250), true);
for (const sourceStartSlot of [null, '0', 0.5, NaN, Infinity, -(2 ** 52) - 1, 2 ** 52])
    assert.equal(isCanonicalMediaTrack({ ...videoTrack, clip: { ...clip, sourceStartSlot } }, 'video', oneSecond, 250), false);
assert.equal(isCanonicalMediaTrack(videoTrack, 'video', oneSecond, 0), false);

// Layer order is derived from the track, never authored or keyframed.
assert.equal(isCanonicalElement({ ...image, z: 1 }), false);
for (const trackIndex of [-MAXIMUM_TRACK_INDEX, -2, -1, 0, 2, MAXIMUM_TRACK_INDEX])
    assert.equal(isCanonicalMediaTrack({ ...track, trackIndex }, 'image', oneSecond), true);
for (const trackIndex of [-MAXIMUM_TRACK_INDEX - 1, 0.5, '0', MAXIMUM_TRACK_INDEX + 1, NaN])
    assert.equal(isCanonicalMediaTrack({ ...track, trackIndex }, 'image', oneSecond), false);
for (const clip of [null, [], {}, { ...track.clip, durationSlots: 0 }])
    assert.equal(isCanonicalMediaTrack({ ...track, clip }, 'image', oneSecond), false);
assert.equal(isCanonicalMediaTrack({ keyframes: [], clips: [], clipsInitialized: false }, 'image', oneSecond), false);
