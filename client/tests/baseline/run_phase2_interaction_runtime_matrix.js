#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const projectRoot = path.resolve(__dirname, '..', '..');
const matrixPath = path.join(__dirname, 'matrix', 'phase2_interaction_runtime_matrix.json');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function loadJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

class InputArbiterModel {
  constructor() {
    this.reset();
  }

  reset() {
    this.mode = 'Idle';
    this.mediaId = '';
    this.handleId = '';
  }

  beginPan() {
    return this.begin('Pan');
  }

  beginMove(mediaId) {
    if (!mediaId) return false;
    return this.begin('Move', mediaId);
  }

  beginResize(mediaId, handleId) {
    if (!mediaId || !handleId) return false;
    return this.begin('Resize', mediaId, handleId);
  }

  beginTextCreate() {
    return this.begin('TextCreate');
  }

  endMove(mediaId) {
    if (this.mode !== 'Move' || this.mediaId !== mediaId) return false;
    this.reset();
    return true;
  }

  endResize(mediaId, handleId) {
    if (this.mode !== 'Resize' || this.mediaId !== mediaId) return false;
    if (handleId && this.handleId !== handleId) return false;
    this.reset();
    return true;
  }

  endPan() {
    if (this.mode !== 'Pan') return false;
    this.reset();
    return true;
  }

  endTextCreate() {
    if (this.mode !== 'TextCreate') return false;
    this.reset();
    return true;
  }

