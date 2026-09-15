#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..', '..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');
const assert = (value, message) => { if (!value) throw new Error(message); };

function main() {
  const sender = read('src/backend/domain/canvas/CanvasDocument.cpp');
  const receiver = read('src/frontend/rendering/remote/RemoteSceneController.cpp');
  const remoteQml = read('resources/qml/RemoteSceneRoot.qml');
  const textQml = read('resources/qml/TextItem.qml');
  const sceneStart = sender.indexOf('QJsonObject CanvasDocument::serializeSceneState() const');
  const sceneEnd = sender.indexOf('QJsonObject CanvasDocument::serializeProjectState() const');
  const sceneSerializer = sender.slice(sceneStart, sceneEnd);

  assert(sceneStart >= 0 && sceneEnd > sceneStart, 'scene serializer boundaries missing');
  assert(sceneSerializer.includes('QStringLiteral("renderSchemaVersion"), 2'), 'schema v2 marker missing');
  for (const field of ['fontPixelSize', 'fontUnderline', 'fontUppercase', 'textOutlineWidthPx', 'z', 'visible']) {
    assert(sceneSerializer.includes(`QStringLiteral("${field}")`), `sender field missing: ${field}`);
  }
  for (const removed of ['fontSize', 'fontBold', 'textBorderWidthPercent', 'uniformScale']) {
    assert(!sceneSerializer.includes(`QStringLiteral("${removed}")`), `obsolete field remains: ${removed}`);
    assert(!receiver.includes(`"${removed}"`), `receiver still accepts obsolete field: ${removed}`);
  }
  assert(receiver.includes('readBoundedInteger(mediaObject, "fontPixelSize", 1, 4096'), 'pixel size is not required');
  assert(receiver.includes('readBoundedInteger(mediaObject, "fontWeight", 1, 900'), 'font weight is not required');
  assert(!receiver.includes('TextRenderMetrics::effectiveFontPixelSize'), 'font fallback remains');
  assert(!receiver.includes('toBool(true)'), 'boolean fallback remains');
  assert(!receiver.includes('mediaArray.size() - idx'), 'implicit stacking fallback remains');
  assert(remoteQml.includes('z: media ? media.z : 0'), 'explicit z is not applied');
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

  console.log('[RENDER_SCHEMA_V2] PASS');
}

try { main(); } catch (error) {
  console.error('[RENDER_SCHEMA_V2] FAIL');
  console.error(error.message);
  process.exit(1);
}
