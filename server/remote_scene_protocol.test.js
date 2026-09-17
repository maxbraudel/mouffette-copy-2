'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');
const {
    SCENE_PHASES, SceneRunRegistry, computeSceneDigest,
} = require('./scene_run_registry');
const { RemoteSessionRegistry } = require('./remote_session_registry');

function socket() {
    return {
        readyState: WebSocket.OPEN,
        bufferedAmount: 0,
        messages: [],
        send(encoded) { this.messages.push(JSON.parse(encoded)); },
        close() { this.readyState = WebSocket.CLOSED; },
    };
}

function addClient(server, connectionId, endpointId) {
    const ws = socket();
    server.clients.set(connectionId, {
        id: connectionId,
        sessionId: connectionId,
        persistentId: endpointId,
        endpointId,
        runtimeId: `runtime-${endpointId}`,
        connectionGeneration: 1,
        authenticated: true,
        machineName: endpointId,
        ws,
    });
    server.currentTransportByEndpoint.set(endpointId, server.clients.get(connectionId));
    server.connectionGenerationSequence = Math.max(server.connectionGenerationSequence,
        server.clients.get(connectionId).connectionGeneration);
    return ws;
}

function messages(ws, type) {
    return ws.messages.filter(message => message.type === type);
}

function envelope(session, extra = {}) {
    return {
        protocolVersion: 7,
        serverBootId: session.serverBootId,
        messageId: crypto.randomUUID(),
        remoteSessionId: session.remoteSessionId,
        generation: session.generation,
        connectionGeneration: 1,
        ...extra,
    };
}

const asset = Object.freeze({
    assetId: 'asset-1', extension: 'png', fileId: 'a'.repeat(64),
    mediaIds: ['media-1'], sha256: 'a'.repeat(64), size: 128,
});
const scene = Object.freeze({
    renderSchemaVersion: 2,
    screens: [{ id: 1, x: 0, y: 0, width: 1920, height: 1080, primary: true }],
    media: [{
        mediaId: 'media-1', assetId: 'asset-1', fileId: 'a'.repeat(64),
        fileName: 'asset.png', type: 'image',
        x: 0, y: 0, width: 1920, height: 1080,
        baseWidth: 1920, baseHeight: 1080, visible: true, z: 1,
        autoDisplay: false, autoDisplayDelayMs: 0,
        autoHide: false, autoHideDelayMs: 0, hideWhenVideoEnds: false,
        fadeInSeconds: 0, fadeOutSeconds: 0, contentOpacity: 1,
        spans: [{
            screenId: 1, normX: 0, normY: 0, normW: 1, normH: 1,
            spanDestNormX: 0, spanDestNormY: 0,
            spanDestNormW: 1, spanDestNormH: 1,
            spanSourceNormX: 0, spanSourceNormY: 0,
            spanSourceNormW: 1, spanSourceNormH: 1,
        }],
    }],
});
const checklist = Object.freeze([
    { itemId: 'screen_1', stage: 'screen_render_graph_ready', ready: true },
    { itemId: 'media-1_file', stage: 'file_validated', ready: true },
    { itemId: 'media-1_memory', stage: 'media_memory_ready', ready: true },
    { itemId: 'media-1_decode', stage: 'image_decoded', ready: true },
    { itemId: 'media-1_texture', stage: 'image_texture_ready', ready: true },
]);

