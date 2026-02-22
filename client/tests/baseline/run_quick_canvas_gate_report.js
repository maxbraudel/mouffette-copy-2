#!/usr/bin/env node

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const projectRoot = path.resolve(__dirname, '..', '..');

function parseArgs(argv) {
  const args = {
    quickLog: path.join(__dirname, 'fixtures', 'perf_logs', 'phase4_quick_auto_targeted.log'),
    legacyLog: path.join(__dirname, 'fixtures', 'perf_logs', 'phase4_legacy_auto.log'),
    out: path.join(__dirname, 'artifacts', 'quick_canvas_gate_report.json')
  };

  for (let i = 2; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--quick-log' && argv[i + 1]) {
      args.quickLog = argv[i + 1];
      i += 1;
      continue;
    }
    if (arg === '--legacy-log' && argv[i + 1]) {
      args.legacyLog = argv[i + 1];
      i += 1;
      continue;
    }
    if (arg === '--out' && argv[i + 1]) {
      args.out = argv[i + 1];
      i += 1;
    }
  }

  return args;
}

function runCommand(command, commandArgs, cwd) {
  const result = spawnSync(command, commandArgs, {
    cwd,
    encoding: 'utf8'
  });

  return {
    command: `${command} ${commandArgs.join(' ')}`,
    exitCode: result.status,
    stdout: result.stdout || '',
    stderr: result.stderr || ''
  };
}

function getGitCommit() {
  const result = spawnSync('git', ['rev-parse', 'HEAD'], {
    cwd: projectRoot,
    encoding: 'utf8'
  });

  if (result.status !== 0) return 'unknown';
  return (result.stdout || '').trim() || 'unknown';
}

function main() {
  const args = parseArgs(process.argv);
  const absoluteOut = path.resolve(projectRoot, args.out);
  const absoluteQuickLog = path.resolve(projectRoot, args.quickLog);
  const absoluteLegacyLog = path.resolve(projectRoot, args.legacyLog);

  const checks = [
    runCommand('bash', ['./tools/check_architecture_boundaries.sh'], projectRoot),
    runCommand('node', ['tests/baseline/run_baseline_checks.js'], projectRoot),
    runCommand('node', ['tests/baseline/run_phase2_interaction_runtime_matrix.js'], projectRoot),
    runCommand('node', ['tests/baseline/run_phase7_integration_hardening.js'], projectRoot),
    runCommand('node', [
      'tests/baseline/run_phase4_perf_delta.js',
      '--quick-log', absoluteQuickLog,
      '--legacy-log', absoluteLegacyLog
    ], projectRoot)
  ];

  const report = {
    generatedAt: new Date().toISOString(),
    gitCommit: getGitCommit(),
    inputs: {
      quickLog: path.relative(projectRoot, absoluteQuickLog),
      legacyLog: path.relative(projectRoot, absoluteLegacyLog)
    },
    checks,
    allPassed: checks.every((check) => check.exitCode === 0)
  };

  fs.mkdirSync(path.dirname(absoluteOut), { recursive: true });
  fs.writeFileSync(absoluteOut, `${JSON.stringify(report, null, 2)}\n`, 'utf8');

  console.log('[QUICK_CANVAS_GATE_REPORT] WROTE', path.relative(projectRoot, absoluteOut));
  if (!report.allPassed) {
    console.error('[QUICK_CANVAS_GATE_REPORT] FAIL');
    process.exit(1);
  }

  console.log('[QUICK_CANVAS_GATE_REPORT] PASS');
}

main();
