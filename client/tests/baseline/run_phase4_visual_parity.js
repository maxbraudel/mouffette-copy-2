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
    'resources/qml/BaseMediaItem.qml',
    'resources/qml/ImageItem.qml',
    'resources/qml/VideoItem.qml',
    'resources/qml/TextItem.qml',
    'resources/qml/MediaVisual.qml',
    'resources/qml/RemoteSceneRoot.qml',
    'resources/qml/CanvasRoot.qml'
  ];

  for (const file of required) {
    assert(fs.existsSync(path.join(projectRoot, file)), `missing required file: ${file}`);
  }

  return { filesChecked: required.length };
}

function checkResourceRegistration() {
  const qrc = readText('resources/resources.qrc');
  const requiredAliases = [
    'BaseMediaItem.qml', 'ImageItem.qml', 'VideoItem.qml', 'TextItem.qml',
    'MediaVisual.qml', 'RemoteSceneRoot.qml'
  ];
  for (const alias of requiredAliases) {
    assert(qrc.includes(`alias="${alias}"`), `resources.qrc missing alias ${alias}`);
  }
  return { aliasesChecked: requiredAliases.length };
}

function checkCanvasBindings() {
  const qmlPath = path.join(projectRoot, 'resources/qml/CanvasRoot.qml');
  assert(fs.existsSync(qmlPath), 'CanvasRoot.qml must exist');
  const qml = fs.readFileSync(qmlPath, 'utf8');
  const mediaInteractions = readText('resources/qml/MediaInteractionHandlers.qml');
  const stat = fs.statSync(qmlPath);
  assert(stat.size > 0, 'CanvasRoot.qml must not be empty');
  assert(mediaInteractions.includes('coord.tryBeginMove('),
    'Media interactions must route move ownership through InputCoordinator');
  assert(qml.includes('inputLayer.inputCoordinator.tryBeginPanAt('),
    'CanvasRoot pan ownership must route through InputCoordinator');
  assert(qml.includes('inputLayer.inputCoordinator.tryBeginTextCreateAt('),
    'CanvasRoot text-create ownership must route through InputCoordinator');
  return { fileExists: true, bytes: stat.size, inputCoordinatorBindings: 3 };
}

function checkCoordinatorFlag() {
  const inputLayer = readText('resources/qml/InputLayer.qml');
  assert(inputLayer.includes('readonly property alias inputCoordinator: coordinator'),
    'InputLayer must expose its per-canvas InputCoordinator');
  assert(inputLayer.includes('function assertInvariants(stage)'),
    'InputCoordinator must retain runtime ownership guards');
  return { coordinatorChecks: 2 };
}

function checkMediaDelegates() {
  const baseItem = readText('resources/qml/BaseMediaItem.qml');
  const imageItem = readText('resources/qml/ImageItem.qml');
  const videoItem = readText('resources/qml/VideoItem.qml');
  const textItem = readText('resources/qml/TextItem.qml');
  const mediaVisual = readText('resources/qml/MediaVisual.qml');
  const canvasRoot = readText('resources/qml/CanvasRoot.qml');
  const remoteRoot = readText('resources/qml/RemoteSceneRoot.qml');
  const remoteController = readText('src/frontend/rendering/remote/RemoteSceneController.cpp');
  const outline = readText('src/frontend/rendering/canvas/TextOutlineItem.cpp');
  const sessionController = readText('src/backend/controllers/CanvasSessionController.cpp');

  assert(baseItem.includes('signal selectRequested'), 'BaseMediaItem must provide selectRequested contract');
  assert(baseItem.includes('onPressed'), 'BaseMediaItem must own primary press handling');
  assert(baseItem.includes('root.primaryPressed(root.mediaId, additive)'),
    'BaseMediaItem press path must emit primaryPressed intent');
  assert(!baseItem.includes('root.selectRequested(root.mediaId, additive)'),
    'BaseMediaItem primary press must not directly select; selection must be coordinator-routed');
  assert(imageItem.includes('BaseMediaItem {'), 'ImageItem must derive from BaseMediaItem');
  assert(videoItem.includes('BaseMediaItem {'), 'VideoItem must derive from BaseMediaItem');
  assert(textItem.includes('BaseMediaItem {'), 'TextItem must derive from BaseMediaItem');
  assert(imageItem.includes('Image {'), 'ImageItem must contain Image element');
  assert(videoItem.includes('VideoOutput {'), 'VideoItem must contain VideoOutput element');
  assert(textItem.includes('TextEdit {'), 'TextItem must use one TextEdit for display and edit');
  assert(textItem.includes('TextOutlineItem {'), 'TextItem must use the retained text outline renderer');
  assert(!textItem.includes('PathSvg') && !textItem.includes('Shape {'),
    'Live text outlines must not rebuild and parse a document-sized SVG');
  assert(textItem.includes('source: textDisplayNode'),
    'Text outline must consume the live editor document directly');
  assert(outline.includes('line.glyphRuns()'),
    'Text outline must use the existing document layout');
  assert(!textItem.includes('visible: !root.editing'), 'Text outline must stay present while editing');
  assert(textItem.includes('anchors.margins: root.textInset'),
    'The border safety margin must surround the text on all sides');
  assert(textItem.includes('onPrimaryDoubleClicked'), 'TextItem must support double-click edit start via base signal');
  assert(textItem.includes('textCommitRequested'), 'TextItem must emit text commit signal');
  assert(canvasRoot.includes('MediaVisual {'), 'Local canvas must instantiate the shared media visual');
  assert(remoteRoot.includes('MediaVisual {'), 'Remote canvas must instantiate the shared media visual');
  assert(mediaVisual.includes('ImageItem {') && mediaVisual.includes('VideoItem {') && mediaVisual.includes('TextItem {'),
    'Shared media visual must own all three visual delegates');
  assert(remoteController.includes('new QQuickWidget'), 'Remote renderer must use QQuickWidget');
  assert(!remoteController.includes('QGraphicsTextItem') && !remoteController.includes('QGraphicsPixmapItem'),
    'Remote renderer must not retain a QGraphics drawing path');
  assert(!sessionController.includes('LegacyCanvasHost::create'),
    'Qt Quick initialization must not silently fall back to a second visible renderer');

  return { delegateChecks: 24 };
}

function checkDtoBridge() {
  const requiredFiles = [
    'src/frontend/rendering/canvas/QuickCanvasController.cpp',
    'src/frontend/rendering/canvas/QuickCanvasHost.cpp',
    'src/frontend/rendering/canvas/QuickCanvasViewAdapter.cpp',
    'src/frontend/rendering/canvas/LegacySceneMirror.cpp'
  ];

  for (const file of requiredFiles) {
    assert(fs.existsSync(path.join(projectRoot, file)), `missing bridge file: ${file}`);
  }

  return { filesChecked: requiredFiles.length };
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
    coordinatorFlag: checkCoordinatorFlag(),
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
