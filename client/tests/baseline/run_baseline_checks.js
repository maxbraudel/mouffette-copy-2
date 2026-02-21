#!/usr/bin/env node

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const root = path.resolve(__dirname, '..', '..');
const baselineRoot = path.resolve(__dirname);

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function sha256(filePath) {
  const data = fs.readFileSync(filePath);
  return crypto.createHash('sha256').update(data).digest('hex');
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function checkFixtures() {
  const fixturesDir = path.join(baselineRoot, 'fixtures');
  const files = fs.readdirSync(fixturesDir).filter((f) => f.endsWith('.json')).sort();
  const expected = [
    'mixed_media_scene.json',
    'reconnect_upload_active_scene.json',
    'selection_snap_stress_scene.json',
    'text_heavy_zoom_scene.json'
  ];

  assert(JSON.stringify(files) === JSON.stringify(expected),
    `Fixtures mismatch. Expected ${expected.join(', ')} got ${files.join(', ')}`);

  files.forEach((file) => {
    const fixturePath = path.join(fixturesDir, file);
    const fixture = readJson(fixturePath);
    assert(fixture.id && fixture.id.length > 0, `${file}: missing id`);
    assert(fixture.determinism && fixture.determinism.seed === 424242, `${file}: determinism.seed must be 424242`);
    assert(Array.isArray(fixture.scene.mediaItems), `${file}: scene.mediaItems must be an array`);
    assert(Array.isArray(fixture.scene.screens), `${file}: scene.screens must be an array`);
  });

  return { filesChecked: files.length };
}

function checkReplays() {
  const replayDir = path.join(baselineRoot, 'replays');
  const files = fs.readdirSync(replayDir).filter((f) => f.endsWith('.json')).sort();
  const expected = ['drag_resize.json', 'text_edit_lifecycle.json', 'zoom_pan.json'];

  assert(JSON.stringify(files) === JSON.stringify(expected),
    `Replay specs mismatch. Expected ${expected.join(', ')} got ${files.join(', ')}`);

  const requiredActions = {
    'zoom_pan.json': ['zoom', 'pan'],
    'drag_resize.json': ['drag', 'resize'],
    'text_edit_lifecycle.json': ['text-create', 'text-edit-start', 'text-commit']
  };

  files.forEach((file) => {
    const replay = readJson(path.join(replayDir, file));
    assert(Array.isArray(replay.steps) && replay.steps.length > 0, `${file}: steps required`);
    const actionSet = new Set(replay.steps.map((s) => s.action));
    requiredActions[file].forEach((action) => {
      assert(actionSet.has(action), `${file}: missing required action '${action}'`);
    });
  });

  return { filesChecked: files.length };
}

function checkSnapshots() {
  const manifest = readJson(path.join(baselineRoot, 'snapshots', 'manifest.json'));
  assert(Array.isArray(manifest.assets) && manifest.assets.length === 4, 'snapshot manifest must contain 4 assets');

  manifest.assets.forEach((asset) => {
    const assetPath = path.join(baselineRoot, 'snapshots', asset.file);
    assert(fs.existsSync(assetPath), `missing snapshot asset: ${asset.file}`);
    const actual = sha256(assetPath);
    assert(actual === asset.sha256, `snapshot hash mismatch for ${asset.file}: expected ${asset.sha256}, got ${actual}`);
  });

  return { filesChecked: manifest.assets.length };
}

function checkInvariance() {
  const invarianceDir = path.join(baselineRoot, 'invariance');
  const files = fs.readdirSync(invarianceDir).filter((f) => f.endsWith('.json')).sort();

  let assertions = 0;
  files.forEach((file) => {
    const spec = readJson(path.join(invarianceDir, file));
    assert(Array.isArray(spec.rules) && spec.rules.length > 0, `${file}: rules required`);

    spec.rules.forEach((rule) => {
      const targetPath = path.join(root, rule.file);
      assert(fs.existsSync(targetPath), `${file}: target file not found ${rule.file}`);
      const content = fs.readFileSync(targetPath, 'utf8');

      rule.mustContain.forEach((pattern) => {
        const regex = new RegExp(pattern, 'm');
        assert(regex.test(content), `${file}: pattern '${pattern}' missing in ${rule.file}`);
        assertions += 1;
      });
    });
  });

  return { filesChecked: files.length, assertions };
}

function main() {
  const started = Date.now();
  const results = {
    fixtures: checkFixtures(),
    replays: checkReplays(),
    snapshots: checkSnapshots(),
    invariance: checkInvariance()
  };

  const elapsed = Date.now() - started;
  console.log('[BASELINE] PASS');
  console.log(JSON.stringify({ results, elapsedMs: elapsed }, null, 2));
}

try {
  main();
} catch (error) {
  console.error('[BASELINE] FAIL');
  console.error(error.message);
  process.exit(1);
}
