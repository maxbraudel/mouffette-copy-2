'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');
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
    const client = {
        id: connectionId,
        sessionId: connectionId,
        persistentId: endpointId,
        endpointId,
        runtimeId: `runtime-${endpointId}`,
        connectionGeneration: 1,
        authenticated: true,
        machineName: endpointId,
        ws,
    };
    server.clients.set(connectionId, client);
    if (client.authenticated) server.currentTransportByEndpoint.set(endpointId, client);
    return { client, ws };
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
    return { server, owner, target, attacker, session };
}

function envelope(session, extra = {}) {
    return {
        protocolVersion: 8,
        serverBootId: session.serverBootId,
        messageId: crypto.randomUUID(),
        remoteSessionId: session.remoteSessionId,
        generation: session.generation,
        connectionGeneration: 1,
        ...extra,
    };
}

function file(assetId = 'asset-1', hash = 'a'.repeat(64), mediaId = 'media-1') {
    return {
        assetId, fileId: hash, sha256: hash, name: `${assetId}.png`,
        extension: 'png', size: 128, mediaIds: [mediaId],
    };
}

function assetState(asset = file(), offset = 0) {
    return {
        assetId: asset.assetId,
        offset,
        size: asset.size,
        sha256: asset.sha256,
    };
}

function messages(ws, type) {
    return ws.messages.filter(message => message.type === type);
}

function startUpload(context, uploadId, asset = file(), transport = socket()) {
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_start', uploadId, files: [asset],
    }), transport);
    return transport;
}

const uploadId = 'upload-1';

// Eight 1 MiB durable-ACK windows bound total incoming bytes. Further senders
// wait before target allocation and are granted fairly when a slot is released.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {}, protocolLogger: () => {} });
    const target = addClient(server, 'bounded-target', 'B');
    const senders = [];
    const asset = { ...file(), size: 2 * 1024 * 1024 };
    for (let index = 0; index < 9; ++index) {
        const id = `bounded-owner-${index}`;
        const owner = addClient(server, id, `A-${index}`);
        const session = server.remoteSessions.open({
            ownerEndpointId: owner.client.endpointId, targetEndpointId: 'B',
            ownerRuntimeId: owner.client.runtimeId, targetRuntimeId: 'runtime-B',
        }).session;
        session.serverBootId = server.serverBootId;
        const transport = socket();
        server.handleMessage(id, envelope(session, { type: 'upload_start',
            uploadId: `bounded-upload-${index}`, files: [asset] }), transport);
        senders.push({ id, owner, session, transport, uploadId: `bounded-upload-${index}` });
    }
    assert.equal(messages(target.ws, 'upload_start').length, 8);
    assert.equal(messages(senders[8].owner.ws, 'upload_resume_ready').at(-1).waitingForCapacity, true);
    server.cleanupStalledUploads(Date.now() + 5000);
    assert.equal(server.uploads.size, 9, 'accepted capacity waits are not idle failures');
    const first = senders[0];
    server.handleMessage('bounded-target', envelope(first.session, { type: 'upload_ready',
        uploadId: first.uploadId, assets: [assetState(asset)] }));
    const chunk = Buffer.alloc(128 * 1024).toString('base64');
    for (let index = 0; index < 8; ++index) {
        server.handleMessage(first.id, envelope(first.session, { type: 'upload_chunk',
            uploadId: first.uploadId, assetId: asset.assetId, sha256: asset.sha256,
            offset: index * 128 * 1024, size: 128 * 1024, data: chunk }), first.transport);
    }
    assert.equal(server.uploads.get(first.uploadId).relayedBytes, 1024 * 1024);
    server.handleMessage('bounded-target', envelope(first.session, { type: 'upload_progress',
        uploadId: first.uploadId, assets: [assetState(asset, 1024 * 1024)] }));
    server.handleMessage(first.id, envelope(first.session, { type: 'upload_chunk',
        uploadId: first.uploadId, assetId: asset.assetId, sha256: asset.sha256,
        offset: 1024 * 1024, size: 128 * 1024, data: chunk }), first.transport);
    assert.equal(server.uploads.get(first.uploadId).relayedBytes, 1152 * 1024,
        'durable progress reopens credit without failing the transfer');
    server.removeUpload(server.uploads.get(first.uploadId));
    assert.equal(messages(target.ws, 'upload_start').length, 9);
    assert.equal(server.uploads.get(senders[8].uploadId).relaySlotGranted, true);
}