// Deterministic timing and terminal replay.
{
    let epoch = 10_000;
    let monotonic = 500;
    const registry = new SceneRunRegistry({
        epochNow: () => epoch,
        monotonicNow: () => monotonic,
        prepareTimeoutMs: 15_000,
        activationLeadMs: 4_000,
        startedAckTimeoutMs: 5_000,
        stopTimeoutMs: 3_000,
        maximumClockUncertaintyMs: 50,
        maximumStartSkewMs: 750,
    });
    const digest = computeSceneDigest(1, [asset], scene);
    const binding = {
        remoteSessionId: 'session-1', generation: 1, sceneRunId: 'run-1', revision: 1,
        digest, manifest: [asset], scene, ownerEndpointId: 'A', targetEndpointId: 'B',
    };
    assert.equal(registry.prepare(binding).ok, true);
    assert.equal(registry.prepare(binding).replay, true);
    assert.equal(registry.prepare({
        ...binding,
        remoteSessionId: 'session-competing',
        sceneRunId: 'run-competing',
        ownerEndpointId: 'C',
    }).error, 'target_scene_already_running',
    'one target-wide scene lock must cover every RemoteSession');
    assert.equal(registry.markPrepared('run-1', 'A', digest).ready, false);
    assert.equal(registry.markPrepared('run-1', 'B', digest).ready, true);
    assert.equal(registry.arm('run-1', 'A', digest, 51).error,
        'clock_uncertainty_too_high');
    assert.equal(registry.arm('run-1', 'A', digest, 10).scheduled, false);
    const committed = registry.arm('run-1', 'B', digest, 10);
    assert.equal(committed.run.activationLeadMs, 4_020);
    assert.equal(committed.run.startEpochMs, 14_020);
    assert.equal(committed.run.startServerMonotonicMs, 4_520);
    assert.equal(registry.markStarted('run-1', 'A', digest, true, 4_520).error,
        'first_frame_presented_before_commit');
    epoch = -1_000_000;
    monotonic = 4_520;
    assert.equal(registry.markStarted('run-1', 'A', digest, true).error,
        'invalid_first_frame_timestamp');
    assert.equal(registry.markStarted('run-1', 'A', digest, true, 4_520).live, false,
        'a wall-clock rollback must not reject an on-time monotonic STARTED acknowledgement');
    monotonic = 5_271;
    const excessiveSkew = registry.markStarted(
        'run-1', 'B', digest, true, 5_271);
    assert.equal(excessiveSkew.error, 'scene_start_skew_too_high');
    assert.equal(excessiveSkew.startSkewMs, 751);
    monotonic = 5_270;
    const started = registry.markStarted('run-1', 'B', digest, true, 5_270);
    assert.equal(started.live, true);
    assert.equal(started.startSkewMs, 750);
    assert.equal(registry.markStarted('run-1', 'B', digest, true, 5_270).replay, true);
    assert.equal(registry.markStarted('run-1', 'B', digest, true, 5_269).error,
        'conflicting_started_ack');
    assert.equal(registry.acceptSnapshot('run-1', 'A', digest, 1).ok, true);
    assert.equal(registry.acceptSnapshot('run-1', 'A', digest, 1).error,
        'stale_snapshot_sequence');
    registry.stop('run-1', 'owner_stop');
    assert.equal(registry.acknowledgeStopped('run-1', 'A', true).completed, false);
    assert.equal(registry.prepare({
        ...binding,
        remoteSessionId: 'session-competing',
        sceneRunId: 'run-competing',
        ownerEndpointId: 'C',
    }).error, 'target_scene_already_running',
    'the target lock must remain held throughout Stopping');
    assert.equal(registry.acknowledgeStopped('run-1', 'B', true).completed, true);
    assert.equal(registry.stop('run-1', 'owner_stop').replay, true);

    const preparing = registry.prepare({
        ...binding, remoteSessionId: 'session-2', sceneRunId: 'run-2',
    }).run;
    epoch += 24 * 60 * 60 * 1_000;
    monotonic = preparing.prepareDeadlineServerMonotonicMs - 1;
    assert.deepEqual(registry.tick(epoch, monotonic), [],
        'a wall-clock jump must not expire scene preparation early');
    assert.equal(registry.tombstones.has('run-1'), true,
        'a wall-clock jump must not evict an idempotency tombstone early');
    epoch -= 48 * 60 * 60 * 1_000;
    monotonic = preparing.prepareDeadlineServerMonotonicMs;
    const timeout = registry.tick(epoch, monotonic)[0];
    assert.equal(timeout.code, 'scene_prepare_timeout');
    assert.equal(timeout.run.phase, SCENE_PHASES.STOPPING);
    registry.forceFinalize('run-2', true);

    epoch = 40_000;
    monotonic = 30_000;
    const noAckBinding = {
        ...binding, remoteSessionId: 'session-3', sceneRunId: 'run-3',
    };
    registry.prepare(noAckBinding);
    registry.markPrepared('run-3', 'A', digest);
    registry.markPrepared('run-3', 'B', digest);
    registry.arm('run-3', 'A', digest, 10);
    registry.arm('run-3', 'B', digest, 10);
    const scheduled = registry.get('run-3');
    epoch += 24 * 60 * 60 * 1_000;
    monotonic = scheduled.startedDeadlineServerMonotonicMs - 1;
    assert.deepEqual(registry.tick(epoch, monotonic), [],
        'a wall-clock jump must not expire the first-frame barrier early');
    epoch -= 48 * 60 * 60 * 1_000;
    monotonic = scheduled.startedDeadlineServerMonotonicMs;
    const missingAck = registry.tick(epoch, monotonic)[0];
    assert.equal(missingAck.code, 'scene_started_timeout');
    assert.equal(missingAck.run.phase, SCENE_PHASES.STOPPING);
    registry.forceFinalize('run-3', true);

    epoch = 80_000;
    monotonic = 60_000;
    const stopping = registry.prepare({
        ...binding, remoteSessionId: 'session-4', sceneRunId: 'run-4',
    }).run;
    registry.stop(stopping.sceneRunId, 'test_stop');
    epoch += 24 * 60 * 60 * 1_000;
    monotonic = stopping.stopDeadlineServerMonotonicMs - 1;
    assert.deepEqual(registry.tick(epoch, monotonic), [],
        'a wall-clock jump must not finalize a STOPPING graph early');
    epoch -= 48 * 60 * 60 * 1_000;
    monotonic = stopping.stopDeadlineServerMonotonicMs;
    const stoppedByMonotonicDeadline = registry.tick(epoch, monotonic)[0];
    assert.equal(stoppedByMonotonicDeadline.code, 'scene_stop_timeout');
    assert.equal(stoppedByMonotonicDeadline.run.phase, SCENE_PHASES.STOPPED);
}

