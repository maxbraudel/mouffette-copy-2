#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const projectRoot = path.resolve(__dirname, '..', '..');
const replayPath = path.join(__dirname, 'replays', 'zoom_pan.json');
const qmlPath = path.join(projectRoot, 'resources', 'qml', 'CanvasRoot.qml');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function loadJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function applyZoomAt(state, anchorX, anchorY, factor, minScale, maxScale) {
  if (!Number.isFinite(factor) || factor <= 0) return;
  const oldScale = state.scale;
  const nextScale = Math.max(minScale, Math.min(maxScale, oldScale * factor));
  if (!Number.isFinite(nextScale)) return;

  const worldX = (anchorX - state.panX) / oldScale;
  const worldY = (anchorY - state.panY) / oldScale;

  state.scale = nextScale;
  state.panX = anchorX - worldX * nextScale;
  state.panY = anchorY - worldY * nextScale;
}

function checkQmlSurface() {
  const qml = fs.readFileSync(qmlPath, 'utf8');
  const requiredTokens = [
    'function applyZoomAt',
    'DragHandler',
    'WheelHandler',
    'PinchHandler',
    'isZoomModifier',
    'wheelDeltaY',
    'wheelDeltaX',
    'recenterView'
  ];

  for (const token of requiredTokens) {
    assert(qml.includes(token), `QML interaction token missing: ${token}`);
  }

  return { checkedTokens: requiredTokens.length };
}

function runReplayDriftCheck() {
  const replay = loadJson(replayPath);
  const tolerance = replay.expected.focalDriftPxMax;
  const state = { scale: 1.0, panX: 0.0, panY: 0.0 };
  const minScale = 0.25;
  const maxScale = 4.0;

  let maxDrift = 0.0;
  let zoomCount = 0;
  let panCount = 0;

  for (const step of replay.steps) {
    if (step.action === 'zoom') {
      zoomCount += 1;
      const anchorX = step.anchorX;
      const anchorY = step.anchorY;
      const worldBeforeX = (anchorX - state.panX) / state.scale;
      const worldBeforeY = (anchorY - state.panY) / state.scale;

      applyZoomAt(state, anchorX, anchorY, step.delta, minScale, maxScale);

      const worldAfterX = (anchorX - state.panX) / state.scale;
      const worldAfterY = (anchorY - state.panY) / state.scale;
      const driftX = (worldAfterX - worldBeforeX) * state.scale;
      const driftY = (worldAfterY - worldBeforeY) * state.scale;
      const driftPx = Math.hypot(driftX, driftY);
      maxDrift = Math.max(maxDrift, driftPx);
    } else if (step.action === 'pan') {
      panCount += 1;
      state.panX += step.dx;
      state.panY += step.dy;
    }
  }

  assert(Number.isFinite(state.scale), 'camera scale became non-finite');
  assert(maxDrift <= tolerance, `focal drift ${maxDrift.toFixed(6)} exceeds tolerance ${tolerance}`);

  return {
    zoomCount,
    panCount,
    maxDriftPx: Number(maxDrift.toFixed(6)),
    tolerancePx: tolerance,
    finalState: state
  };
}

function main() {
  const qmlCheck = checkQmlSurface();
  const replayCheck = runReplayDriftCheck();

  console.log('[PHASE2_PARITY] PASS');
  console.log(JSON.stringify({ qmlCheck, replayCheck }, null, 2));
}

try {
  main();
} catch (error) {
  console.error('[PHASE2_PARITY] FAIL');
  console.error(error.message);
  process.exit(1);
}