// Validated upload is scoped to the exact session generation and creates the
// only inventory that scene_prepare may consume.
{
    const context = setup();
    const transport = startUpload(context, uploadId);
    assert.equal(context.server.uploads.get(uploadId).remoteSessionId,
        context.session.remoteSessionId);
    assert.equal(messages(context.target.ws, 'upload_start').length, 1);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_ready', uploadId, assets: [assetState()],
    }));
    const data = Buffer.alloc(128, 0x5a).toString('base64');
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_chunk', uploadId, assetId: 'asset-1',
        offset: 0, size: 128, sha256: 'a'.repeat(64), data,
    }), transport);
    assert.equal(messages(context.target.ws, 'upload_chunk').at(-1).offset, 0);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_progress', uploadId,
        assets: [{ assetId: 'asset-1', offset: 128, size: 128, sha256: 'a'.repeat(64) }],
    }));
    assert.equal(messages(context.owner.ws, 'upload_progress').at(-1).durableBytes, 128);
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_complete', uploadId, assets: [assetState(file(), 128)],
    }), transport);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_finished', uploadId,
        assets: [{ assetId: 'asset-1', offset: 128, size: 128, sha256: 'a'.repeat(64) }],
    }));
    assert.equal(context.server.uploads.has(uploadId), false);
    const stored = context.server.sessionAssets.get(context.session.remoteSessionId)
        .get('asset-1');
    assert.equal(stored.generation, context.session.generation);
    assert.equal(stored.ownerEndpointId, 'A');
    assert.equal(messages(context.owner.ws, 'upload_finished').length, 1);
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_start', uploadId, files: [file()],
    }), transport);
    assert.equal(messages(context.owner.ws, 'upload_finished').at(-1).replay, true);
    assert.equal(messages(context.target.ws, 'upload_start').length, 1,
        'a terminal upload retry must not recreate target staging');
}

// If B promoted the upload but upload_finished was lost, an exact upload_start
// replay asks B to repeat only the terminal validation.  No staging is reopened,
// and both the active transfer and its terminal tombstone advance to the
// authenticated owner connection generation on resume.
{
    const context = setup();
    const initialTransport = startUpload(context, uploadId);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_ready', uploadId, assets: [assetState()],
    }));
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_chunk', uploadId, assetId: 'asset-1',
        offset: 0, size: 128, sha256: 'a'.repeat(64),
        data: Buffer.alloc(128, 0x41).toString('base64'),
    }), initialTransport);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_progress', uploadId, assets: [assetState(file(), 128)],
    }));
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_complete', uploadId, assets: [assetState(file(), 128)],
    }), initialTransport);
    assert.equal(context.server.uploads.get(uploadId).awaitingTargetValidation, true);
    assert.equal(messages(context.target.ws, 'upload_complete').length, 1);

    context.session.generation = 2;
    context.owner.client.connectionGeneration = 2;
    context.session.ownerConnectionGeneration = 2;
    context.server.rebindSessionGeneration(context.session);
    const resumedTransport = socket();
    context.server.handleMessage('owner-connection', {
        ...envelope(context.session, {
            type: 'upload_start', uploadId, files: [file()],
        }),
        connectionGeneration: 2,
    }, resumedTransport);
    const completionReplay = messages(context.target.ws, 'upload_complete').at(-1);
    assert.equal(completionReplay.replay, true);
    assert.equal(completionReplay.generation, 2);
    assert.equal(completionReplay.connectionGeneration, 2);
    assert.deepEqual(completionReplay.assets, [assetState(file(), 128)]);
    assert.equal(messages(context.target.ws, 'upload_start').length, 1,
        'lost final ACK recovery must not recreate target staging');
    assert.equal(messages(context.owner.ws, 'upload_resume_ready').at(-1).replay, true);

    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_finished', uploadId, assets: [assetState(file(), 128)],
    }));
    assert.equal(context.server.uploads.has(uploadId), false);
    const recovered = messages(context.owner.ws, 'upload_finished').at(-1);
    assert.equal(recovered.generation, 2);
    assert.equal(recovered.connectionGeneration, 2);
    assert.equal(context.server.sessionAssets.get(context.session.remoteSessionId)
        .get('asset-1').generation, 2);

    context.session.generation = 3;
    context.owner.client.connectionGeneration = 3;
    context.session.ownerConnectionGeneration = 3;
    context.server.rebindSessionGeneration(context.session);
    context.server.handleMessage('owner-connection', {
        ...envelope(context.session, {
            type: 'upload_start', uploadId, files: [file()],
        }),
        connectionGeneration: 3,
    }, socket());
    const terminalReplay = messages(context.owner.ws, 'upload_finished').at(-1);
    assert.equal(terminalReplay.replay, true);
    assert.equal(terminalReplay.generation, 3);
    assert.equal(terminalReplay.connectionGeneration, 3,
        'terminal replay must use the resumed owner transport generation');
    assert.equal(messages(context.target.ws, 'upload_complete').length, 2,
        'a committed terminal replay must not contact B again');
}

