#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..', '..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');
const assert = (value, message) => { if (!value) throw new Error(message); };

function main() {
  const sender = read('src/frontend/rendering/canvas/ScreenCanvas.cpp');
  const receiver = read('src/frontend/rendering/remote/RemoteSceneController.cpp');
  const remoteQml = read('resources/qml/RemoteSceneRoot.qml');

  assert(sender.includes('root["renderSchemaVersion"] = 2'), 'schema v2 marker missing');
  for (const field of ['fontPixelSize', 'fontUnderline', 'fontUppercase', 'textOutlineWidthPx', 'z', 'visible']) {
    assert(sender.includes(`m["${field}"]`), `sender field missing: ${field}`);
  }
  for (const legacy of ['fontSize', 'fontBold', 'textBorderWidthPercent', 'uniformScale']) {
    assert(sender.includes(`m["${legacy}"]`), `legacy sender field removed: ${legacy}`);
  }
  assert(receiver.includes('m.value("fontPixelSize").toInt(0)'), 'v2 pixel size is not read');
  assert(receiver.includes('TextRenderMetrics::effectiveFontPixelSize'), 'v1 font fallback is missing');
  assert(receiver.includes('m.value("visible").toBool(true)'), 'visibility fallback is missing');
  assert(receiver.includes('mediaArray.size() - idx'), 'v1 topmost-first z fallback is missing');
  assert(remoteQml.includes('media.z || 0'), 'explicit z is not applied');
  assert(remoteQml.includes('media.contentVisible !== false'), 'visibility is not applied');

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
