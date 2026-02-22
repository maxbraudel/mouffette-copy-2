#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

const projectRoot = path.resolve(__dirname, '..', '..');
const matrixPath = path.join(__dirname, 'matrix', 'phase7_integration_hardening.json');

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function runMatrix() {
  assert(fs.existsSync(matrixPath), `missing matrix file: ${path.relative(projectRoot, matrixPath)}`);

  const matrix = readJson(matrixPath);
  assert(Array.isArray(matrix.cases) && matrix.cases.length > 0, 'phase7 matrix cases are required');

  let totalRules = 0;
  let totalAssertions = 0;

  for (const testCase of matrix.cases) {
    assert(testCase.id && testCase.id.length > 0, 'each matrix case must have an id');
    assert(Array.isArray(testCase.rules) && testCase.rules.length > 0, `${testCase.id}: rules are required`);

    for (const rule of testCase.rules) {
      totalRules += 1;
      const targetPath = path.join(projectRoot, rule.file);
      assert(fs.existsSync(targetPath), `${testCase.id}: target file not found ${rule.file}`);

      const content = fs.readFileSync(targetPath, 'utf8');
      const patterns = Array.isArray(rule.mustContain) ? rule.mustContain : [];
      assert(patterns.length > 0, `${testCase.id}: mustContain patterns are required for ${rule.file}`);

      for (const pattern of patterns) {
        const regex = new RegExp(pattern, 'm');
        assert(regex.test(content), `${testCase.id}: pattern '${pattern}' missing in ${rule.file}`);
        totalAssertions += 1;
      }
    }
  }

  return {
    matrixId: matrix.id || 'phase7_integration_hardening',
    casesChecked: matrix.cases.length,
    rulesChecked: totalRules,
    assertions: totalAssertions,
    matrixFile: path.relative(projectRoot, matrixPath)
  };
}

function main() {
  const result = runMatrix();
  console.log('[PHASE7_INTEGRATION_HARDENING] PASS');
  console.log(JSON.stringify(result, null, 2));
}

try {
  main();
} catch (error) {
  console.error('[PHASE7_INTEGRATION_HARDENING] FAIL');
  console.error(error.message);
  process.exit(1);
}
