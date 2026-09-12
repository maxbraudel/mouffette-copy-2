'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { loadServerConfig, parseBoolean, parseDotEnv } = require('./config');

assert.equal(parseBoolean('false', 'FLAG'), false);
assert.equal(parseBoolean('true', 'FLAG'), true);
assert.throws(() => parseBoolean('1', 'FLAG'), /true or false/);
assert.deepEqual({ ...parseDotEnv('A=one\n# ignored\nB="two"\n') }, { A: 'one', B: 'two' });

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mouffette-config-'));
const envFile = path.join(temporary, '.env');
fs.writeFileSync(envFile, [
    'MOUFFETTE_SERVER_HOST=127.0.0.1',
    'MOUFFETTE_SERVER_PORT=9090',
    'MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS=750',
    'MOUFFETTE_PEER_LEASE_TIMEOUT_MS=3000',
    'MOUFFETTE_SCENE_PREPARE_TIMEOUT_MS=15000',
    'MOUFFETTE_SCENE_ACTIVATION_LEAD_MS=4000',
    'MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS=50',
    'MOUFFETTE_UPLOAD_IDLE_TIMEOUT_MS=45000',
    'MOUFFETTE_UPLOAD_TARGET_ACK_TIMEOUT_MS=30000',
    'MOUFFETTE_REMOVAL_ACK_TIMEOUT_MS=30000',
    'MOUFFETTE_CURSOR_DEBUG=false',
    'UNKNOWN_KEY=value',
].join('\n'));
const config = loadServerConfig({ envFile });
assert.equal(config.port, 9090);
assert.equal(config.cursorDebug, false);
assert.ok(config.warnings.some((warning) => warning.includes('UNKNOWN_KEY')));

process.env.MOUFFETTE_MISSPELLED_OPTION = 'true';
const processWarningConfig = loadServerConfig({ envFile });
assert.ok(processWarningConfig.warnings.some(
    (warning) => warning.includes('MOUFFETTE_MISSPELLED_OPTION')));
delete process.env.MOUFFETTE_MISSPELLED_OPTION;

fs.writeFileSync(envFile, 'MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS=1000\nMOUFFETTE_PEER_LEASE_TIMEOUT_MS=3000\n');
assert.throws(() => loadServerConfig({ envFile }), /at least 4x/);

fs.rmSync(temporary, { recursive: true, force: true });
console.log('server config tests passed');
