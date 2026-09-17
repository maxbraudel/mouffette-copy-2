'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');
const { computeSceneDigest } = require('./scene_run_registry');

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

function setup() {
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A');
    const target = addClient(server, 'target-connection', 'B');
    const attacker = addClient(server, 'attacker-connection', 'C');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    const asset = {
        assetId: 'asset-1', uploadId: 'upload-1',
        extension: 'png', fileId: 'a'.repeat(64), sha256: 'a'.repeat(64),
        name: 'asset-1.png', mediaIds: ['media-1'], size: 128,
        remoteSessionId: session.remoteSessionId, generation: session.generation,
        ownerEndpointId: 'A', targetEndpointId: 'B', validatedAt: Date.now(),
    };
    server.sessionAssets.set(session.remoteSessionId,
        new Map([[asset.assetId, asset]]));
    server.uploadTombstones.set(asset.uploadId, {
        uploadId: asset.uploadId,
        remoteSessionId: session.remoteSessionId,
        generation: session.generation,
        ownerEndpointId: 'A', targetEndpointId: 'B',
        manifestDigest: 'b'.repeat(64), status: 'finished',
        payload: { type: 'upload_finished' }, expiresAt: Date.now() + 60_000,
    });
    return { server, owner, target, attacker, session, asset };
}

function envelope(context, extra = {}) {
    return {
        protocolVersion: 7,
        serverBootId: context.server.serverBootId,
        messageId: crypto.randomUUID(),
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        ...extra,
    };
}

function removalRequest(context, removalId = crypto.randomUUID(), extra = {}) {
    return envelope(context, {
        type: 'upload_remove',
        removalId,
        uploadId: context.asset.uploadId,
        assetId: context.asset.assetId,
        offset: context.asset.size,
        size: context.asset.size,
        sha256: context.asset.sha256,
        ...extra,
    });
}

function removalAck(context, request, extra = {}) {
    return envelope(context, {
        type: 'upload_removed',
        removalId: request.removalId,
        uploadId: request.uploadId,
        assetId: request.assetId,
        offset: request.offset,
        size: request.size,
        sha256: request.sha256,
        fileId: context.asset.fileId,
        extension: context.asset.extension,
        result: 'committed',
        cacheQuarantined: true,
        ...extra,
    });
}

function messages(ws, type) {
    return ws.messages.filter(message => message.type === type);
}

// upload_remove is an owner -> target command. Its top-level generation is
// therefore A's source transport, never B's recipient transport.
{
    const context = setup();
    context.server.clients.get('target-connection').connectionGeneration = 9;
    context.session.targetConnectionGeneration = 9;
    const request = removalRequest(context);
    context.server.handleMessage('owner-connection', request);
    const relayed = messages(context.target, 'upload_remove').at(-1);
    assert.equal(relayed.connectionGeneration, 1);
}

// A validated target commit is the sole point at which server inventory is
// removed. The exact transaction result is replayed without touching B again.
{
    const context = setup();
    const request = removalRequest(context);
    context.server.handleMessage('owner-connection', request);
    const relayed = messages(context.target, 'upload_remove').at(-1);
    assert.equal(relayed.fileId, context.asset.fileId);
    assert.equal(relayed.extension, 'png');
    assert.equal(relayed.offset, context.asset.size);
    assert.equal(relayed.replay, false);
    assert.equal(context.server.sessionAssets.get(context.session.remoteSessionId)
        .has(context.asset.assetId), true,
    'inventory must remain authoritative until B commits');

    context.server.handleMessage('target-connection', removalAck(context, request, {
        removedFileCount: 1,
        quarantinedBytes: context.asset.size,
    }));
    assert.equal(context.server.sessionAssets.has(context.session.remoteSessionId), false);
    assert.equal(context.server.pendingAssetRemovals.has(request.removalId), false);
    assert.equal(context.server.assetRemovalTombstones.get(request.removalId).status,
        'committed');
    assert.equal(context.server.uploadTombstones.get(context.asset.uploadId).status,
        'asset_removed', 'a completed upload cannot be replayed after one asset is unloaded');
    const committed = messages(context.owner, 'upload_removed').at(-1);
    assert.equal(committed.success, true);
    assert.equal(committed.result, 'committed');

    context.server.handleMessage('owner-connection', {
        ...request,
        messageId: crypto.randomUUID(),
    });
    assert.equal(messages(context.target, 'upload_remove').length, 1,
        'a committed retry must never execute target cleanup twice');
    assert.equal(messages(context.owner, 'upload_removed').at(-1).replay, true);

    context.server.handleMessage('target-connection', {
        ...removalAck(context, request),
        messageId: crypto.randomUUID(),
    });
    assert.equal(messages(context.owner, 'upload_removed').at(-1).replay, true,
        'a duplicate target commit replays the same terminal result');
}

