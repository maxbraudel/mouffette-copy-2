'use strict';

const fs = require('node:fs');
const path = require('node:path');

const DECLARATIONS = Object.freeze({
    MOUFFETTE_SERVER_HOST: { type: 'string', default: '0.0.0.0', minLength: 1, maxLength: 255 },
    MOUFFETTE_SERVER_PORT: { type: 'int', default: 8080, min: 1, max: 65535 },
    MOUFFETTE_AUTH_CHALLENGE_TIMEOUT_MS: { type: 'int', default: 10000, min: 1000, max: 120000 },
    MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS: { type: 'int', default: 750, min: 250, max: 5000 },
    MOUFFETTE_PEER_LEASE_TIMEOUT_MS: { type: 'int', default: 3000, min: 1000, max: 30000 },
    MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS: { type: 'int', default: 5000, min: 2000, max: 60000 },
    MOUFFETTE_REMOTE_SESSION_DEGRADED_AFTER_MS: { type: 'int', default: 1500, min: 250, max: 29999 },
    MOUFFETTE_SESSION_LEASE_SWEEP_INTERVAL_MS: { type: 'int', default: 100, min: 25, max: 5000 },
    MOUFFETTE_REMOTE_SESSION_OPEN_TIMEOUT_MS: { type: 'int', default: 5000, min: 250, max: 30000 },
    MOUFFETTE_REMOTE_SESSION_OPEN_REQUEST_TTL_MS: { type: 'int', default: 300000, min: 1000, max: 86400000 },
    MOUFFETTE_REMOTE_SESSION_TOMBSTONE_TTL_MS: { type: 'int', default: 300000, min: 1000, max: 86400000 },
    MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_INITIAL_MS: { type: 'int', default: 500, min: 100, max: 60000 },
    MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_MAX_MS: { type: 'int', default: 5000, min: 100, max: 300000 },
    MOUFFETTE_SCENE_PREPARE_TIMEOUT_MS: { type: 'int', default: 15000, min: 1000, max: 120000 },
    MOUFFETTE_SCENE_ACTIVATION_LEAD_MS: { type: 'int', default: 500, min: 500, max: 10000 },
    // 250 ms accepts a measured RTT up to 500 ms (uncertainty is RTT/2),
    // while the cross-client error budget remains bounded by start skew below.
    MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS: { type: 'int', default: 250, min: 0, max: 250 },
    MOUFFETTE_SCENE_STARTED_ACK_TIMEOUT_MS: { type: 'int', default: 5000, min: 1000, max: 15000 },
    MOUFFETTE_SCENE_MAX_START_SKEW_MS: { type: 'int', default: 750, min: 50, max: 5000 },
    MOUFFETTE_SCENE_STOP_TIMEOUT_MS: { type: 'int', default: 3000, min: 250, max: 120000 },
    MOUFFETTE_SCENE_RUN_TOMBSTONE_TTL_MS: { type: 'int', default: 60000, min: 1000, max: 86400000 },
    MOUFFETTE_UPLOAD_IDLE_TIMEOUT_MS: { type: 'int', default: 45000, min: 5000, max: 600000 },
    MOUFFETTE_UPLOAD_TARGET_ACK_TIMEOUT_MS: { type: 'int', default: 30000, min: 1000, max: 120000 },
    MOUFFETTE_UPLOAD_CHANNEL_TOKEN_TTL_MS: { type: 'int', default: 30000, min: 1000, max: 300000 },
    MOUFFETTE_UPLOAD_SWEEP_INTERVAL_MS: { type: 'int', default: 5000, min: 100, max: 60000 },
    MOUFFETTE_UPLOAD_RESULT_TOMBSTONE_TTL_MS: { type: 'int', default: 60000, min: 1000, max: 86400000 },
    MOUFFETTE_REMOVAL_ACK_TIMEOUT_MS: { type: 'int', default: 30000, min: 1000, max: 120000 },
    MOUFFETTE_ASSET_REMOVAL_TOMBSTONE_TTL_MS: { type: 'int', default: 60000, min: 2000, max: 86400000 },
    MOUFFETTE_STATS_INTERVAL_MS: { type: 'int', default: 30000, min: 1000, max: 3600000 },
    MOUFFETTE_CURSOR_DEBUG: { type: 'bool', default: false },
});