// An active uploadId is an immutable idempotency key. Only an exact replay of
// the same authenticated session/runtime/generation and normalized manifest is
// accepted; changed asset metadata cannot inherit its offsets.
{
    const context = setup();
    const original = file();
    const transport = startUpload(context, uploadId, original);
    const originalDigest = context.server.uploads.get(uploadId).manifestDigest;

    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_start', uploadId, files: [structuredClone(original)],
    }), transport);
    assert.equal(messages(context.owner.ws, 'upload_resume_ready').at(-1).replay, true);

    const changed = file('asset-1', 'b'.repeat(64), 'media-1');
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_start', uploadId, files: [changed],
    }), transport);
    assert.equal(messages(context.owner.ws, 'upload_rejected').at(-1).code,
        'upload_id_reused');
    assert.equal(context.server.uploads.get(uploadId).manifestDigest, originalDigest);
    assert.equal(messages(context.target.ws, 'upload_start').length, 1,
        'a conflicting active upload replay must never reach the target');

    const otherSession = context.server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'C',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-C',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    otherSession.serverBootId = context.server.serverBootId;
    context.server.handleMessage('owner-connection', envelope(otherSession, {
        type: 'upload_start', uploadId, files: [structuredClone(original)],
    }), socket());
    assert.equal(messages(context.owner.ws, 'upload_rejected').at(-1).code,
        'upload_id_reused',
        'an identical manifest cannot reuse an active uploadId in another session tuple');
}

// READY proves the target's complete durable inventory, not merely possession
// of the uploadId. Missing, duplicate, unknown or altered tuple fields abort
// the transfer before the owner may stream bytes.
{
    const first = file('asset-ready-1', '1'.repeat(64), 'media-ready-1');
    const second = file('asset-ready-2', '2'.repeat(64), 'media-ready-2');
    const valid = [assetState(first), assetState(second)];
    const invalidInventories = [
        [valid[0]],
        [valid[0], valid[0]],
        [valid[0], { ...valid[1], assetId: 'asset-unknown' }],
        [valid[0], { ...valid[1], size: valid[1].size + 1 }],
        [valid[0], { ...valid[1], sha256: '3'.repeat(64) }],
        [valid[0], { ...valid[1], offset: 1 }],
    ];
    invalidInventories.forEach((assets, index) => {
        const context = setup();
        const currentUploadId = `invalid-ready-${index}`;
        context.server.handleMessage('owner-connection', envelope(context.session, {
            type: 'upload_start', uploadId: currentUploadId, files: [first, second],
        }), socket());
        context.server.handleMessage('target-connection', envelope(context.session, {
            type: 'upload_ready', uploadId: currentUploadId, assets,
        }));
        assert.equal(messages(context.owner.ws, 'upload_rejected').at(-1).code,
            'invalid_upload_ready_inventory');
        assert.equal(context.server.uploads.has(currentUploadId), false);
    });
}