// Socket identity, both generations and every immutable inventory field are
// checked before a request or acknowledgement may mutate state.
{
    const context = setup();
    const request = removalRequest(context);
    context.server.handleMessage('attacker-connection', request);
    assert.equal(messages(context.attacker, 'error').at(-1).code,
        'not_a_session_party');
    assert.equal(messages(context.target, 'upload_remove').length, 0);

    context.server.handleMessage('owner-connection', {
        ...request,
        messageId: crypto.randomUUID(),
        sha256: 'c'.repeat(64),
    });
    const correlatedError = messages(context.owner, 'error').at(-1);
    assert.equal(correlatedError.code,
        'asset_inventory_mismatch');
    assert.equal(correlatedError.remoteSessionId, request.remoteSessionId);
    assert.equal(correlatedError.generation, request.generation);
    assert.equal(correlatedError.connectionGeneration, 1);
    assert.equal(correlatedError.removalId, request.removalId);
    assert.equal(correlatedError.uploadId, request.uploadId);
    assert.equal(correlatedError.assetId, request.assetId);
    assert.equal(correlatedError.offset, request.offset);
    assert.equal(correlatedError.size, request.size);
    assert.equal(correlatedError.sha256, 'c'.repeat(64));
    assert.equal(correlatedError.ownerEndpointId, 'A');
    assert.equal(correlatedError.targetEndpointId, 'B');
    assert.equal(messages(context.target, 'upload_remove').length, 0);

    for (const mutation of [
        { uploadId: 'upload-other' },
        { assetId: 'asset-other' },
        { offset: context.asset.size - 1 },
        { offset: context.asset.size + 1, size: context.asset.size + 1 },
    ]) {
        context.server.handleMessage('owner-connection', {
            ...request,
            ...mutation,
            messageId: crypto.randomUUID(),
        });
        assert.equal(messages(context.target, 'upload_remove').length, 0);
    }

    context.server.handleMessage('owner-connection', {
        ...request,
        messageId: crypto.randomUUID(),
        connectionGeneration: 2,
    });
    assert.equal(messages(context.owner, 'error').at(-1).code,
        'stale_connection_generation');

    context.server.handleMessage('owner-connection', {
        ...request,
        messageId: crypto.randomUUID(),
        generation: context.session.generation + 1,
    });
    assert.equal(messages(context.owner, 'error').at(-1).code,
        'stale_remote_session_generation');

    context.server.handleMessage('owner-connection', request);
    context.server.handleMessage('target-connection', removalAck(context, request, {
        sha256: 'd'.repeat(64),
    }));
    assert.equal(messages(context.target, 'error').at(-1).code,
        'asset_removal_ack_mismatch');
    assert.equal(context.server.sessionAssets.get(context.session.remoteSessionId)
        .has(context.asset.assetId), true);
    assert.equal(context.server.pendingAssetRemovals.has(request.removalId), true);
}

// A SceneRun that references the asset is stopped completely before B receives
// the unload request. An unrelated SceneRun does not create this dependency.
{
    const context = setup();
    const manifest = [{
        assetId: context.asset.assetId,
        extension: context.asset.extension,
        fileId: context.asset.fileId,
        mediaIds: context.asset.mediaIds,
        sha256: context.asset.sha256,
        size: context.asset.size,
    }];
    const scene = {
        screens: [{ id: 'screen-1' }],
        media: [{ mediaId: 'media-1', assetId: 'asset-1', type: 'image' }],
    };
    const digest = computeSceneDigest(1, manifest, scene);
    context.server.sceneRuns.prepare({
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        sceneRunId: 'run-removal-1', revision: 1, digest, manifest, scene,
        ownerEndpointId: 'A', targetEndpointId: 'B',
    });
    context.session.sceneRunId = 'run-removal-1';
    const request = removalRequest(context);
    context.server.handleMessage('owner-connection', request);
    assert.equal(messages(context.owner, 'stop').length, 1);
    assert.equal(messages(context.target, 'stop').length, 1);
    assert.equal(messages(context.target, 'upload_remove').length, 0,
        'target cleanup must wait for both render graphs to stop');

    for (const connectionId of ['owner-connection', 'target-connection']) {
        context.server.handleMessage(connectionId, envelope(context, {
            type: 'stopped', sceneRunId: 'run-removal-1', digest, success: true,
        }));
    }
    assert.equal(context.server.sceneRuns.get('run-removal-1'), null);
    assert.equal(messages(context.target, 'upload_remove').length, 1);
}

// A target-side cleanup failure is terminal for this removal ID but keeps the
// validated inventory. Retrying the same ID only replays that exact outcome.
{
    const context = setup();
    const request = removalRequest(context);
    context.server.handleMessage('owner-connection', request);
    context.server.handleMessage('target-connection', removalAck(context, request, {
        result: 'cleanup_error',
        cacheQuarantined: false,
        errorCode: 'quarantine_failed',
        reason: 'Unable to quarantine the cache entry',
    }));
    assert.equal(context.server.sessionAssets.get(context.session.remoteSessionId)
        .has(context.asset.assetId), true);
    assert.equal(messages(context.owner, 'upload_removed').at(-1).success, false);
    assert.equal(messages(context.owner, 'upload_removed').at(-1).code,
        'quarantine_failed');
    assert.equal(context.session.phase, 'CleanupPending',
        'cleanup_error must keep B unavailable pending whole-session cleanup');
    assert.equal(messages(context.target, 'remote_session_terminating').length, 1);
    context.server.handleMessage('owner-connection', {
        ...request,
        messageId: crypto.randomUUID(),
    });
    assert.equal(messages(context.target, 'upload_remove').length, 1,
        'a terminal session cannot redispatch targeted cleanup');
    assert.equal(messages(context.owner, 'error').at(-1).code,
        'remote_session_not_active');
}