// WAN clock quality expands only this run's delivery lead. The welcome policy
// advertises the worst-case ceiling, while the exact COMMIT remains adaptive.
{
    let monotonic = 20_000;
    const registry = new SceneRunRegistry({
        epochNow: () => 30_000,
        monotonicNow: () => monotonic,
        activationLeadMs: 500,
        maximumClockUncertaintyMs: 250,
        maximumStartSkewMs: 750,
    });
    const digest = computeSceneDigest(1, [asset], scene);
    const binding = {
        remoteSessionId: 'session-wan-lead', generation: 1,
        sceneRunId: 'run-wan-lead', revision: 1, digest,
        manifest: [asset], scene, ownerEndpointId: 'A', targetEndpointId: 'B',
    };
    assert.equal(registry.activationLeadCeilingMs(), 1_000);
    assert.equal(registry.prepare(binding).ok, true);
    assert.equal(registry.markPrepared(binding.sceneRunId, 'A', digest).ready, false);
    assert.equal(registry.markPrepared(binding.sceneRunId, 'B', digest).ready, true);
    assert.equal(registry.arm(binding.sceneRunId, 'A', digest, 60).scheduled, false);
    const committed = registry.arm(binding.sceneRunId, 'B', digest, 180);
    assert.equal(committed.run.activationLeadMs, 860,
        'base lead plus a complete worst reported RTT is reserved');
    assert.equal(committed.run.startServerMonotonicMs, 20_860);

    const scheduledStart = committed.run.startServerMonotonicMs;
    const scheduledLead = committed.run.activationLeadMs;
    const replayed = registry.arm(binding.sceneRunId, 'A', digest, 220);
    assert.equal(replayed.ok, true);
    assert.equal(replayed.replay, true);
    assert.equal(replayed.scheduled, true);
    assert.equal(replayed.run.armedClockUncertaintyByEndpoint.get('A'), 220,
        'a rebound transport replaces the reporter clock bound');
    assert.equal(replayed.run.startServerMonotonicMs, scheduledStart,
        'replaying ARMED must not reschedule an already published COMMIT');
    assert.equal(replayed.run.activationLeadMs, scheduledLead);

    const rejectedReplay = registry.arm(binding.sceneRunId, 'A', digest, 251);
    assert.equal(rejectedReplay.error, 'clock_uncertainty_too_high');
    assert.equal(committed.run.armedClockUncertaintyByEndpoint.get('A'), 220,
        'an invalid replay must not replace the last validated reporter bound');

    monotonic = scheduledStart;
    assert.equal(registry.markStarted(
        binding.sceneRunId, 'A', digest, true, scheduledStart).live, false);
    assert.equal(registry.markStarted(
        binding.sceneRunId, 'B', digest, true, scheduledStart).live, true);
    const liveReplay = registry.arm(binding.sceneRunId, 'B', digest, 230);
    assert.equal(liveReplay.ok, true);
    assert.equal(liveReplay.replay, true);
    assert.equal(liveReplay.run.armedClockUncertaintyByEndpoint.get('B'), 230);
    assert.equal(liveReplay.run.startServerMonotonicMs, scheduledStart);
}

// Grace cancels pre-start graphs while retaining the resumable session.
{
    let now = 100_000;
    let sequence = 0;
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    server.remoteSessions = new RemoteSessionRegistry({
        leaseTimeoutMs: 3_000,
        now: () => now,
        idFactory: () => `grace-scene-${++sequence}`,
    });
    server.sceneRuns = new SceneRunRegistry({
        epochNow: () => now,
        monotonicNow: () => now,
        prepareTimeoutMs: 15_000,
        activationLeadMs: 4_000,
        maximumClockUncertaintyMs: 50,
    });
    const owner = addClient(server, 'grace-owner', 'A');
    addClient(server, 'grace-target', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    const digest = computeSceneDigest(1, [asset], scene);
    const run = server.sceneRuns.prepare({
        remoteSessionId: session.remoteSessionId,
        generation: session.generation,
        sceneRunId: 'grace-scene-run', revision: 1, digest,
        manifest: [asset], scene,
        ownerEndpointId: 'A', targetEndpointId: 'B',
    }).run;
    session.sceneRunId = run.sceneRunId;

    now += 1;
    assert.equal(server.handleRemoteSessionDeparture(
        server.clients.get('grace-owner'), now), true);
    assert.equal(session.phase, 'Grace');
    assert.equal(run.phase, SCENE_PHASES.STOPPING);
    assert.equal(messages(owner, 'stop').length, 1,
        'Grace must prevent a delayed start');

    now = 102_999;
    server.clients.get('grace-owner').connectionGeneration = 2;
    const resumed = server.remoteSessions.resume({
        remoteSessionId: session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: session.resumeToken,
        generation: session.generation,
        connectionGeneration: 2,
    }, now);
    assert.equal(resumed.ok, true);
    server.rebindSessionGeneration(resumed.session);
    assert.equal(resumed.session.phase, 'Active');
    assert.equal(run.phase, SCENE_PHASES.STOPPING);
    assert.equal(run.generation, resumed.session.generation);
}

// Scene messages enforce the lease before any phase mutation. Preparation at
// 2,999 ms is valid, while PREPARED at the exact deadline first terminalizes
// the RemoteSession and tears down the graph.
{
    let now = 200_000;
    let sequence = 0;
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    server.remoteSessions = new RemoteSessionRegistry({
        leaseTimeoutMs: 3_000,
        now: () => now,
        idFactory: () => `scene-lease-${++sequence}`,
    });
    server.sceneRuns = new SceneRunRegistry({
        epochNow: () => now,
        monotonicNow: () => now,
        prepareTimeoutMs: 15_000,
        activationLeadMs: 4_000,
        maximumClockUncertaintyMs: 50,
    });
    const owner = addClient(server, 'lease-owner', 'A');
    const target = addClient(server, 'lease-target', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    session.mediaResidency = { generation: session.generation, sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256, state: 'ready', progress: 1, error: '' }] };
    server.sessionAssets.set(session.remoteSessionId, new Map([[
        asset.assetId,
        { ...asset, remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            ownerEndpointId: 'A', targetEndpointId: 'B', uploadId: 'lease-upload' },
    ]]));
    const digest = computeSceneDigest(1, [asset], scene);

    now = 202_999;
    server.handleMessage('lease-owner', envelope(session, {
        type: 'scene_prepare', sceneRunId: 'lease-scene-run', revision: 1,
        digest, manifest: [asset], scene,
    }));
    assert.equal(server.sceneRuns.get('lease-scene-run').phase, SCENE_PHASES.PREPARING);

    now = 203_000;
    server.handleMessage('lease-owner', envelope(session, {
        type: 'prepared', sceneRunId: 'lease-scene-run', digest,
        success: true, checklist,
    }));
    assert.equal(session.phase, 'CleanupPending');
    assert.equal(server.sceneRuns.get('lease-scene-run').phase, SCENE_PHASES.STOPPING);
    assert.equal(server.sceneRuns.get('lease-scene-run').preparedEndpoints.size, 0);
    assert.equal(messages(owner, 'error').at(-1).code, 'lease_expired');
    assert.equal(messages(target, 'remote_session_terminating').length, 1);
}

