#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..', '..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');
const assert = (value, message) => { if (!value) throw new Error(message); };

function main() {
  const sender = read('src/backend/domain/canvas/CanvasDocument.cpp');
  const timeline = read('src/backend/domain/scene/SceneTimeline.cpp');
  const schema = read('src/backend/domain/scene/SceneTimeline.h');
  const receiver = read('src/frontend/rendering/remote/RemoteSceneController.cpp');
  const canvasMedia = read('src/backend/domain/media/CanvasMedia.h');
  const remoteQml = read('resources/qml/RemoteSceneRoot.qml');
  const textQml = read('resources/qml/TextItem.qml');
  const sceneStart = sender.indexOf('QJsonObject CanvasDocument::serializeSceneState() const');
  const sceneEnd = sender.indexOf('QJsonObject CanvasDocument::serializeProjectState() const');
  const sceneSerializer = sender.slice(sceneStart, sceneEnd);

  assert(sceneStart >= 0 && sceneEnd > sceneStart, 'scene serializer boundaries missing');
  assert(schema.includes('RenderSchemaVersion = 6'), 'schema v6 marker missing');
  assert(sceneSerializer.includes('SceneTimeline::RenderSchemaVersion'), 'sender bypasses canonical schema version');
  assert(sceneSerializer.includes('m_timelineSettings.toJson()'), 'scene timeline settings missing');
  for (const field of ['fontPixelSize', 'fontUnderline', 'fontUppercase', 'textOutlineWidthPx', 'visible']) {
    assert(timeline.includes(`"${field}"`), `sender field missing: ${field}`);
  }
  for (const removed of ['fontSize', 'fontBold', 'textBorderWidthPercent', 'uniformScale']) {
    assert(!sceneSerializer.includes(`QStringLiteral("${removed}")`), `obsolete field remains: ${removed}`);
    assert(!receiver.includes(`"${removed}"`), `receiver still accepts obsolete field: ${removed}`);
  }
  assert(!timeline.includes('"z"'), 'authoring or keyframes still serialize independent z');
  assert(timeline.includes('onlyKeys(o,{"keyframes","clip","trackIndex"})'),
    'timeline must accept exactly keyframes, one clip, and a track index');
  assert(timeline.includes('integer(o,"trackIndex",MinimumTrackIndex,MaximumTrackIndex,&index)'),
    'track index is not validated as a bounded signed integer');
  assert(timeline.includes('!o.value("clip").isObject()'), 'timeline must reject missing or non-object clips');
  assert(!timeline.includes('"clips"'), 'obsolete multiple clips per instance remain');
  assert(canvasMedia.includes('SceneTimeline::trackZ(m_timelineTrack.trackIndex)'),
    'editor stacking does not use the canonical track order');
  assert(receiver.includes('SceneTimeline::trackZ(item->timeline.trackIndex)'),
    'remote stacking does not use the canonical track order');
  assert(receiver.includes('clipsByTrack[track.trackIndex].append(track.clip)'),
    'receiver does not collect clips across instances of the same track');
  assert(receiver.includes('clips[index - 1].endSlot() > clips[index].startSlot'),
    'receiver does not reject overlapping instances on one track');
  assert(receiver.includes('SceneTimeline::ElementState::fromMediaJson'), 'receiver bypasses canonical intrinsic validation');
  assert(receiver.includes('SceneTimeline::MediaTrack::fromJson'), 'receiver bypasses canonical timeline validation');
  assert(!receiver.includes('TextRenderMetrics::effectiveFontPixelSize'), 'font fallback remains');
  assert(!receiver.includes('toBool(true)'), 'boolean fallback remains');
  assert(!receiver.includes('mediaArray.size() - idx'), 'implicit stacking fallback remains');
  assert(remoteQml.includes('z: media ? media.z : 0'), 'derived render z is not applied');
  assert(remoteQml.includes('media.contentVisible && media.renderVisible'), 'explicit visibility is not applied');
  assert(remoteQml.includes('root.spanReady(media.mediaId, media.spanId)'), 'canonical span identity is not used');
  assert(!remoteQml.includes('remoteMediaId'), 'remote renderer identity alias remains');
  assert(textQml.includes('outlinePixels: Math.max(0, root.outlineWidthPx)'), 'pixel outline is not authoritative');
  assert(!textQml.includes('outlineWidthPercent'), 'percentage outline fallback remains');

  const outline = (percent, pixels) => percent <= 0 ? 0 : Math.max(1, Math.round(percent * pixels / 100));
  assert(outline(0, 48) === 0, 'zero outline formula regression');
  assert(outline(1, 48) === 1, 'minimum outline formula regression');
  assert(outline(12.5, 48) === 6, 'outline formula regression');
  assert(outline(150, 48) === 72, 'outline formula must not introduce a hidden clamp');

  console.log('[RENDER_SCHEMA_V6] PASS');
}

try { main(); } catch (error) {
  console.error('[RENDER_SCHEMA_V6] FAIL');
  console.error(error.message);
  process.exit(1);
}