function parseDotEnv(text, source = '.env') {
    const values = Object.create(null);
    for (const [index, rawLine] of String(text).split(/\r?\n/).entries()) {
        const line = rawLine.trim();
        if (!line || line.startsWith('#')) continue;
        const equals = line.indexOf('=');
        if (equals <= 0) throw new Error(`${source}:${index + 1}: expected KEY=VALUE`);
        const key = line.slice(0, equals).trim();
        let value = line.slice(equals + 1).trim();
        if (!/^[A-Z][A-Z0-9_]*$/.test(key)) {
            throw new Error(`${source}:${index + 1}: invalid key ${key}`);
        }
        if ((value.startsWith('"') && value.endsWith('"'))
            || (value.startsWith("'") && value.endsWith("'"))) {
            value = value.slice(1, -1);
        }
        values[key] = value;
    }
    return values;
}

function parseBoolean(value, key) {
    if (typeof value === 'boolean') return value;
    const normalized = String(value).trim().toLowerCase();
    if (normalized === 'true') return true;
    if (normalized === 'false') return false;
    throw new Error(`${key} must be true or false`);
}

function parseDeclaredValue(key, raw, declaration) {
    if (declaration.type === 'bool') return parseBoolean(raw, key);
    if (declaration.type === 'int') {
        if (!/^-?\d+$/.test(String(raw).trim())) throw new Error(`${key} must be an integer`);
        const value = Number(raw);
        if (!Number.isSafeInteger(value) || value < declaration.min || value > declaration.max) {
            throw new Error(`${key} must be in [${declaration.min}, ${declaration.max}]`);
        }
        return value;
    }
    const value = String(raw);
    if (value.length < declaration.minLength || value.length > declaration.maxLength) {
        throw new Error(`${key} has an invalid length`);
    }
    return value;
}