// A successful PREPARED message is a fail-closed protocol boundary. An
// incomplete checklist or wrong digest immediately destroys both prepared
// graphs while preserving the active RemoteSession and validated uploads.
for (const invalidCase of [
    {
        suffix: 'checklist',
        code: 'scene_checklist_incomplete',
        prepared: digest => ({
            digest,
            checklist: [
                { itemId: 'media-1_decode', stage: 'image_decoded', ready: true },
            ],
        }),
    },
    {
        suffix: 'digest',
        code: 'scene_digest_mismatch',
        prepared: () => ({ digest: 'b'.repeat(64), checklist }),
    },
    {
        suffix: 'decoder',
        code: 'decoder_failed',
        prepared: digest => ({
            digest, success: false, checklist, errorCode: 'decoder_failed',
        }),
    },
]) {
    const server = new MouffetteServer(0);
    const owner = addClient(server, `invalid-${invalidCase.suffix}-owner`, 'A');
    const target = addClient(server, `invalid-${invalidCase.suffix}-target`, 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    session.mediaResidency = { generation: session.generation, sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256, state: 'ready', progress: 1, error: '' }] };
    server.sessionAssets.set(session.remoteSessionId, new Map([[
        asset.assetId,
        { ...asset, remoteSessionId: session.remoteSessionId,
            generation: session.generation, ownerEndpointId: 'A', targetEndpointId: 'B',
            uploadId: `validated-${invalidCase.suffix}` },
    ]]));
    const digest = computeSceneDigest(1, [asset], scene);
    const sceneRunId = `run-invalid-${invalidCase.suffix}`;
    server.handleMessage(`invalid-${invalidCase.suffix}-owner`, envelope(session, {
        type: 'scene_prepare', sceneRunId, revision: 1,
        digest, manifest: [asset], scene,
    }));
    server.handleMessage(`invalid-${invalidCase.suffix}-target`, envelope(session, {
        type: 'prepared', sceneRunId, success: true,
        ...invalidCase.prepared(digest),
    }));

    const run = server.sceneRuns.get(sceneRunId);
    const preparationError = messages(target, 'error').at(-1)
        || messages(owner, 'error').at(-1);
    assert.equal(preparationError.code, invalidCase.code);
    assert.equal(run.phase, SCENE_PHASES.STOPPING);
    assert.equal(run.failed, true);
    assert.equal(server.metrics.value('scene_prepare_failed_total'), 1,
        'every fail-closed preparation error is counted once');
    assert.equal(messages(owner, 'stop').at(-1).reason, invalidCase.code);
    assert.equal(messages(target, 'stop').at(-1).reason, invalidCase.code);
    assert.equal(session.phase, 'Active');
    assert.equal(server.sessionAssets.get(session.remoteSessionId).has(asset.assetId), true,
        'SceneRun failure must retain validated session uploads');
    assert.equal(messages(owner, 'remote_session_terminating').length, 0);
    assert.equal(messages(target, 'remote_session_terminating').length, 0);

    server.handleMessage(`invalid-${invalidCase.suffix}-owner`, envelope(session, {
        type: 'stopped', sceneRunId, digest, success: true,
    }));
    server.handleMessage(`invalid-${invalidCase.suffix}-target`, envelope(session, {
        type: 'stopped', sceneRunId, digest, success: true,
    }));
    assert.equal(server.sceneRuns.get(sceneRunId), null);
    assert.equal(session.sceneRunId, null);
    assert.equal(server.sessionAssets.get(session.remoteSessionId).has(asset.assetId), true);
}

