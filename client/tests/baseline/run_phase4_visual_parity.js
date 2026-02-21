#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const projectRoot = path.resolve(__dirname, '..', '..');

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function readText(relativePath) {
  return fs.readFileSync(path.join(projectRoot, relativePath), 'utf8');
}

function checkFilesPresent() {
  const required = [
    'resources/qml/ImageItem.qml',
    'resources/qml/VideoItem.qml',
    'resources/qml/TextItem.qml',
    'resources/qml/CanvasRoot.qml'
  ];

  for (const file of required) {
    assert(fs.existsSync(path.join(projectRoot, file)), `missing required file: ${file}`);
  }

  return { filesChecked: required.length };
}

function checkResourceRegistration() {
  const qrc = readText('resources/resources.qrc');
  const requiredAliases = ['ImageItem.qml', 'VideoItem.qml', 'TextItem.qml'];
  for (const alias of requiredAliases) {
    assert(qrc.includes(`alias="${alias}"`), `resources.qrc missing alias ${alias}`);
  }
  return { aliasesChecked: requiredAliases.length };
}

function checkCanvasBindings() {
  const qml = readText('resources/qml/CanvasRoot.qml');
  const requiredTokens = [
    'property var mediaModel',
    'signal mediaSelectRequested',
    'signal textCommitRequested',
    'ImageItem {',
    'VideoItem {',
    'TextItem {',
    'mediaZ: parent.media.z',
    'selected: !!parent.media.selected',
    'onTextCommitRequested'
  ];

  for (const token of requiredTokens) {
    assert(qml.includes(token), `CanvasRoot missing token: ${token}`);
  }

  return { tokensChecked: requiredTokens.length };
}

function checkMediaDelegates() {
  const imageItem = readText('resources/qml/ImageItem.qml');
  const videoItem = readText('resources/qml/VideoItem.qml');
  const textItem = readText('resources/qml/TextItem.qml');

  assert(imageItem.includes('Image {'), 'ImageItem must contain Image element');
  assert(videoItem.includes('VideoOutput {'), 'VideoItem must contain VideoOutput element');
  assert(textItem.includes('Text {'), 'TextItem must contain Text element');
  assert(textItem.includes('TextEdit {'), 'TextItem must contain TextEdit for edit contract');
  assert(textItem.includes('onDoubleClicked'), 'TextItem must support double-click edit start');
  assert(textItem.includes('textCommitRequested'), 'TextItem must emit text commit signal');

  return { delegateChecks: 6 };
}

function checkDtoBridge() {
  const controller = readText('src/frontend/rendering/canvas/QuickCanvasController.cpp');
  const host = readText('src/frontend/rendering/canvas/QuickCanvasHost.cpp');

  const controllerTokens = [
    'setMediaScene(',
    'syncMediaModelFromScene(',
    'mediaEntry.insert(QStringLiteral("x"), sceneRect.x())',
    'mediaEntry.insert(QStringLiteral("y"), sceneRect.y())',
    'mediaEntry.insert(QStringLiteral("width"), sceneRect.width())',
    'mediaEntry.insert(QStringLiteral("height"), sceneRect.height())',
    'mediaEntry.insert(QStringLiteral("z"), media->zValue())',
    'mediaEntry.insert(QStringLiteral("selected"), media->isSelected())',
    'handleTextCommitRequested('
  ];
  for (const token of controllerTokens) {
    assert(controller.includes(token), `QuickCanvasController missing token: ${token}`);
  }

  assert(host.includes('ScreenCanvas* mediaCanvas = new ScreenCanvas'), 'QuickCanvasHost must keep hidden ScreenCanvas model source');
  assert(host.includes('controller->setMediaScene(mediaCanvas->scene())'), 'QuickCanvasHost must bind media scene to controller');

  return { controllerTokens: controllerTokens.length, hostChecks: 2 };
}

function checkBaselineSnapshotsPresence() {
  const manifestPath = path.join(projectRoot, 'tests/baseline/snapshots/manifest.json');
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  assert(Array.isArray(manifest.assets) && manifest.assets.length >= 4, 'snapshot manifest must include baseline assets');

  for (const asset of manifest.assets) {
    const filePath = path.join(projectRoot, 'tests/baseline/snapshots', asset.file);
    assert(fs.existsSync(filePath), `missing baseline snapshot: ${asset.file}`);
  }

  return { snapshotAssets: manifest.assets.length };
}

function main() {
  const results = {
    files: checkFilesPresent(),
    resources: checkResourceRegistration(),
    canvas: checkCanvasBindings(),
    delegates: checkMediaDelegates(),
    dtoBridge: checkDtoBridge(),
    snapshots: checkBaselineSnapshotsPresence()
  };

  console.log('[PHASE4_VISUAL_PARITY] PASS');
  console.log(JSON.stringify(results, null, 2));
}

try {
  main();
} catch (error) {
  console.error('[PHASE4_VISUAL_PARITY] FAIL');
  console.error(error.message);
  process.exit(1);
}