function loadServerConfig(options = {}) {
    const envFile = options.envFile || process.env.MOUFFETTE_ENV_FILE
        || path.join(__dirname, '.env');
    let fileValues = Object.create(null);
    if (envFile && fs.existsSync(envFile)) {
        fileValues = parseDotEnv(fs.readFileSync(envFile, 'utf8'), envFile);
    } else if (options.requireEnvFile) {
        throw new Error(`Configuration file does not exist: ${envFile}`);
    }

    const warnings = [];
    for (const key of Object.keys(fileValues)) {
        if (!Object.prototype.hasOwnProperty.call(DECLARATIONS, key)) {
            warnings.push(`Unknown configuration key in ${envFile}: ${key}`);
        }
    }
    for (const key of Object.keys(process.env)) {
        if (key.startsWith('MOUFFETTE_')
            && key !== 'MOUFFETTE_ENV_FILE'
            && !Object.prototype.hasOwnProperty.call(DECLARATIONS, key)) {
            warnings.push('Unknown process configuration key: ' + key);
        }
    }

    const values = Object.create(null);
    const provenance = Object.create(null);
    for (const [key, declaration] of Object.entries(DECLARATIONS)) {
        let raw = declaration.default;
        let source = 'built-in';
        if (Object.prototype.hasOwnProperty.call(fileValues, key)) {
            raw = fileValues[key];
            source = envFile;
        }
        if (Object.prototype.hasOwnProperty.call(process.env, key)) {
            raw = process.env[key];
            source = 'process-env';
        }
        values[key] = parseDeclaredValue(key, raw, declaration);
        provenance[key] = source;
    }

    if (values.MOUFFETTE_PEER_LEASE_TIMEOUT_MS
        < values.MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS * 4) {
        throw new Error('MOUFFETTE_PEER_LEASE_TIMEOUT_MS must be at least 4x the heartbeat interval');
    }
    if (values.MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS <= values.MOUFFETTE_PEER_LEASE_TIMEOUT_MS) {
        throw new Error('MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS must exceed the transport lease timeout');
    }
    if (values.MOUFFETTE_REMOTE_SESSION_DEGRADED_AFTER_MS
        < values.MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS
        || values.MOUFFETTE_REMOTE_SESSION_DEGRADED_AFTER_MS
            >= values.MOUFFETTE_PEER_LEASE_TIMEOUT_MS) {
        throw new Error('MOUFFETTE_REMOTE_SESSION_DEGRADED_AFTER_MS must be at least one heartbeat and smaller than the peer lease timeout');
    }
    if (values.MOUFFETTE_SESSION_LEASE_SWEEP_INTERVAL_MS
        > values.MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS) {
        throw new Error('MOUFFETTE_SESSION_LEASE_SWEEP_INTERVAL_MS must not exceed the heartbeat interval');
    }
    if (values.MOUFFETTE_REMOTE_SESSION_OPEN_REQUEST_TTL_MS
        < values.MOUFFETTE_REMOTE_SESSION_OPEN_TIMEOUT_MS) {
        throw new Error('MOUFFETTE_REMOTE_SESSION_OPEN_REQUEST_TTL_MS must cover the remote session open timeout');
    }
    if (values.MOUFFETTE_REMOTE_SESSION_TOMBSTONE_TTL_MS
        < Math.max(values.MOUFFETTE_REMOTE_SESSION_OPEN_TIMEOUT_MS,
            values.MOUFFETTE_PEER_LEASE_TIMEOUT_MS)) {
        throw new Error('MOUFFETTE_REMOTE_SESSION_TOMBSTONE_TTL_MS must cover the open and lease timeouts');
    }
    if (values.MOUFFETTE_REMOTE_SESSION_OPEN_REQUEST_TTL_MS
        > values.MOUFFETTE_REMOTE_SESSION_TOMBSTONE_TTL_MS) {
        throw new Error('MOUFFETTE_REMOTE_SESSION_OPEN_REQUEST_TTL_MS must not exceed MOUFFETTE_REMOTE_SESSION_TOMBSTONE_TTL_MS');
    }
    if (values.MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_MAX_MS
        < values.MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_INITIAL_MS) {
        throw new Error('MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_MAX_MS must be at least the initial teardown retry delay');
    }
    if (values.MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS
        >= values.MOUFFETTE_SCENE_ACTIVATION_LEAD_MS) {
        throw new Error('MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS must be smaller than activation lead');
    }
    if (values.MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS * 2
        > values.MOUFFETTE_SCENE_MAX_START_SKEW_MS) {
        throw new Error('Twice MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS must not exceed MOUFFETTE_SCENE_MAX_START_SKEW_MS');
    }
    if (values.MOUFFETTE_SCENE_MAX_START_SKEW_MS
        >= values.MOUFFETTE_SCENE_STARTED_ACK_TIMEOUT_MS) {
        throw new Error('MOUFFETTE_SCENE_MAX_START_SKEW_MS must be smaller than the started acknowledgement timeout');
    }
    if (values.MOUFFETTE_SCENE_RUN_TOMBSTONE_TTL_MS
        < values.MOUFFETTE_SCENE_STOP_TIMEOUT_MS) {
        throw new Error('MOUFFETTE_SCENE_RUN_TOMBSTONE_TTL_MS must cover the scene stop timeout');
    }
    if (values.MOUFFETTE_UPLOAD_RESULT_TOMBSTONE_TTL_MS
        < values.MOUFFETTE_UPLOAD_TARGET_ACK_TIMEOUT_MS) {
        throw new Error('MOUFFETTE_UPLOAD_RESULT_TOMBSTONE_TTL_MS must cover the upload target acknowledgement timeout');
    }
    if (values.MOUFFETTE_ASSET_REMOVAL_TOMBSTONE_TTL_MS
        < values.MOUFFETTE_REMOVAL_ACK_TIMEOUT_MS * 2) {
        throw new Error('MOUFFETTE_ASSET_REMOVAL_TOMBSTONE_TTL_MS must be at least 2x the removal acknowledgement timeout');
    }

    return Object.freeze({
        host: values.MOUFFETTE_SERVER_HOST,
        port: values.MOUFFETTE_SERVER_PORT,
        authChallengeTimeoutMs: values.MOUFFETTE_AUTH_CHALLENGE_TIMEOUT_MS,
        heartbeatIntervalMs: values.MOUFFETTE_PEER_HEARTBEAT_INTERVAL_MS,
        leaseTimeoutMs: values.MOUFFETTE_PEER_LEASE_TIMEOUT_MS,
        sessionRecoveryTimeoutMs: values.MOUFFETTE_REMOTE_SESSION_RECOVERY_TIMEOUT_MS,
        remoteSessionDegradedAfterMs: values.MOUFFETTE_REMOTE_SESSION_DEGRADED_AFTER_MS,
        sessionLeaseSweepIntervalMs: values.MOUFFETTE_SESSION_LEASE_SWEEP_INTERVAL_MS,
        remoteSessionOpenTimeoutMs: values.MOUFFETTE_REMOTE_SESSION_OPEN_TIMEOUT_MS,
        remoteSessionOpenRequestTtlMs: values.MOUFFETTE_REMOTE_SESSION_OPEN_REQUEST_TTL_MS,
        remoteSessionTombstoneTtlMs: values.MOUFFETTE_REMOTE_SESSION_TOMBSTONE_TTL_MS,
        remoteSessionTeardownRetryInitialMs:
            values.MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_INITIAL_MS,
        remoteSessionTeardownRetryMaxMs:
            values.MOUFFETTE_REMOTE_SESSION_TEARDOWN_RETRY_MAX_MS,
        scenePrepareTimeoutMs: values.MOUFFETTE_SCENE_PREPARE_TIMEOUT_MS,
        sceneActivationLeadMs: values.MOUFFETTE_SCENE_ACTIVATION_LEAD_MS,
        sceneMaxClockSkewMs: values.MOUFFETTE_SCENE_MAX_CLOCK_SKEW_MS,
        sceneStartedAckTimeoutMs: values.MOUFFETTE_SCENE_STARTED_ACK_TIMEOUT_MS,
        sceneMaxStartSkewMs: values.MOUFFETTE_SCENE_MAX_START_SKEW_MS,
        sceneStopTimeoutMs: values.MOUFFETTE_SCENE_STOP_TIMEOUT_MS,
        sceneRunTombstoneTtlMs: values.MOUFFETTE_SCENE_RUN_TOMBSTONE_TTL_MS,
        uploadIdleTimeoutMs: values.MOUFFETTE_UPLOAD_IDLE_TIMEOUT_MS,
        uploadTargetAckTimeoutMs: values.MOUFFETTE_UPLOAD_TARGET_ACK_TIMEOUT_MS,
        uploadChannelTokenTtlMs: values.MOUFFETTE_UPLOAD_CHANNEL_TOKEN_TTL_MS,
        uploadSweepIntervalMs: values.MOUFFETTE_UPLOAD_SWEEP_INTERVAL_MS,
        uploadResultTombstoneTtlMs: values.MOUFFETTE_UPLOAD_RESULT_TOMBSTONE_TTL_MS,
        removalAckTimeoutMs: values.MOUFFETTE_REMOVAL_ACK_TIMEOUT_MS,
        assetRemovalTombstoneTtlMs: values.MOUFFETTE_ASSET_REMOVAL_TOMBSTONE_TTL_MS,
        statsIntervalMs: values.MOUFFETTE_STATS_INTERVAL_MS,
        cursorDebug: values.MOUFFETTE_CURSOR_DEBUG,
        policyVersion: 3,
        values: Object.freeze(values),
        provenance: Object.freeze(provenance),
        warnings: Object.freeze(warnings),
    });
}

module.exports = { DECLARATIONS, loadServerConfig, parseDotEnv, parseBoolean };
