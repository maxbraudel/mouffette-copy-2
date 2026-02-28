#!/usr/bin/env node

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function makeState() {
  return {
    mode: 'idle',
    ownerId: '',
    primaryGestureActive: false,
    primaryOwnerKind: 'none',
    pressTargetKind: 'unknown',
    pressTargetMediaId: '',
    lastPrimaryPressKind: 'unknown',
    lastPrimaryPressMediaId: '',
  };
}

function invariants(state) {
  assert(!(state.mode === 'idle' && state.ownerId !== ''), 'idle mode with non-empty ownerId');
  assert(!(state.mode !== 'idle' && state.mode !== 'pan' && state.ownerId === ''), 'non-canvas active mode without ownerId');
  assert(!(state.primaryGestureActive && state.primaryOwnerKind === 'none'), 'active gesture without owner kind');
  assert(!(!state.primaryGestureActive && state.primaryOwnerKind !== 'none'), 'inactive gesture with owner kind');
}

function beginPrimaryGesture(state) {
  state.primaryGestureActive = true;
  state.lastPrimaryPressKind = 'unknown';
  state.lastPrimaryPressMediaId = '';

  const draw = Math.random();
  if (draw < 0.2) {
    state.primaryOwnerKind = 'handle';
    state.pressTargetKind = 'handle';
    state.pressTargetMediaId = 'media-' + (1 + Math.floor(Math.random() * 4));
  } else if (draw < 0.8) {
    state.primaryOwnerKind = 'media';
    state.pressTargetKind = 'media';
    state.pressTargetMediaId = 'media-' + (1 + Math.floor(Math.random() * 4));
  } else {
    state.primaryOwnerKind = 'canvas';
    state.pressTargetKind = 'canvas';
    state.pressTargetMediaId = '';
  }
}

function endPrimaryGesture(state) {
  state.lastPrimaryPressKind = state.pressTargetKind;
  state.lastPrimaryPressMediaId = state.pressTargetMediaId;
  state.primaryGestureActive = false;
  state.primaryOwnerKind = 'none';
  state.pressTargetKind = 'unknown';
  state.pressTargetMediaId = '';
}

function beginMode(state, mode, ownerId) {
  if (state.mode !== 'idle' && !(state.mode === mode && state.ownerId === ownerId)) {
    return false;
  }
  state.mode = mode;
  state.ownerId = ownerId || '';
  return true;
}

function endMode(state, mode, ownerId) {
  if (state.mode !== mode)
    return;
  if (state.ownerId !== '' && ownerId !== '' && state.ownerId !== ownerId)
    return;
  state.mode = 'idle';
  state.ownerId = '';
}

function forceReset(state) {
  state.mode = 'idle';
  state.ownerId = '';
  state.pressTargetKind = 'unknown';
  state.pressTargetMediaId = '';
  state.lastPrimaryPressKind = 'unknown';
  state.lastPrimaryPressMediaId = '';
  state.primaryGestureActive = false;
  state.primaryOwnerKind = 'none';
}

function randomStep(state) {
  const op = Math.floor(Math.random() * 10);
  const media = 'media-' + (1 + Math.floor(Math.random() * 4));

  switch (op) {
    case 0:
      if (!state.primaryGestureActive) beginPrimaryGesture(state);
      break;
    case 1:
      if (state.primaryGestureActive) endPrimaryGesture(state);
      break;
    case 2:
      beginMode(state, 'move', media);
      break;
    case 3:
      beginMode(state, 'resize', media);
      break;
    case 4:
      beginMode(state, 'pan', 'canvas');
      break;
    case 5:
      endMode(state, 'move', media);
      break;
    case 6:
      endMode(state, 'resize', media);
      break;
    case 7:
      endMode(state, 'pan', 'canvas');
      break;
    case 8:
      beginMode(state, 'text', 'canvas');
      endMode(state, 'text', 'canvas');
      break;
    case 9:
      if (Math.random() < 0.05) forceReset(state);
      break;
    default:
      break;
  }
}

function run() {
  const seeds = 120;
  const stepsPerSeed = 500;

  for (let seedIndex = 0; seedIndex < seeds; seedIndex += 1) {
    const state = makeState();
    for (let step = 0; step < stepsPerSeed; step += 1) {
      randomStep(state);
      invariants(state);
    }
  }

  console.log('[PHASE8_RANDOMIZED_INPUT_SEQUENCES] PASS');
}

try {
  run();
} catch (err) {
  console.error('[PHASE8_RANDOMIZED_INPUT_SEQUENCES] FAIL');
  console.error(err && err.stack ? err.stack : err);
  process.exit(1);
}