// The same fail-closed rule applies only after an ARMED packet has passed the
// authenticated RemoteSession correlation checks. Foreign or stale packets
// cannot use a known sceneRunId to tear down another party's graph.
for (const invalidCase of [
    {
        suffix: 'armed-digest',
        code: 'scene_digest_mismatch',
        prepareBoth: true,
        armed: digest => ({ digest: 'b'.repeat(64), clockUncertaintyMs: 10 }),
    },
    {
        suffix: 'armed-phase',
        code: 'scene_not_prepared',
        prepareBoth: false,
        armed: digest => ({ digest, clockUncertaintyMs: 10 }),
    },
]) {
    const server = new MouffetteServer(0);
    const ownerId = `${invalidCase.suffix}-owner`;
    const targetId = `${invalidCase.suffix}-target`;
    const attackerId = `${invalidCase.suffix}-attacker`;
    const owner = addClient(server, ownerId, 'A');
    const target = addClient(server, targetId, 'B');
    const attacker = addClient(server, attackerId, 'C');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    session.mediaResidency = { generation: session.generation, sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256, state: 'ready', progress: 1, error: '' }] };
    server.sessionAssets.set(session.remoteSessionId, new Map([[
        asset.assetId,
        { ...asset, remoteSessionId: session.remoteSessionId,
            generation: session.generation, ownerEndpointId: 'A', targetEndpointId: 'B',
            uploadId: `validated-${invalidCase.suffix}` },
    ]]));
    const digest = computeSceneDigest(1, [asset], scene);
    const sceneRunId = `run-invalid-${invalidCase.suffix}`;
    server.handleMessage(ownerId, envelope(session, {
        type: 'scene_prepare', sceneRunId, revision: 1,
        digest, manifest: [asset], scene,
    }));
    if (invalidCase.prepareBoth) {
        server.handleMessage(ownerId, envelope(session, {
            type: 'prepared', sceneRunId, digest, success: true, checklist,
        }));
        server.handleMessage(targetId, envelope(session, {
            type: 'prepared', sceneRunId, digest, success: true, checklist,
        }));
        assert.equal(server.sceneRuns.get(sceneRunId).phase, SCENE_PHASES.PREPARED);
    }

    server.handleMessage(attackerId, envelope(session, {
        type: 'armed', sceneRunId, digest, clockUncertaintyMs: 10,
    }));
    assert.equal(messages(attacker, 'error').at(-1).code, 'not_a_session_party');
    assert.notEqual(server.sceneRuns.get(sceneRunId).phase, SCENE_PHASES.STOPPING);
    server.handleMessage(ownerId, {
        ...envelope(session, {
            type: 'armed', sceneRunId, digest, clockUncertaintyMs: 10,
        }),
        generation: session.generation + 1,
    });
    assert.equal(messages(owner, 'error').at(-1).code,
        'stale_remote_session_generation');
    assert.notEqual(server.sceneRuns.get(sceneRunId).phase, SCENE_PHASES.STOPPING);

    server.handleMessage(targetId, envelope(session, {
        type: 'armed', sceneRunId, ...invalidCase.armed(digest),
    }));
    const run = server.sceneRuns.get(sceneRunId);
    assert.equal(messages(target, 'error').at(-1).code, invalidCase.code);
    assert.equal(run.phase, SCENE_PHASES.STOPPING);
    assert.equal(run.failed, true);
    assert.equal(messages(owner, 'stop').at(-1).reason, invalidCase.code);
    assert.equal(messages(target, 'stop').at(-1).reason, invalidCase.code);
    assert.equal(session.phase, 'Active');
    assert.equal(server.sessionAssets.get(session.remoteSessionId).has(asset.assetId), true);
}

