#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

function parseArgs(argv) {
  const out = {
    quickLog: '/tmp/mouffette_phase4_audit_quick_perf.log',
    legacyLog: '/tmp/mouffette_phase4_audit_legacy.log'
  };

  for (let index = 2; index < argv.length; index += 1) {
    const arg = argv[index];
    if (arg === '--quick-log' && argv[index + 1]) {
      out.quickLog = argv[index + 1];
      index += 1;
      continue;
    }
    if (arg === '--legacy-log' && argv[index + 1]) {
      out.legacyLog = argv[index + 1];
      index += 1;
      continue;
    }
  }

  return out;
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function percentile(values, p) {
  if (values.length === 0) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const rank = Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1));
  return sorted[rank];
}

function parseCanvasPerf(logText) {
  const lines = logText.split(/\r?\n/).filter((line) => line.includes('[CanvasPerf]'));
  const windows = [];

  for (const line of lines) {
    const zoom = Number((line.match(/zoomEvents\s+(\d+)/) || [])[1] || 0);
    const relayouts = Number((line.match(/relayouts\s+(\d+)/) || [])[1] || 0);
    const fullRelayouts = Number((line.match(/fullRelayouts\s+(\d+)/) || [])[1] || 0);

    windows.push({
      zoomEvents: zoom,
      relayouts,
      fullRelayouts,
      relayoutPerZoom: zoom > 0 ? relayouts / zoom : null,
      fullRelayoutPerZoom: zoom > 0 ? fullRelayouts / zoom : null
    });
  }

  return windows;
}

function summarizeWindows(windows) {
  const zoomWindows = windows.filter((windowItem) => windowItem.zoomEvents > 0);
  const relayoutRatios = zoomWindows.map((windowItem) => windowItem.relayoutPerZoom);
  const fullRelayoutRatios = zoomWindows.map((windowItem) => windowItem.fullRelayoutPerZoom);

  return {
    windows: windows.length,
    zoomWindows: zoomWindows.length,
    totalZoomEvents: windows.reduce((acc, value) => acc + value.zoomEvents, 0),
    totalRelayouts: windows.reduce((acc, value) => acc + value.relayouts, 0),
    p95RelayoutPerZoom: percentile(relayoutRatios, 95),
    p95FullRelayoutPerZoom: percentile(fullRelayoutRatios, 95)
  };
}

function main() {
  const args = parseArgs(process.argv);

  assert(fs.existsSync(args.quickLog), `quick log not found: ${args.quickLog}`);
  assert(fs.existsSync(args.legacyLog), `legacy log not found: ${args.legacyLog}`);

  const quickWindows = parseCanvasPerf(fs.readFileSync(path.resolve(args.quickLog), 'utf8'));
  const legacyWindows = parseCanvasPerf(fs.readFileSync(path.resolve(args.legacyLog), 'utf8'));

  const quick = summarizeWindows(quickWindows);
  const legacy = summarizeWindows(legacyWindows);

  assert(quick.windows > 0, 'quick log has no CanvasPerf windows');
  assert(legacy.windows > 0, 'legacy log has no CanvasPerf windows');
  assert(quick.zoomWindows > 0 && legacy.zoomWindows > 0,
    'zoomEvents are zero in one or both logs. Re-run both paths while executing the text-heavy zoom replay manually.');

  const delta = {
    p95RelayoutPerZoomDelta: quick.p95RelayoutPerZoom - legacy.p95RelayoutPerZoom,
    p95FullRelayoutPerZoomDelta: quick.p95FullRelayoutPerZoom - legacy.p95FullRelayoutPerZoom
  };

  const improved = delta.p95RelayoutPerZoomDelta < 0 || delta.p95FullRelayoutPerZoomDelta < 0;
  assert(improved, 'no improvement detected in relayout-per-zoom p95 proxy');

  console.log('[PHASE4_PERF_DELTA] PASS');
  console.log(JSON.stringify({ quick, legacy, delta }, null, 2));
}

try {
  main();
} catch (error) {
  console.error('[PHASE4_PERF_DELTA] FAIL');
  console.error(error.message);
  process.exit(1);
}
