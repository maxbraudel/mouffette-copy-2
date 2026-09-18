'use strict';

// Canonical render schema 6. Keep numeric bounds in sync with SceneTimeline.cpp.
const MAXIMUM_SOURCE_OFFSET_SLOTS = 2 ** 52;
const MAXIMUM_DURATION_MS = 604_800_000;
const MAXIMUM_TRACK_INDEX = 9999;
const COMMON_ELEMENT_KEYS = Object.freeze([
    'type', 'x', 'y', 'width', 'height', 'baseWidth', 'baseHeight', 'scale',
    'visible', 'contentOpacity', 'opacityOverrideEnabled', 'rawOpacity',
]);
const TEXT_ELEMENT_KEYS = Object.freeze([
    'text', 'fontFamily', 'fontPixelSize', 'fontWeight', 'fontItalic', 'fontUnderline',
    'fontUppercase', 'textColor', 'textOutlineWidthPx', 'textOutlineWidthPercent',
    'textBorderColor', 'textHighlightEnabled', 'textHighlightColor', 'textFitToTextEnabled',
    'horizontalAlignment', 'verticalAlignment', 'fontWeightOverrideEnabled', 'rawFontWeight',
    'textColorOverrideEnabled', 'rawTextColor', 'outlineWidthOverrideEnabled',
    'rawOutlineWidthPercent', 'outlineColorOverrideEnabled', 'rawOutlineColor',
]);
const VIDEO_ELEMENT_KEYS = Object.freeze(['muted', 'volume']);
const plain = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const finite = (value, low, high) => Number.isFinite(value) && value >= low && value <= high;
const integer = (value, low, high) => Number.isSafeInteger(value) && finite(value, low, high);
const boolean = value => typeof value === 'boolean';
const identifier = value => typeof value === 'string' && value.length > 0 && value.length <= 128;
const color = value => typeof value === 'string' && /^#[0-9a-f]{8}$/i.test(value);
const onlyKeys = (value, keys) => plain(value) && keys.every(key => Object.hasOwn(value, key))
    && Object.keys(value).every(key => keys.includes(key));