// Full wire lifecycle, authenticated session ownership and exact inventory.
{
    let wireEpoch = Date.now();
    let wireMonotonic = 10_000;
    const server = new MouffetteServer(0);
    server.epochNow = () => wireEpoch;
    server.monotonicNow = () => wireMonotonic;
    server.sceneRuns = new SceneRunRegistry({
        epochNow: () => wireEpoch,
        monotonicNow: () => wireMonotonic,
        prepareTimeoutMs: 15_000,
        maximumClockUncertaintyMs: 50,
    });
    const owner = addClient(server, 'owner-connection', 'A');
    const target = addClient(server, 'target-connection', 'B');
    const attacker = addClient(server, 'attacker-connection', 'C');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    session.mediaResidency = { generation: session.generation, sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256, state: 'ready', progress: 1, error: '' }] };
    server.sessionAssets.set(session.remoteSessionId, new Map([[
        asset.assetId,
        { ...asset, remoteSessionId: session.remoteSessionId, generation: session.generation,
            ownerEndpointId: 'A', targetEndpointId: 'B', uploadId: 'upload-1' },
    ]]));
    const digest = computeSceneDigest(1, [asset], scene);
    const prepare = envelope(session, {
        type: 'scene_prepare', sceneRunId: 'run-wire-1', revision: 1,
        digest, manifest: [asset], scene,
    });

    server.handleMessage('attacker-connection', structuredClone(prepare));
    assert.equal(messages(attacker, 'error').at(-1).code, 'not_a_session_party');
    server.handleMessage('owner-connection', structuredClone(prepare));
    assert.equal(messages(target, 'scene_prepare').length, 1);
    assert.equal(messages(target, 'scene_prepare')[0].ownerEndpointId, 'A');
    assert.equal(messages(target, 'scene_prepare')[0].targetEndpointId, 'B');
    const accepted = messages(owner, 'prepare_progress').at(-1);
    assert.equal(accepted.aggregate, true);
    assert.equal(accepted.stage, 'accepted');
    assert.equal(accepted.replay, undefined);
    server.handleMessage('owner-connection', structuredClone(prepare));
    assert.equal(messages(target, 'scene_prepare').length, 1);
    const replayedAcceptance = messages(owner, 'prepare_progress').at(-1);
    assert.equal(replayedAcceptance.aggregate, true);
    assert.equal(replayedAcceptance.stage, 'accepted');
    assert.equal(replayedAcceptance.replay, true);

    server.handleMessage('owner-connection', envelope(session, {
        type: 'prepared', sceneRunId: 'run-wire-1', digest, success: true, checklist,
    }));
    server.handleMessage('target-connection', envelope(session, {
        type: 'prepare_progress', sceneRunId: 'run-wire-1', digest, percent: 100, checklist,
    }));
    server.handleMessage('target-connection', envelope(session, {
        type: 'prepared', sceneRunId: 'run-wire-1', digest, success: true, checklist,
    }));
    assert.equal(server.sceneRuns.get('run-wire-1').phase, SCENE_PHASES.PREPARED);
    assert.equal(messages(owner, 'prepare_progress').at(-1).reporterEndpointId, 'B');

    server.handleMessage('owner-connection', envelope(session, {
        type: 'armed', sceneRunId: 'run-wire-1', digest, clockUncertaintyMs: 10,
    }));
    server.handleMessage('target-connection', envelope(session, {
        type: 'armed', sceneRunId: 'run-wire-1', digest, clockUncertaintyMs: 10,
    }));
    assert.equal(messages(owner, 'commit').at(-1).activationLeadMs, 520);
    assert.equal(messages(target, 'commit').length, 1);
    wireEpoch += 520;
    wireMonotonic += 520;

    server.handleMessage('owner-connection', envelope(session, {
        type: 'started', sceneRunId: 'run-wire-1', digest, firstFramePresented: true,
        presentedServerMonotonicMs: wireMonotonic,
    }));
    wireMonotonic += 25;
    server.handleMessage('target-connection', envelope(session, {
        type: 'started', sceneRunId: 'run-wire-1', digest, firstFramePresented: true,
        presentedServerMonotonicMs: wireMonotonic,
    }));
    assert.equal(server.sceneRuns.get('run-wire-1').phase, SCENE_PHASES.LIVE);
    assert.equal(messages(owner, 'started').at(-1).startSkewMs, 25);
    server.handleMessage('owner-connection', envelope(session, {
        type: 'state_snapshot', sceneRunId: 'run-wire-1', digest, sequence: 1,
        sampledServerMonotonicMs: wireMonotonic,
        snapshot: { videos: [{ mediaId: 'media-1', positionMs: 42 }] },
    }));
    assert.equal(messages(target, 'state_snapshot').at(-1).sequence, 1);

    const snapshotCountBeforeInvalidTimestamp = messages(target, 'state_snapshot').length;
    server.handleMessage('owner-connection', envelope(session, {
        type: 'state_snapshot', sceneRunId: 'run-wire-1', digest, sequence: 2,
        snapshot: { videos: [] },
    }));
    assert.equal(messages(owner, 'error').at(-1).code,
        'invalid_state_snapshot_timestamp');
    server.handleMessage('owner-connection', envelope(session, {
        type: 'state_snapshot', sceneRunId: 'run-wire-1', digest, sequence: 2,
        sampledServerMonotonicMs:
            wireMonotonic + server.config.sceneMaxClockSkewMs + 1,
        snapshot: { videos: [] },
    }));
    assert.equal(messages(owner, 'error').at(-1).code,
        'invalid_state_snapshot_timestamp');
    assert.equal(messages(target, 'state_snapshot').length,
        snapshotCountBeforeInvalidTimestamp,
        'an absent or implausibly future server timestamp must fail closed');

    const authoritativeSnapshot = { serializedState: 'x'.repeat(300 * 1024) };
    server.handleMessage('owner-connection', envelope(session, {
        type: 'state_snapshot', sceneRunId: 'run-wire-1', digest, sequence: 2,
        sampledServerMonotonicMs: wireMonotonic,
        snapshot: authoritativeSnapshot,
    }));
    assert.equal(messages(target, 'state_snapshot').at(-1).sequence, 2,
        'a complete snapshot larger than the former 256 KiB ceiling is relayed');
    const relayedSnapshotCount = messages(target, 'state_snapshot').length;

    server.handleMessage('owner-connection', envelope(session, {
        type: 'state_snapshot', sceneRunId: 'run-wire-1', digest, sequence: 3,
        sampledServerMonotonicMs: wireMonotonic,
        snapshot: {
            serializedState: 'x'.repeat(
                server.MAX_REMOTE_SCENE_STATE_SNAPSHOT_BYTES + 1),
        },
    }));
    assert.equal(messages(owner, 'error').at(-1).code, 'invalid_state_snapshot');
    assert.equal(messages(target, 'state_snapshot').length, relayedSnapshotCount,
        'a snapshot larger than 8 MiB must not reach the target');

    server.handleMessage('owner-connection', envelope(session, {
        type: 'stop', sceneRunId: 'run-wire-1', digest, reason: 'owner_stop',
    }));
    assert.equal(messages(owner, 'stop').length, 1);
    assert.equal(messages(target, 'stop').length, 1);
    server.handleMessage('owner-connection', envelope(session, {
        type: 'stopped', sceneRunId: 'run-wire-1', digest, success: true,
    }));
    server.handleMessage('target-connection', envelope(session, {
        type: 'stopped', sceneRunId: 'run-wire-1', digest, success: true,
    }));
    assert.equal(server.sceneRuns.get('run-wire-1'), null);
    assert.equal(messages(owner, 'stopped').at(-1).success, true);

    server.handleMessage('owner-connection', {
        protocolVersion: 7, serverBootId: server.serverBootId,
        messageId: crypto.randomUUID(),
        type: 'remote_scene_start',
    });
    assert.equal(messages(owner, 'error').at(-1).code, 'removed_message_type');
}

