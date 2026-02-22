#!/usr/bin/env node

const { spawnSync } = require('child_process');
const path = require('path');

const projectRoot = path.resolve(__dirname, '..', '..');

function main() {
  const result = spawnSync('node', ['tests/baseline/run_phase2_interaction_runtime_matrix.js'], {
    cwd: projectRoot,
    encoding: 'utf8'
  });

  if (result.stdout) process.stdout.write(result.stdout);
  if (result.stderr) process.stderr.write(result.stderr);

  if (result.status !== 0) {
    console.error('[PHASE2_PARITY] FAIL');
    process.exit(result.status || 1);
  }

  console.log('[PHASE2_PARITY] PASS');
}

main();