function elementKeys(type) {
    return [...COMMON_ELEMENT_KEYS, ...(type === 'video' ? VIDEO_ELEMENT_KEYS
        : type === 'text' ? TEXT_ELEMENT_KEYS : [])];
}
function isCanonicalElement(value, extraKeys = []) {
    if (!plain(value) || !['image', 'video', 'text'].includes(value.type)
        || !onlyKeys(value, [...elementKeys(value.type), ...extraKeys])
        || !finite(value.x, -1e9, 1e9) || !finite(value.y, -1e9, 1e9)
        || !finite(value.width, 0.0001, 1e9) || !finite(value.height, 0.0001, 1e9)
        || !finite(value.baseWidth, 0.0001, 1e9) || !finite(value.baseHeight, 0.0001, 1e9)
        || !finite(value.scale, 0.0001, 1e9)
        || !boolean(value.visible) || !finite(value.contentOpacity, 0, 1)
        || !boolean(value.opacityOverrideEnabled) || !finite(value.rawOpacity, 0, 1)) return false;
    if (value.type === 'video') return boolean(value.muted) && finite(value.volume, 0, 1);
    if (value.type !== 'text') return true;
    return typeof value.text === 'string' && value.text.length <= 1_000_000
        && typeof value.fontFamily === 'string' && value.fontFamily.length <= 1024
        && finite(value.fontPixelSize, 1, 2048) && finite(value.fontWeight, 1, 1000)
        && boolean(value.fontItalic) && boolean(value.fontUnderline) && boolean(value.fontUppercase)
        && color(value.textColor) && color(value.textBorderColor) && color(value.textHighlightColor)
        && boolean(value.textHighlightEnabled) && boolean(value.textFitToTextEnabled)
        && finite(value.textOutlineWidthPx, 0, 2048) && finite(value.textOutlineWidthPercent, 0, 100)
        && boolean(value.fontWeightOverrideEnabled) && finite(value.rawFontWeight, 1, 1000)
        && boolean(value.textColorOverrideEnabled) && color(value.rawTextColor)
        && boolean(value.outlineWidthOverrideEnabled) && finite(value.rawOutlineWidthPercent, 0, 100)
        && boolean(value.outlineColorOverrideEnabled) && color(value.rawOutlineColor)
        && ['left', 'center', 'right'].includes(value.horizontalAlignment)
        && ['top', 'center', 'bottom'].includes(value.verticalAlignment);
}
const maximumSlot = settings => Math.floor(settings.maxDurationMs * settings.slotsPerSecond / 1000);
function isCanonicalTimelineSettings(value) {
    return onlyKeys(value, ['maxDurationMs', 'stopSlot', 'slotsPerSecond'])
        && integer(value.maxDurationMs, 1, MAXIMUM_DURATION_MS)
        && integer(value.slotsPerSecond, 1, 240) && maximumSlot(value) >= 1
        && integer(value.stopSlot, -1, maximumSlot(value));
}
function isCanonicalMediaTrack(value, type, settings, sourceDuration = 0) {
    if (!isCanonicalTimelineSettings(settings) || !['image', 'video', 'text'].includes(type)
        || !onlyKeys(value, ['keyframes', 'clip', 'trackIndex'])
        || !Array.isArray(value.keyframes) || value.keyframes.length > 10_000
        || !integer(value.trackIndex, 0, MAXIMUM_TRACK_INDEX)
        || (type === 'video' && !integer(sourceDuration, 0, MAXIMUM_DURATION_MS))) return false;
    const maximum = maximumSlot(settings);
    const ids = new Set(); const times = new Set();
    for (const key of value.keyframes) {
        if (!onlyKeys(key, ['id', 'slot', 'state']) || !identifier(key.id) || ids.has(key.id)
            || !integer(key.slot, 0, maximum) || times.has(key.slot)
            || !isCanonicalElement(key.state) || key.state.type !== type) return false;
        ids.add(key.id); times.add(key.slot);
    }
    const clip = value.clip;
    return onlyKeys(clip, ['id', 'startSlot', 'sourceStartSlot', 'durationSlots'])
        && identifier(clip.id) && !ids.has(clip.id) && integer(clip.startSlot, 0, maximum)
        && integer(clip.durationSlots, 1, maximum)
        && (type === 'video'
            ? sourceDuration > 0 && integer(clip.sourceStartSlot, -MAXIMUM_SOURCE_OFFSET_SLOTS,
                MAXIMUM_SOURCE_OFFSET_SLOTS - clip.durationSlots)
            : clip.sourceStartSlot === null)
        && clip.startSlot + clip.durationSlots <= maximum;
}
// Per-instance validation cannot detect collisions between separate instances.
function isCanonicalSceneTracks(media) {
    const clipsByTrack = new Map();
    const timelineIds = new Set();
    for (const item of media) {
        const { trackIndex, clip } = item.timeline;
        for (const id of [item.mediaId, clip.id, ...item.timeline.keyframes.map(key => key.id)]) {
            if (timelineIds.has(id)) return false;
            timelineIds.add(id);
        }
        const clips = clipsByTrack.get(trackIndex) || [];
        clips.push(clip);
        clipsByTrack.set(trackIndex, clips);
    }
    for (const clips of clipsByTrack.values()) {
        clips.sort((a, b) => a.startSlot - b.startSlot);
        if (clips.some((clip, index) => index > 0
            && clips[index - 1].startSlot + clips[index - 1].durationSlots > clip.startSlot)) return false;
    }
    return true;
}
function isCanonicalTimelineSnapshot(snapshot, settings) {
    return isCanonicalTimelineSettings(settings) && onlyKeys(snapshot, ['timelinePositionMs'])
        && finite(snapshot.timelinePositionMs, 0,
            (settings.stopSlot < 0 ? maximumSlot(settings) : settings.stopSlot) * 1000 / settings.slotsPerSecond);
}
module.exports = { MAXIMUM_DURATION_MS, MAXIMUM_TRACK_INDEX, isCanonicalElement,
    isCanonicalTimelineSettings, isCanonicalMediaTrack, isCanonicalSceneTracks, isCanonicalTimelineSnapshot };