// An asset from another session generation cannot be prepared.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A');
    const target = addClient(server, 'target-connection', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    session.mediaResidency = { generation: session.generation, sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256, state: 'ready', progress: 1, error: '' }] };
    server.sessionAssets.set(session.remoteSessionId, new Map([[
        asset.assetId,
        { ...asset, remoteSessionId: session.remoteSessionId, generation: 99,
            ownerEndpointId: 'A', targetEndpointId: 'B' },
    ]]));
    const digest = computeSceneDigest(1, [asset], scene);
    server.handleMessage('owner-connection', envelope(session, {
        type: 'scene_prepare', sceneRunId: 'run-invalid-1', revision: 1,
        digest, manifest: [asset], scene,
    }));
    const inventoryError = messages(owner, 'error').at(-1);
    assert.equal(inventoryError.code, 'scene_asset_not_validated');
    assert.equal(inventoryError.message,
        'At least one media file in this scene has not been uploaded to the remote client. Upload all media before launching the scene.');
    assert.equal(inventoryError.remoteSessionId, session.remoteSessionId);
    assert.equal(inventoryError.generation, session.generation);
    assert.equal(inventoryError.sceneRunId, 'run-invalid-1');
    assert.equal(inventoryError.digest, digest);
    assert.equal(messages(target, 'scene_prepare').length, 0);
    assert.equal(server.sceneRuns.get('run-invalid-1'), null);
    assert.equal(server.metrics.value('scene_prepare_failed_total'), 1);
}

// If the owner cannot receive the acceptance barrier, the target must not keep
// a preparation graph alive waiting for an owner that never became accepted.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'ack-owner-connection', 'A');
    const target = addClient(server, 'ack-target-connection', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    session.mediaResidency = { generation: session.generation, sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256, state: 'ready', progress: 1, error: '' }] };
    server.sessionAssets.set(session.remoteSessionId, new Map([[
        asset.assetId,
        { ...asset, remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            ownerEndpointId: 'A', targetEndpointId: 'B', uploadId: 'ack-upload' },
    ]]));
    const digest = computeSceneDigest(1, [asset], scene);
    owner.readyState = WebSocket.CLOSED;
    server.handleMessage('ack-owner-connection', envelope(session, {
        type: 'scene_prepare', sceneRunId: 'run-ack-failure', revision: 1,
        digest, manifest: [asset], scene,
    }));

    const run = server.sceneRuns.get('run-ack-failure');
    assert.equal(messages(target, 'scene_prepare').length, 1);
    assert.equal(run.phase, SCENE_PHASES.STOPPING);
    assert.equal(run.failed, true);
    assert.equal(run.stopReason, 'scene_prepare_ack_delivery_failed');
    assert.equal(messages(target, 'stop').at(-1).reason,
        'scene_prepare_ack_delivery_failed');
}

// Video bounds and signed end offsets survive canonical validation; malformed
// ranges and offsets still fail before any remote preparation is created.
for (const [overrides, accepted] of [
    [{}, true], [{ endPositionMs: 1800 }, true],
    [{ endPositionMs: 1000 }, false], [{ endPositionMs: 999 }, false],
    [{ endPositionMs: -1 }, false], [{ endPositionMs: 1800.5 }, false],
    [{ endPositionMs: '1800' }, false], [{ endPositionMs: 604_800_001 }, false],
    [{ autoHide: true, hideWhenVideoEnds: true, autoHideDelayMs: -250,
        autoMute: true, muteWhenVideoEnds: true, autoMuteDelayMs: -400 }, true],
    [{ autoHideDelayMs: -604_800_001 }, false],
    [{ autoMuteDelayMs: -604_800_001 }, false],
    [{ autoHideDelayMs: -0.5 }, false], [{ autoMuteDelayMs: '-400' }, false],
    [{ autoDisplayDelayMs: -250 }, false], [{ autoPlayDelayMs: -250 }, false],
]) {
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const owner = addClient(server, 'range-owner', 'A');
    const target = addClient(server, 'range-target', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    const videoAsset = { ...asset, extension: 'mp4' };
    const video = {
        ...scene.media[0], type: 'video', fileName: 'clip.mp4',
        autoPlay: true, autoPlayDelayMs: 0, autoPause: false, autoPauseDelayMs: 0,
        muted: true, volume: 1, continuousLoop: true, repeatEnabled: false, repeatCount: 0,
        autoUnmute: false, autoUnmuteDelayMs: 0, autoMute: false, autoMuteDelayMs: 0,
        muteWhenVideoEnds: false, audioFadeInSeconds: 0, audioFadeOutSeconds: 0,
        startPositionMs: 1000,
        ...overrides,
    };
    const rangedScene = { ...scene, media: [video] };
    session.mediaResidency = { generation: session.generation, sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256, state: 'ready', progress: 1, error: '' }] };
    server.sessionAssets.set(session.remoteSessionId, new Map([[
        videoAsset.assetId, { ...videoAsset, remoteSessionId: session.remoteSessionId,
            generation: session.generation, ownerEndpointId: 'A', targetEndpointId: 'B',
            uploadId: 'range-upload' },
    ]]));
    server.handleMessage('range-owner', envelope(session, {
        type: 'scene_prepare', sceneRunId: 'range-run', revision: 1,
        digest: computeSceneDigest(1, [videoAsset], rangedScene),
        manifest: [videoAsset], scene: rangedScene,
    }));
    if (accepted) {
        assert.equal(messages(target, 'scene_prepare').length, 1, JSON.stringify(messages(owner, 'error')));
        const forwarded = messages(target, 'scene_prepare')[0].scene.media[0];
        assert.equal(forwarded.endPositionMs, overrides.endPositionMs);
        assert.equal(forwarded.autoHideDelayMs, video.autoHideDelayMs);
        assert.equal(forwarded.autoMuteDelayMs, video.autoMuteDelayMs);
    } else {
        assert.equal(messages(owner, 'error').at(-1).code, 'invalid_scene_manifest');
        assert.equal(messages(target, 'scene_prepare').length, 0);
        assert.equal(server.sceneRuns.get('range-run'), null);
    }
}