// Durable progress uses the same complete, unique inventory contract, so a
// partial acknowledgement cannot silently advance only selected assets.
{
    const context = setup();
    const first = file('asset-progress-1', '4'.repeat(64), 'media-progress-1');
    const second = file('asset-progress-2', '5'.repeat(64), 'media-progress-2');
    const currentUploadId = 'invalid-progress-inventory';
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_start', uploadId: currentUploadId, files: [first, second],
    }), socket());
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_ready', uploadId: currentUploadId,
        assets: [assetState(first), assetState(second)],
    }));
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_progress', uploadId: currentUploadId,
        assets: [assetState(first)],
    }));
    assert.equal(messages(context.owner.ws, 'upload_rejected').at(-1).code,
        'invalid_upload_progress_inventory');
    assert.equal(context.server.uploads.has(currentUploadId), false);
}

// Completion repeats the immutable asset tuple and proves that every byte is
// present.  Omitting or altering that inventory aborts before B may validate
// or promote anything.
{
    const invalidInventories = [
        undefined,
        [],
        [{ ...assetState(file(), 128), size: 129 }],
        [{ ...assetState(file(), 128), sha256: 'b'.repeat(64) }],
        [{ ...assetState(file(), 128), offset: 127 }],
    ];
    invalidInventories.forEach((assets, index) => {
        const context = setup();
        const currentUploadId = `invalid-complete-${index}`;
        const transport = startUpload(context, currentUploadId);
        context.server.handleMessage('target-connection', envelope(context.session, {
            type: 'upload_ready', uploadId: currentUploadId, assets: [assetState()],
        }));
        context.server.handleMessage('owner-connection', envelope(context.session, {
            type: 'upload_chunk', uploadId: currentUploadId, assetId: 'asset-1',
            offset: 0, size: 128, sha256: 'a'.repeat(64),
            data: Buffer.alloc(128, 0x42).toString('base64'),
        }), transport);
        const completion = { type: 'upload_complete', uploadId: currentUploadId };
        if (assets !== undefined) completion.assets = assets;
        context.server.handleMessage('owner-connection',
            envelope(context.session, completion), transport);
        assert.equal(messages(context.owner.ws, 'upload_rejected').at(-1).code,
            'invalid_upload_completion_inventory');
        assert.equal(context.server.uploads.has(currentUploadId), false);
        assert.equal(messages(context.target.ws, 'upload_complete').length, 0);
    });
}

// Completion is a commit barrier: relaying the final chunk is insufficient.
// Keep an older sender's request pending until B reports every byte durable.
{
    const context = setup();
    const prematureUploadId = 'completion-before-durable-ack';
    const transport = startUpload(context, prematureUploadId);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_ready', uploadId: prematureUploadId, assets: [assetState()],
    }));
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_chunk', uploadId: prematureUploadId,
        assetId: 'asset-1', offset: 0, size: 128,
        sha256: 'a'.repeat(64), data: Buffer.alloc(128, 0x44).toString('base64'),
    }), transport);
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_complete', uploadId: prematureUploadId,
        assets: [assetState(file(), 128)],
    }), transport);
    assert.equal(messages(context.target.ws, 'upload_complete').length, 0);
    assert.equal(context.server.uploads.get(prematureUploadId).completionRequested, true);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_progress', uploadId: prematureUploadId,
        assets: [assetState(file(), 128)],
    }));
    assert.equal(messages(context.target.ws, 'upload_complete').length, 1);
    assert.equal(context.server.uploads.get(prematureUploadId)
        .awaitingTargetValidation, true);
    assert.equal(context.server.uploads.get(prematureUploadId).completionRequested, false);
}