// A render-graph stop timeout must not fall through into file removal. The
// whole RemoteSession teardown is the only safe reconciliation path.
{
    const context = setup();
    const manifest = [{
        assetId: context.asset.assetId,
        extension: context.asset.extension,
        fileId: context.asset.fileId,
        mediaIds: context.asset.mediaIds,
        sha256: context.asset.sha256,
        size: context.asset.size,
    }];
    const scene = {
        screens: [{ id: 'screen-1' }],
        media: [{ mediaId: 'media-1', assetId: 'asset-1', type: 'image' }],
    };
    const digest = computeSceneDigest(1, manifest, scene);
    context.server.sceneRuns.prepare({
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        sceneRunId: 'run-removal-timeout', revision: 1, digest, manifest, scene,
        ownerEndpointId: 'A', targetEndpointId: 'B',
    });
    context.session.sceneRunId = 'run-removal-timeout';
    const request = removalRequest(context);
    context.server.handleMessage('owner-connection', request);
    const run = context.server.sceneRuns.get('run-removal-timeout');
    context.server.sweepSceneRuns(
        run.stopDeadlineAt, run.stopDeadlineServerMonotonicMs);
    assert.equal(messages(context.target, 'upload_remove').length, 0);
    assert.equal(messages(context.owner, 'upload_removed').at(-1).code,
        'scene_stop_timeout');
    assert.equal(context.session.phase, 'CleanupPending');
    assert.equal(context.server.sessionAssets.get(context.session.remoteSessionId)
        .has(context.asset.assetId), true);
}

// The fixed acknowledgement deadline is not extended by retries. Timeout
// keeps inventory intact and closes the session so whole-cache teardown can
// reconcile an ACK-lost target safely.
{
    const context = setup();
    const request = removalRequest(context);
    context.server.handleMessage('owner-connection', request);
    const pending = context.server.pendingAssetRemovals.get(request.removalId);
    const fixedDeadline = pending.deadlineAt;
    context.server.handleMessage('owner-connection', {
        ...request,
        messageId: crypto.randomUUID(),
    });
    assert.equal(pending.deadlineAt, fixedDeadline);
    assert.equal(messages(context.target, 'upload_remove').at(-1).replay, true);

    context.server.sweepAssetRemovals(fixedDeadline);
    assert.equal(context.server.pendingAssetRemovals.has(request.removalId), false);
    assert.equal(context.server.sessionAssets.get(context.session.remoteSessionId)
        .has(context.asset.assetId), true);
    assert.equal(messages(context.owner, 'upload_removed').at(-1).result, 'timeout');
    assert.equal(context.session.phase, 'CleanupPending');
    assert.equal(messages(context.target, 'remote_session_terminating').length, 1);
}

// A disk worker can finish after a resume. Its immutable operation proof must
// settle on the new authenticated transport without pretending to apply the
// new session epoch; duplicate proof remains valid after logical closure.
{
    const context = setup();
    const { server, session } = context;
    const request = removalRequest(context);
    server.handleMessage('owner-connection', request);
    const originalAck = removalAck(context, request);
    ++session.generation;
    server.clients.get('target-connection').connectionGeneration = 2;
    session.targetConnectionGeneration = 2;
    server.rebindSessionGeneration(session);
    server.handleMessage('target-connection', { ...originalAck, connectionGeneration: 2 });
    assert.equal(server.pendingAssetRemovals.size, 0);
    assert.equal(server.sessionAssets.has(session.remoteSessionId), false);
    assert.equal(messages(context.owner, 'upload_removed').at(-1).success, true);
    assert.equal(messages(context.owner, 'upload_removed').at(-1).generation, session.generation);

    server.remoteSessions.terminate(session.remoteSessionId, 'test_closed');
    server.beginRemoteSessionTeardown(session);
    server.handleMessage('target-connection', {
        ...originalAck, connectionGeneration: 2, messageId: crypto.randomUUID(),
    });
    assert.equal(messages(context.owner, 'upload_removed').at(-1).replay, true);
    const before = messages(context.owner, 'upload_removed').length;
    server.handleMessage('target-connection', { ...originalAck, messageId: crypto.randomUUID() });
    assert.equal(messages(context.target, 'error').at(-1).code, 'stale_connection_generation');
    server.handleMessage('attacker-connection', { ...originalAck, messageId: crypto.randomUUID() });
    assert.equal(messages(context.owner, 'upload_removed').length, before);
}

console.log('asset removal protocol v7 tests passed');
