'use strict';

// Canonical render schema 3. Keep numeric bounds in sync with SceneTimeline.cpp.
const MAXIMUM_DURATION_MS = 604_800_000;
const COMMON_ELEMENT_KEYS = Object.freeze([
    'type', 'x', 'y', 'width', 'height', 'baseWidth', 'baseHeight', 'scale',
    'visible', 'z', 'contentOpacity', 'opacityOverrideEnabled', 'rawOpacity',
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
        || !finite(value.scale, 0.0001, 1e9) || !finite(value.z, -1e9, 1e9)
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
function isCanonicalTimelineSettings(value) {
    return onlyKeys(value, ['maxDurationMs', 'stopTimeMs'])
        && integer(value.maxDurationMs, 1, MAXIMUM_DURATION_MS)
        && integer(value.stopTimeMs, -1, value.maxDurationMs);
}
function isCanonicalMediaTrack(value, type, maximum, sourceDuration = 0) {
    if (!onlyKeys(value, ['keyframes', 'clips', 'clipsInitialized'])
        || !Array.isArray(value.keyframes) || value.keyframes.length > 10_000
        || !Array.isArray(value.clips) || value.clips.length > 10_000
        || !boolean(value.clipsInitialized)
        || ((!value.clipsInitialized || type !== 'video') && value.clips.length > 0)) return false;
    const ids = new Set(); const times = new Set();
    for (const key of value.keyframes) {
        if (!onlyKeys(key, ['id', 'timeMs', 'state']) || !identifier(key.id) || ids.has(key.id)
            || !integer(key.timeMs, 0, maximum) || times.has(key.timeMs)
            || !isCanonicalElement(key.state) || key.state.type !== type) return false;
        ids.add(key.id); times.add(key.timeMs);
    }
    for (const clip of value.clips) {
        if (!onlyKeys(clip, ['id', 'startMs', 'sourceInMs', 'sourceOutMs'])
            || !identifier(clip.id) || ids.has(clip.id) || !integer(clip.startMs, 0, maximum)
            || !integer(clip.sourceInMs, 0, MAXIMUM_DURATION_MS)
            || !integer(clip.sourceOutMs, clip.sourceInMs + 1, sourceDuration)
            || clip.startMs + clip.sourceOutMs - clip.sourceInMs > maximum) return false;
        ids.add(clip.id);
    }
    const clips = [...value.clips].sort((a, b) => a.startMs - b.startMs);
    return clips.every((clip, i) => i === 0
        || clips[i - 1].startMs + clips[i - 1].sourceOutMs - clips[i - 1].sourceInMs <= clip.startMs);
}
function isCanonicalTimelineSnapshot(snapshot, settings) {
    return isCanonicalTimelineSettings(settings) && onlyKeys(snapshot, ['timelinePositionMs'])
        && integer(snapshot.timelinePositionMs, 0,
            settings.stopTimeMs < 0 ? settings.maxDurationMs : settings.stopTimeMs);
}
module.exports = { MAXIMUM_DURATION_MS, isCanonicalElement,
    isCanonicalTimelineSettings, isCanonicalMediaTrack, isCanonicalTimelineSnapshot };