// Even after the first valid completion entered target validation, a duplicate
// completion may only replay the same immutable inventory.
{
    const context = setup();
    const transport = startUpload(context, 'conflicting-complete-replay');
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_ready', uploadId: 'conflicting-complete-replay',
        assets: [assetState()],
    }));
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_chunk', uploadId: 'conflicting-complete-replay',
        assetId: 'asset-1', offset: 0, size: 128, sha256: 'a'.repeat(64),
        data: Buffer.alloc(128, 0x43).toString('base64'),
    }), transport);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_progress', uploadId: 'conflicting-complete-replay',
        assets: [assetState(file(), 128)],
    }));
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_complete', uploadId: 'conflicting-complete-replay',
        assets: [assetState(file(), 128)],
    }), transport);
    assert.equal(context.server.uploads.get('conflicting-complete-replay')
        .awaitingTargetValidation, true);
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_complete', uploadId: 'conflicting-complete-replay',
        assets: [{ ...assetState(file(), 128), offset: 127 }],
    }), transport);
    assert.equal(messages(context.owner.ws, 'upload_rejected').at(-1).code,
        'invalid_upload_completion_inventory');
    assert.equal(context.server.uploads.has('conflicting-complete-replay'), false);
    assert.equal(messages(context.target.ws, 'upload_complete').length, 1);
}

// Sender spoofing, stale generations and disallowed formats never reach B.
{
    const context = setup();
    context.server.handleMessage('attacker-connection', envelope(context.session, {
        type: 'upload_start', uploadId, files: [file()],
    }), socket());
    assert.equal(messages(context.attacker.ws, 'upload_rejected').at(-1).code,
        'not_a_session_party');
    context.server.handleMessage('owner-connection', {
        ...envelope(context.session, { type: 'upload_start', uploadId, files: [file()] }),
        generation: context.session.generation + 1,
    }, socket());
    assert.equal(messages(context.owner.ws, 'upload_rejected').at(-1).code,
        'stale_remote_session_generation');
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_start', uploadId,
        files: [{ ...file(), name: 'payload.exe', extension: 'exe' }],
    }), socket());
    assert.equal(messages(context.owner.ws, 'upload_rejected').at(-1).code,
        'invalid_upload_asset_metadata');
    assert.equal(messages(context.target.ws, 'upload_start').length, 0);
}

// Two global transfers are permitted for an owner, but only one per session;
// a third independent session is rejected by the server-side guard.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A');
    const targets = ['B', 'C', 'D'].map(endpointId =>
        addClient(server, `target-${endpointId}`, endpointId));
    const sessions = targets.map(({ client }) => server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: client.endpointId,
        ownerRuntimeId: 'runtime-A', targetRuntimeId: client.runtimeId,
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session);
    sessions.forEach(session => { session.serverBootId = server.serverBootId; });
    for (let index = 0; index < 3; ++index) {
        server.handleMessage('owner-connection', envelope(sessions[index], {
            type: 'upload_start', uploadId: `upload-${index + 1}`,
            files: [file(`asset-${index + 1}`, String(index + 1).repeat(64), `media-${index + 1}`)],
        }), socket());
    }
    assert.equal(server.uploads.size, 2);
    assert.equal(messages(owner.ws, 'upload_rejected').at(-1).code,
        'upload_concurrency_exceeded');

    server.handleMessage('owner-connection', envelope(sessions[0], {
        type: 'upload_start', uploadId: 'upload-same-session',
        files: [file('asset-extra', 'e'.repeat(64), 'media-extra')],
    }), socket());
    assert.equal(messages(owner.ws, 'upload_rejected').at(-1).code,
        'upload_session_busy');
}