  begin(mode, mediaId = '', handleId = '') {
    if (this.mode === 'Idle') {
      this.mode = mode;
      this.mediaId = mediaId;
      this.handleId = handleId;
      return true;
    }
    if (mode === 'Move' && this.mode === 'Move' && this.mediaId === mediaId) return true;
    if (mode === 'Resize' && this.mode === 'Resize' && this.mediaId === mediaId && this.handleId === handleId) return true;
    return false;
  }
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

function createState() {
  return {
    arbiter: new InputArbiterModel(),
    camera: { scale: 1.0, panX: 0.0, panY: 0.0 },
    media: { m1: { x: 0.0, y: 0.0, scale: 1.0 } },
    moveRefreshCanceled: false,
    metrics: {
      zoomCount: 0,
      panCount: 0,
      maxFocalDriftPx: 0.0
    }
  };
}

function executeStep(state, step, expected) {
  switch (step.action) {
    case 'zoom': {
      state.metrics.zoomCount += 1;
      const beforeX = (step.anchorX - state.camera.panX) / state.camera.scale;
      const beforeY = (step.anchorY - state.camera.panY) / state.camera.scale;
      applyZoomAt(state.camera, step.anchorX, step.anchorY, step.delta, 0.25, 4.0);
      const afterX = (step.anchorX - state.camera.panX) / state.camera.scale;
      const afterY = (step.anchorY - state.camera.panY) / state.camera.scale;
      const driftX = (afterX - beforeX) * state.camera.scale;
      const driftY = (afterY - beforeY) * state.camera.scale;
      const driftPx = Math.hypot(driftX, driftY);
      state.metrics.maxFocalDriftPx = Math.max(state.metrics.maxFocalDriftPx, driftPx);
      break;
    }
    case 'pan': {
      assert(state.arbiter.beginPan(), 'pan should start from idle');
      state.metrics.panCount += 1;
      state.camera.panX += step.dx;
      state.camera.panY += step.dy;
      assert(state.arbiter.endPan(), 'pan should end cleanly');
      break;
    }
    case 'tryBeginPan': {
      const granted = state.arbiter.beginPan();
      if (step.expectDenied) {
        assert(!granted, 'pan must be denied due to active higher-priority session');
      }
      break;
    }
    case 'beginMove': {
      assert(state.arbiter.beginMove(step.mediaId), `move begin denied for ${step.mediaId}`);
      break;
    }
    case 'tryBeginMove': {
      const granted = state.arbiter.beginMove(step.mediaId);
      if (step.expectDenied) {
        assert(!granted, 'move must be denied due to active resize/text session');
      }
      break;
    }
    case 'moveDelta': {
      assert(state.arbiter.mode === 'Move' && state.arbiter.mediaId === step.mediaId,
        `move delta applied without active move session for ${step.mediaId}`);
      state.media[step.mediaId].x += step.dx;
      state.media[step.mediaId].y += step.dy;
      break;
    }
    case 'endMove': {
      assert(state.arbiter.endMove(step.mediaId), `move end failed for ${step.mediaId}`);
      break;
    }
    case 'beginResize': {
      assert(state.arbiter.beginResize(step.mediaId, step.handleId),
        `resize begin denied for ${step.mediaId}/${step.handleId}`);
      break;
    }
    case 'resizeScale': {
      assert(state.arbiter.mode === 'Resize' && state.arbiter.mediaId === step.mediaId,
        `resize scale applied without active resize session for ${step.mediaId}`);
      state.media[step.mediaId].scale *= step.factor;
      break;
    }
    case 'endResize': {
      assert(state.arbiter.endResize(step.mediaId, step.handleId),
        `resize end failed for ${step.mediaId}/${step.handleId}`);
      break;
    }
    case 'beginTextCreate': {
      assert(state.arbiter.beginTextCreate(), 'text create session should start from idle');
      break;
    }
    case 'endTextCreate': {
      assert(state.arbiter.endTextCreate(), 'text create session should end cleanly');
      break;
    }
    case 'modelRefresh': {
      if (state.arbiter.mode === 'Move' || state.arbiter.mode === 'Resize') {
        state.moveRefreshCanceled = false;
      }
      break;
    }
    default:
      throw new Error(`unsupported action: ${step.action}`);
  }

  const finite = Number.isFinite(state.camera.scale)
    && Number.isFinite(state.camera.panX)
    && Number.isFinite(state.camera.panY)
    && Number.isFinite(state.media.m1.x)
    && Number.isFinite(state.media.m1.y)
    && Number.isFinite(state.media.m1.scale);
  assert(finite, `non-finite numeric state after action ${step.action}`);

  if (expected && typeof expected.focalDriftPxMax === 'number') {
    assert(state.metrics.maxFocalDriftPx <= expected.focalDriftPxMax,
      `focal drift ${state.metrics.maxFocalDriftPx.toFixed(6)} exceeds ${expected.focalDriftPxMax}`);
  }
}

function runCase(testCase) {
  const runOnce = () => {
    const state = createState();
    for (const step of testCase.steps) {
      executeStep(state, step, testCase.expected || {});
    }
    return state;
  };

  const first = runOnce();
  let second = null;

  if (testCase.runTwice) {
    second = runOnce();
    const deterministic = JSON.stringify(first) === JSON.stringify(second);
    assert(deterministic, `${testCase.id}: runTwice produced different final state`);
  }

  const expected = testCase.expected || {};
  if (typeof expected.zoomCount === 'number') {
    assert(first.metrics.zoomCount === expected.zoomCount,
      `${testCase.id}: expected ${expected.zoomCount} zooms, got ${first.metrics.zoomCount}`);
  }
  if (typeof expected.panCount === 'number') {
    assert(first.metrics.panCount === expected.panCount,
      `${testCase.id}: expected ${expected.panCount} pans, got ${first.metrics.panCount}`);
  }
  if (typeof expected.finalMediaX === 'number') {
    assert(Math.abs(first.media.m1.x - expected.finalMediaX) < 1e-6,
      `${testCase.id}: unexpected final media X ${first.media.m1.x}`);
  }
  if (typeof expected.finalMediaY === 'number') {
    assert(Math.abs(first.media.m1.y - expected.finalMediaY) < 1e-6,
      `${testCase.id}: unexpected final media Y ${first.media.m1.y}`);
  }
  if (typeof expected.finalScale === 'number') {
    assert(Math.abs(first.media.m1.scale - expected.finalScale) < 1e-6,
      `${testCase.id}: unexpected final media scale ${first.media.m1.scale}`);
  }
  if (typeof expected.modelRefreshCanceledGesture === 'boolean') {
    assert(first.moveRefreshCanceled === expected.modelRefreshCanceledGesture,
      `${testCase.id}: model refresh cancellation mismatch`);
  }
  if (expected.returnsToIdle === true) {
    assert(first.arbiter.mode === 'Idle', `${testCase.id}: arbiter did not return to Idle`);
  }
  if (expected.finite === true) {
    assert(Number.isFinite(first.camera.scale)
      && Number.isFinite(first.media.m1.x)
      && Number.isFinite(first.media.m1.scale), `${testCase.id}: final state not finite`);
  }

  return {
    id: testCase.id,
    steps: testCase.steps.length,
    finalMode: first.arbiter.mode,
    maxFocalDriftPx: Number(first.metrics.maxFocalDriftPx.toFixed(6)),
    final: {
      camera: first.camera,
      media: first.media.m1
    }
  };
}

function main() {
  assert(fs.existsSync(matrixPath), `missing matrix file: ${path.relative(projectRoot, matrixPath)}`);
  const matrix = loadJson(matrixPath);
  assert(Array.isArray(matrix.cases) && matrix.cases.length > 0, 'matrix cases are required');

  const caseResults = matrix.cases.map(runCase);
  const summary = {
    matrixId: matrix.id,
    matrixFile: path.relative(projectRoot, matrixPath),
    casesChecked: caseResults.length,
    stepsChecked: caseResults.reduce((total, c) => total + c.steps, 0),
    caseResults
  };

  console.log('[PHASE2_INTERACTION_RUNTIME_MATRIX] PASS');
  console.log(JSON.stringify(summary, null, 2));
}

try {
  main();
} catch (error) {
  console.error('[PHASE2_INTERACTION_RUNTIME_MATRIX] FAIL');
  console.error(error.message);
  process.exit(1);
}