console.log('scene protocol v7 tests passed');

// Residency is a separate, authenticated barrier; upload completion never implies it.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const owner = addClient(server, 'memory-owner', 'A');
    const target = addClient(server, 'memory-target', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    server.sessionAssets.set(session.remoteSessionId, new Map([[
        asset.assetId, { ...asset, remoteSessionId: session.remoteSessionId,
            generation: session.generation, ownerEndpointId: 'A', targetEndpointId: 'B',
            uploadId: 'memory-upload' },
    ]]));
    const prepare = { type: 'scene_prepare', sceneRunId: 'memory-run', revision: 1,
        digest: computeSceneDigest(1, [asset], scene), manifest: [asset], scene };
    server.handleMessage('memory-owner', envelope(session, prepare));
    assert.equal(messages(owner, 'error').at(-1).code, 'scene_memory_unavailable');
    assert.equal(messages(target, 'scene_prepare').length, 0);
    const ready = { type: 'media_residency', sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256,
            state: 'ready', progress: 1, error: '' }] };
    server.handleMessage('memory-owner', envelope(session, ready));
    assert.equal(messages(owner, 'error').at(-1).code, 'not_session_target');
    assert.equal(session.mediaResidency, undefined);
    server.handleMessage('memory-target', envelope(session, ready));
    assert.equal(messages(owner, 'media_residency').length, 1);
    server.handleMessage('memory-target', envelope(session, ready));
    assert.equal(messages(target, 'error').at(-1).code, 'invalid_media_residency');
    server.handleMessage('memory-target', envelope(session, { ...ready, sequence: 2,
        generation: session.generation + 1 }));
    assert.equal(messages(owner, 'media_residency').length, 1);
    server.handleMessage('memory-target', envelope(session, { ...ready, sequence: 2,
        assets: [{ ...ready.assets[0], sha256: 'b'.repeat(64) }] }));
    assert.equal(messages(target, 'error').at(-1).code, 'invalid_media_residency');
    server.handleMessage('memory-owner', envelope(session, prepare));
    assert.equal(messages(target, 'scene_prepare').length, 1);
    server.handleMessage('memory-target', envelope(session, { ...ready, sequence: 2,
        assets: [{ ...ready.assets[0], state: 'waiting_for_memory', progress: 0 }] }));
    assert.equal(server.sceneRuns.get('memory-run').phase, SCENE_PHASES.STOPPING);
    assert.equal(messages(owner, 'stop').at(-1).reason, 'scene_memory_unavailable');
    server.handleMessage('memory-target', envelope(session, { ...ready, sequence: 3,
        protocolVersion: 4 }));
    assert.equal(messages(target, 'error').at(-1).code, 'protocol_version_mismatch');
}

// Offscreen media still belongs to the scene and requires residency, with no render spans.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const offscreen = { ...scene, media: [{ ...scene.media[0], spans: [] }] };
    const required = server.requiredSceneChecklist({ manifest: [asset], scene: offscreen });
    assert.ok(required.some(item => item.stage === 'media_memory_ready'));
    const owner = addClient(server, 'offscreen-owner', 'A');
    const target = addClient(server, 'offscreen-target', 'B');
    const session = server.remoteSessions.open({ ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1 }).session;
    session.serverBootId = server.serverBootId;
    server.sessionAssets.set(session.remoteSessionId, new Map([[asset.assetId,
        { ...asset, remoteSessionId: session.remoteSessionId, generation: session.generation,
            ownerEndpointId: 'A', targetEndpointId: 'B', uploadId: 'offscreen-upload' }]]));
    const request = { type: 'scene_prepare', sceneRunId: 'offscreen-run', revision: 1,
        digest: computeSceneDigest(1, [asset], offscreen), manifest: [asset], scene: offscreen };
    server.handleMessage('offscreen-owner', envelope(session, request));
    assert.equal(messages(owner, 'error').at(-1).code, 'scene_memory_unavailable');
    server.handleMessage('offscreen-target', envelope(session, { type: 'media_residency', sequence: 1,
        assets: [{ assetId: asset.assetId, sha256: asset.sha256, state: 'ready', progress: 1, error: '' }] }));
    server.handleMessage('offscreen-owner', envelope(session, request));
    assert.equal(messages(target, 'scene_prepare').length, 1);
}
