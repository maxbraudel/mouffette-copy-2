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
    'MOUFFETTE_SCENE_PREPARE_TIMEOUT_MS=15000',
    'MOUFFETTE_SCENE_ACTIVATION_LEAD_MS=4000',
    'MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS=50',
    'MOUFFETTE_SCENE_STARTED_ACK_TIMEOUT_MS=5000',
    'MOUFFETTE_SCENE_MAX_START_SKEW_MS=750',
    'MOUFFETTE_UPLOAD_IDLE_TIMEOUT_MS=45000',
    'MOUFFETTE_UPLOAD_TARGET_ACK_TIMEOUT_MS=30000',
    'MOUFFETTE_REMOVAL_ACK_TIMEOUT_MS=30000',
    'MOUFFETTE_CURSOR_DEBUG=false',
    'UNKNOWN_KEY=value',
].join('\n'));
const config = loadServerConfig({ envFile });
assert.equal(config.port, 9090);
assert.equal(config.cursorDebug, false);
assert.equal(config.sceneStartedAckTimeoutMs, 5000);
assert.equal(config.sceneMaxStartSkewMs, 750);
assert.equal(config.sceneActivationLeadMs, 4000,
    'the explicit activation lead is loaded');
assert.equal(config.remoteSessionTeardownRetryInitialMs, 500);
assert.equal(config.remoteSessionTeardownRetryMaxMs, 5000);
assert.equal(config.remoteSessionOpenRequestTtlMs, 300000);
assert.equal(config.remoteSessionTombstoneTtlMs, 300000);
assert.equal(config.sessionRecoveryTimeoutMs, 3000);
assert.equal(config.leaseTimeoutMs, 1500);
assert.equal(config.remoteSessionOpenTimeoutMs, 5000);
assert.equal(config.policyVersion, 4);
assert.ok(config.warnings.some((warning) => warning.includes('UNKNOWN_KEY')));

process.env.MOUFFETTE_MISSPELLED_OPTION = 'true';
const processWarningConfig = loadServerConfig({ envFile });
assert.ok(processWarningConfig.warnings.some(
    (warning) => warning.includes('MOUFFETTE_MISSPELLED_OPTION')));
delete process.env.MOUFFETTE_MISSPELLED_OPTION;

fs.writeFileSync(envFile, [
    'MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS=750',
].join('\n'));
const defaultActivationLeadConfig = loadServerConfig({ envFile });
assert.equal(defaultActivationLeadConfig.sceneActivationLeadMs, 500,
    'the activation lead defaults to 500 ms even though the peer lease is longer');

fs.writeFileSync(envFile, 'MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS=6500\n');
assert.equal(loadServerConfig({ envFile }).sessionRecoveryTimeoutMs, 6500);
fs.writeFileSync(envFile, 'MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS=3000\n');
assert.equal(loadServerConfig({ envFile }).sessionRecoveryTimeoutMs, 3000);

fs.writeFileSync(envFile, 'MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS=1000\nMOUFFETTE_PEER_LEASE_TIMEOUT_MS=3000\n');
assert.equal(loadServerConfig({ envFile }).leaseTimeoutMs, 2000);
assert.ok(loadServerConfig({ envFile }).warnings.some(w => w.includes('MOUFFETTE_PEER_LEASE_TIMEOUT_MS')));
fs.writeFileSync(envFile, 'MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS=0\n');
assert.throws(() => loadServerConfig({ envFile }), /must be in/);

fs.writeFileSync(envFile, [
    'MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS=750',
    'MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_INITIAL_MS=1000',
    'MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_MAX_MS=500',
].join('\n'));
assert.throws(() => loadServerConfig({ envFile }),
    /at least the initial teardown retry delay/);

fs.writeFileSync(envFile, [
    'MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS=750',
    'MOUFFETTE_REMOTE_SESSION_OPEN_REQUEST_TTL_MS=6000',
    'MOUFFETTE_REMOTE_SESSION_TOMBSTONE_TTL_MS=5000',
].join('\n'));
assert.throws(() => loadServerConfig({ envFile }), (error) => {
    assert.equal(error.message,
        'MOUFFETTE_REMOTE_SESSION_OPEN_REQUEST_TTL_MS must not exceed MOUFFETTE_REMOTE_SESSION_TOMBSTONE_TTL_MS');
    return true;
});

fs.writeFileSync(envFile, [
    'MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS=750',
    'MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS=250',
    'MOUFFETTE_SCENE_MAX_START_SKEW_MS=400',
].join('\n'));
assert.throws(() => loadServerConfig({ envFile }),
    /Twice MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS/,
    'the two endpoint clock-error bounds must fit inside the start-skew budget');

fs.rmSync(temporary, { recursive: true, force: true });
console.log('server config tests passed');
