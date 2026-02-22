#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const projectRoot = path.resolve(__dirname, '..', '..');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function readText(relativePath) {
  const filePath = path.join(projectRoot, relativePath);
  return fs.readFileSync(filePath, 'utf8');
}

function checkCanvasRoot() {
  const qml = readText('resources/qml/CanvasRoot.qml');
  const required = [
    'InteractionArbiter {',
    'id: interactionArbiter',
    'mediaDrag.active',
    'canStartInteraction("move", mediaDelegate.currentMediaId)',
    '!selectionChrome.handlePriorityActive',
    'panDrag.active',
    'canStartInteraction(mode, ownerId)',
    'interactionArbiter.begin(mode, ownerId)',
    'interactionArbiter.end(mode, ownerId)'
  ];

  required.forEach((token) => {
    assert(qml.includes(token), `CanvasRoot token missing: ${token}`);
  });

  return { checkedTokens: required.length };
}

function checkSelectionChrome() {
  const qml = readText('resources/qml/SelectionChrome.qml');
  const required = [
    'property int handleHoverCount: 0',
    'readonly property bool pointerOnHandle: handleHoverCount > 0',
    'readonly property bool handlePriorityActive: interacting || pointerOnHandle',
    'HoverHandler {',
    'root.handleHoverCount += 1'
  ];

  required.forEach((token) => {
    assert(qml.includes(token), `SelectionChrome token missing: ${token}`);
  });

  return { checkedTokens: required.length };
}

function checkControllerOwnership() {
  const cpp = readText('src/frontend/rendering/canvas/QuickCanvasController.cpp');
  assert(!cpp.includes('case QEvent::MouseButtonPress:'),
    'QuickCanvasController must not own text-create mouse press semantics');
  assert(cpp.includes('SIGNAL(textCreateRequested(double,double))'),
    'QuickCanvasController must still receive text create requests from QML signal');

  return { checkedTokens: 2 };
}

function checkQrc() {
  const qrc = readText('resources/resources.qrc');
  assert(qrc.includes('<file alias="InteractionArbiter.qml">qml/InteractionArbiter.qml</file>'),
    'resources.qrc must include InteractionArbiter.qml');
  return { checkedTokens: 1 };
}

function main() {
  const result = {
    canvasRoot: checkCanvasRoot(),
    selectionChrome: checkSelectionChrome(),
    controllerOwnership: checkControllerOwnership(),
    qrc: checkQrc()
  };

  console.log('[PHASE2_INTERACTION_OWNERSHIP] PASS');
  console.log(JSON.stringify(result, null, 2));
}

try {
  main();
} catch (error) {
  console.error('[PHASE2_INTERACTION_OWNERSHIP] FAIL');
  console.error(error.message);
  process.exit(1);
}