// Resume rewinds relayed-but-unacknowledged data to the last contiguous durable
// offset and rebinds transport/generation without changing immutable metadata.
{
    const context = setup();
    const firstTransport = startUpload(context, uploadId);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_ready', uploadId, assets: [assetState()],
    }));
    context.server.handleMessage('owner-connection', envelope(context.session, {
        type: 'upload_chunk', uploadId, assetId: 'asset-1', offset: 0, size: 64,
        sha256: 'a'.repeat(64), data: Buffer.alloc(64, 0x31).toString('base64'),
    }), firstTransport);
    context.server.handleMessage('target-connection', envelope(context.session, {
        type: 'upload_progress', uploadId,
        assets: [{ assetId: 'asset-1', offset: 32, size: 128, sha256: 'a'.repeat(64) }],
    }));
    context.session.generation = 2;
    context.owner.client.connectionGeneration = 2;
    context.session.ownerConnectionGeneration = 2;
    context.server.rebindSessionGeneration(context.session);
    assert.equal(context.server.uploads.get(uploadId).ownerConnectionGeneration, 2,
        'the immutable upload binding advances only through authenticated session resume');
    const secondTransport = socket();
    context.server.handleMessage('owner-connection', {
        ...envelope(context.session, { type: 'upload_resume', uploadId }),
        connectionGeneration: 2,
    }, secondTransport);
    const resumed = messages(context.owner.ws, 'upload_resume_ready').at(-1);
    assert.equal(resumed.assets[0].offset, 32);
    assert.equal(context.server.uploads.get(uploadId).assetStates.get('asset-1').nextOffset, 32);
    assert.equal(context.server.uploads.get(uploadId).transportSocket, secondTransport);
}

// The fixed RemoteSession lease is checked synchronously by every upload
// command. A command at 2,999 ms is accepted; one at exactly 3,000 ms first
// terminalizes the session and cannot relay or mutate upload data.
{
    let now = 70_000;
    let sequence = 0;
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    server.remoteSessions = new RemoteSessionRegistry({
        leaseTimeoutMs: 3_000,
        now: () => now,
        idFactory: () => `upload-lease-${++sequence}`,
    });
    const owner = addClient(server, 'owner-connection', 'A');
    const target = addClient(server, 'target-connection', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    session.serverBootId = server.serverBootId;
    const transport = socket();

    now = 72_999;
    server.handleMessage('owner-connection', envelope(session, {
        type: 'upload_start', uploadId: 'lease-upload', files: [file()],
    }), transport);
    assert.equal(server.uploads.has('lease-upload'), true);
    assert.equal(messages(target.ws, 'upload_start').length, 1);

    now = 73_000;
    server.handleMessage('owner-connection', envelope(session, {
        type: 'upload_chunk', uploadId: 'lease-upload', assetId: 'asset-1',
        offset: 0, size: 1, sha256: 'a'.repeat(64),
        data: Buffer.from([1]).toString('base64'),
    }), transport);
    assert.equal(session.phase, 'CleanupPending');
    assert.equal(server.uploads.has('lease-upload'), false);
    assert.equal(messages(target.ws, 'upload_chunk').length, 0);
    const rejection = messages(owner.ws, 'upload_rejected').at(-1);
    assert.equal(rejection.code, 'lease_expired');
    assert.equal(rejection.remoteSessionId, session.remoteSessionId);
    assert.equal(rejection.generation, session.generation);
    assert.equal(rejection.ownerEndpointId, owner.client.endpointId);
    assert.equal(rejection.targetEndpointId, target.client.endpointId);
    const terminalAbort = messages(target.ws, 'upload_abort').at(-1);
    assert.equal(terminalAbort.connectionGeneration, 1,
        'server-generated upload aborts remain bound to the upload owner/source');
    assert.equal(terminalAbort.remoteSessionId, session.remoteSessionId);
    assert.equal(terminalAbort.generation, session.generation);
    assert.equal(terminalAbort.ownerEndpointId, owner.client.endpointId);
    assert.equal(terminalAbort.targetEndpointId, target.client.endpointId);
}

// A server-generated abort relayed owner -> target retains the owner's source
// generation even when B is on a different transport generation.
{
    const context = setup();
    context.target.client.connectionGeneration = 7;
    context.session.targetConnectionGeneration = 7;
    startUpload(context, 'source-bound-abort');
    const start = messages(context.target.ws, 'upload_start').at(-1);
    assert.equal(start.connectionGeneration, 1);
    context.server.abortUploadsForRemoteSession(context.session, 'test_abort');
    const abort = messages(context.target.ws, 'upload_abort').at(-1);
    assert.equal(abort.connectionGeneration, 1,
        'the recipient generation must not overwrite the upload owner/source generation');
}

console.log('upload protocol v8 tests passed');
