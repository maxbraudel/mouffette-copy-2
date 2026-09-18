'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { MouffetteServer } = require('./server');
const { SCENE_PHASES } = require('./scene_run_registry');

function socket() {
    return { readyState: 1, bufferedAmount: 0, messages: [],
        send(raw) { this.messages.push(JSON.parse(raw)); }, close() { this.readyState = 3; } };
}
function fixture() {
    let now = 100000;
    const server = new MouffetteServer({ port: 0, monotonicNow: () => now,
        epochNow: () => now, protocolLogger: () => {}, metricLogger: () => {} });
    const add = id => {
        const peer = { id, endpointId: id, runtimeId: `runtime-${id}`, connectionGeneration: 1,
            authenticated: true, machineName: id, ws: socket(), data: socket(),
            lastHeartbeatMonotonicAt: now };
        server.clients.set(id, peer); server.currentTransportByEndpoint.set(id, peer);
        server.registerUploadSocket(peer, peer.data); return peer;
    };
    const owner = add('A'), target = add('B');
    const open = (a = owner, b = target) => server.remoteSessions.open({
        ownerEndpointId: a.endpointId, targetEndpointId: b.endpointId,
        ownerRuntimeId: a.runtimeId, targetRuntimeId: b.runtimeId }).session;
    const session = open();
    const send = (peer, type, fields = {}, data = server.uploadDataMessageTypes.has(type), binding = session) => {
        server.handleMessage(peer.id, { type, protocolVersion: 12, serverBootId: server.serverBootId,
            messageId: crypto.randomUUID(), connectionGeneration: peer.connectionGeneration,
            remoteSessionId: binding.remoteSessionId, generation: binding.generation, ...fields },
        data ? peer.data : null);
    };
    const asset = (id = 'one') => ({ assetId: id, fileId: 'a'.repeat(64), sha256: 'a'.repeat(64),
        extension: 'png', name: 'test.png', size: 128 * 1024, mediaIds: [`media-${id}`] });
    const state = (file, offset = 0) => ({ assetId: file.assetId, sha256: file.sha256, size: file.size, offset });
    const latest = (peer, type, data = true) => (data ? peer.data : peer.ws).messages.filter(m => m.type === type).at(-1);
    return { server, owner, target, session, add, open, send, asset, state, latest,
        advance(value) { now = value; }, now: () => now };
}

// Two missed heartbeats degrade; only five seconds fence the transport. Neither
// repeated detection nor a later socket close renews the 15-second recovery.
{
    const f = fixture();
    const run = f.server.sceneRuns.prepare({ remoteSessionId: f.session.remoteSessionId,
        generation: 1, sceneRunId: 'live', revision: 1, digest: 'a'.repeat(64), manifest: [],
        scene: {}, ownerEndpointId: 'A', targetEndpointId: 'B' }).run;
    run.phase = SCENE_PHASES.LIVE;
    f.advance(101500); f.server.sweepRemoteSessionLeases();
    assert.equal(f.owner.ws.readyState, 1);
    assert.equal(f.session.degradedEndpoints.size, 2);
    assert.equal(f.session.graceDeadlineAt, 116500);
    assert.equal(run.phase, SCENE_PHASES.LIVE);
    f.advance(105000); f.server.sweepRemoteSessionLeases();
    assert.equal(f.owner.ws.readyState, 3);
    assert.equal(f.session.graceDeadlineAt, 116500);
    f.advance(116499); f.server.sweepRemoteSessionLeases();
    assert.equal(run.phase, SCENE_PHASES.LIVE);
    f.advance(116500); f.server.sweepRemoteSessionLeases();
    assert.equal(f.session.phase, 'CleanupPending');
    assert.equal(run.phase, SCENE_PHASES.STOPPING);
}

// Data never falls back to control; receiver reconnection replays the same
// immutable start and accepts its already-durable retained bytes.
{
    const f = fixture(), file = f.asset();
    f.target.data.readyState = 3; f.server.unregisterUploadSocket(f.target, f.target.data);
    f.send(f.owner, 'upload_start', { uploadId: 'retained', files: [file] });
    assert.equal(f.server.uploads.get('retained').pendingRelayStart, true);
    assert.equal(f.target.ws.messages.some(m => m.type === 'upload_start'), false);
    f.target.data = socket(); f.server.registerUploadSocket(f.target, f.target.data);
    f.server.restoreUploadsForDataChannel(f.target);
    assert.equal(f.latest(f.target, 'upload_start').uploadId, 'retained');
    f.send(f.target, 'upload_ready', { uploadId: 'retained', assets: [f.state(file, 65536)] });
    const upload = f.server.uploads.get('retained');
    assert.equal(upload.durableBytes, 65536); assert.equal(upload.relayedBytes, 65536);
    assert.equal(f.latest(f.owner, 'upload_ready').assets[0].offset, 65536);
    f.server.restoreUploadsForDataChannel(f.target);
    f.send(f.target, 'upload_progress', { uploadId: 'retained', delta: true,
        assets: [f.state(file, 98304)] });
    assert.equal(f.server.uploads.get('retained'), upload,
        'a delayed ACK ahead of the old relay cursor waits for full READY reconciliation');
    assert.equal(f.latest(f.target, 'upload_resume').assets[0].offset, 65536,
        'an accepted receiver reconciles queued writers through RESUME, never a fresh START');
    f.send(f.target, 'upload_ready', { uploadId: 'retained', assets: [f.state(file, 65536)] });
    f.send(f.owner, 'upload_chunk', { uploadId: 'retained', assetId: file.assetId,
        offset: 65536, size: 32768, sha256: file.sha256,
        data: Buffer.alloc(32768).toString('base64') }, false);
    assert.equal(f.latest(f.owner, 'upload_rejected', false).temporary, true);
    assert.equal(upload.relayedBytes, 65536);
    f.server.remoteSessions.markDisconnected('B');
    f.send(f.owner, 'upload_resume', { uploadId: 'retained' });
    assert.equal(f.latest(f.owner, 'upload_rejected', false).code, 'remote_session_reconnecting');
    assert.equal(f.server.uploads.get('retained'), upload);
}

// Delta ACKs retain unchanged assets; fairness and aggregate credit are enforced
// without terminalizing a sender holding an older advertised credit.
{
    const f = fixture(), files = [f.asset('one'), f.asset('two')];
    f.send(f.owner, 'upload_start', { uploadId: 'fair-a', files });
    f.send(f.target, 'upload_ready', { uploadId: 'fair-a', assets: files.map(file => f.state(file)) });
    const other = f.add('C'), session2 = f.open(other, f.target);
    f.send(other, 'upload_start', { uploadId: 'fair-c', files: [f.asset('three')] }, true, session2);
    assert.equal(f.server.uploadWindowBytes(f.server.uploads.get('fair-a')), 32768);
    const chunk = { uploadId: 'fair-a', assetId: 'one', offset: 0, size: 32768,
        sha256: files[0].sha256, data: Buffer.alloc(32768).toString('base64') };
    f.send(f.owner, 'upload_chunk', chunk);
    f.send(f.owner, 'upload_chunk', { ...chunk, offset: 32768 });
    assert.equal(f.latest(f.owner, 'upload_rejected', false).code, 'upload_window_wait');
    assert.equal(f.server.uploads.has('fair-a'), true);
    f.send(f.owner, 'upload_chunk', { ...chunk, offset: 65536 });
    assert.equal(f.latest(f.owner, 'upload_rejected', false).temporary, true);
    assert.equal(f.server.uploads.has('fair-a'), true, 'queued packets after a pause stay nonterminal');
    f.advance(100500);
    f.send(f.target, 'upload_progress', { uploadId: 'fair-a', delta: true,
        assets: [f.state(files[0], 32768)] });
    const upload = f.server.uploads.get('fair-a');
    assert.equal(upload.assetStates.get('one').durableOffset, 32768);
    assert.equal(upload.assetStates.get('two').durableOffset, 0);
    f.send(f.target, 'upload_progress', { uploadId: 'fair-a', delta: true, assets: [f.state(files[0], 0)] });
    assert.equal(upload.assetStates.get('one').durableOffset, 32768, 'a delayed ACK never rewinds durable progress');
    assert.equal(f.server.uploads.has('fair-a'), true);
    assert.equal(f.server.recipientUploadWindow('B').bytes, 32768);
    f.server.recordUploadConfirmation(upload, 1024 * 1024); f.advance(101000);
    f.server.recordUploadConfirmation(upload, 1024 * 1024);
    assert.equal(f.server.recipientUploadWindow('B').bytes, 262144);
    assert.equal(f.owner.ws.messages.some(m => f.server.uploadDataMessageTypes.has(m.type)), false);
}

// Recovery preserves preparation, clears stale clock approvals, and does not
// shift a published COMMIT or resend an expired playback command.
{
    const f = fixture();
    const run = f.server.sceneRuns.prepare({ remoteSessionId: f.session.remoteSessionId,
        generation: 1, sceneRunId: 'recover-prepare', revision: 1, digest: 'a'.repeat(64),
        manifest: [], scene: {}, ownerEndpointId: 'A', targetEndpointId: 'B' }).run;
    for (const id of ['A', 'B']) f.server.sceneRuns.markPrepared(run.sceneRunId, id, run.digest);
    f.server.sceneRuns.arm(run.sceneRunId, 'A', run.digest, 10);
    f.server.remoteSessions.markDisconnected('A'); f.server.pauseSceneForRecovery(run, f.session);
    assert.equal(run.phase, SCENE_PHASES.PREPARED); assert.equal(run.armedEndpoints.size, 0);
    f.session.phase = 'Active'; f.session.graceEndpoints.clear(); f.session.degradedEndpoints.clear();
    f.advance(102000); f.server.reconcileSceneAfterRecovery(f.session);
    assert.equal(f.latest(f.owner, 'prepared', false).allPrepared, true);
    f.server.sceneRuns.arm(run.sceneRunId, 'A', run.digest, 10);
    f.server.sceneRuns.arm(run.sceneRunId, 'B', run.digest, 10);
    const deadline = run.startServerMonotonicMs;
    f.server.pauseSceneForRecovery(run, f.session);
    f.advance(deadline + 800); f.server.reconcileSceneAfterRecovery(f.session);
    assert.equal(f.owner.ws.messages.some(m => m.type === 'commit'), false);
    assert.equal(run.startServerMonotonicMs, deadline);
    f.advance(deadline + 6000);
    assert.equal(f.server.sceneRuns.markStarted(run.sceneRunId, 'A', run.digest, true, deadline).ok, true);
    assert.equal(f.server.sceneRuns.markStarted(run.sceneRunId, 'B', run.digest, true, deadline + 100).live, true);
}
// Oversized scene commands fail before acquiring a target run or graph lock.
{
    const f = fixture();
    f.send(f.owner, 'scene_prepare', { sceneRunId: 'oversized', revision: 1,
        digest: 'a'.repeat(64), manifest: [], scene: { text: 'x'.repeat(245761) } });
    assert.equal(f.latest(f.owner, 'error', false).code, 'scene_payload_too_large');
    assert.equal(f.server.sceneRuns.getForSession(f.session.remoteSessionId), null);
}

// Residency deltas retain unmodified media and produce a five-second summary.
{
    const f = fixture(), logs = [], files = [f.asset('one'), f.asset('two')];
    f.server.protocolLogger = encoded => logs.push(JSON.parse(encoded));
    f.server.sessionAssets.set(f.session.remoteSessionId, new Map(files.map(file => [file.assetId, file])));
    const assets = files.map(file => ({ assetId: file.assetId, sha256: file.sha256,
        state: 'ready', progress: 1, error: '' }));
    f.send(f.target, 'media_residency', { sequence: 1, assets });
    f.advance(104000); f.send(f.target, 'heartbeat', { sequence: 1 });
    f.advance(105000);
    f.send(f.target, 'media_residency', { sequence: 2, delta: true, assets: [assets[1]] });
    assert.equal(f.session.mediaResidency.assets.length, 2);
    assert.equal(f.server.sceneMemoryReady(f.session, files), true);
    assert.equal(f.latest(f.owner, 'media_residency', false).delta, true);
    assert.equal(logs.filter(event => event.event === 'media_residency_summary').length, 1);
    assert.equal(logs.some(event => event.event === 'protocol_message_received' && event.type === 'media_residency'), false);
}
// Final upload evidence and media residency use independent sockets. Control
// may arrive first without losing the only ready notification for retained media.
{
    const f = fixture(), file = f.asset();
    f.send(f.owner, 'upload_start', { uploadId: 'ordered', files: [file] });
    f.send(f.target, 'upload_ready', { uploadId: 'ordered', assets: [f.state(file, file.size)] });
    f.send(f.owner, 'upload_complete', { uploadId: 'ordered', assets: [f.state(file, file.size)] });
    f.send(f.target, 'media_residency', { sequence: 1, assets: [{ assetId: file.assetId,
        sha256: file.sha256, state: 'ready', progress: 1, error: '' }] });
    assert.equal(f.server.sceneMemoryReady(f.session, [file]), true);
    assert.equal(f.server.validateSceneInventory(f.session, [file]).ok, false,
        'residency alone never validates a scene asset');
    f.send(f.target, 'upload_finished', { uploadId: 'ordered', assets: [f.state(file, file.size)] });
    assert.equal(f.server.validateSceneInventory(f.session, [file]).ok, true);
    assert.equal(f.server.sceneMemoryReady(f.session, [file]), true);
}
// A failed delta enqueue cannot be followed by a smaller credit-only message
// that claims unreported bytes. That would look like an integrity mismatch.
{
    const f = fixture(), file = f.asset(), second = f.asset('second');
    f.send(f.owner, 'upload_start', { uploadId: 'queue-bound', files: [file, second] });
    f.send(f.target, 'upload_ready', { uploadId: 'queue-bound', assets: [f.state(file), f.state(second)] });
    f.send(f.owner, 'upload_chunk', { uploadId: 'queue-bound', assetId: file.assetId,
        offset: 0, size: 32768, sha256: file.sha256, data: Buffer.alloc(32768).toString('base64') });
    const relay = f.server.sendToEndpoint.bind(f.server);
    f.server.sendToEndpoint = (endpoint, payload) => payload.type === 'upload_progress'
        && payload.assets.length ? false : relay(endpoint, payload);
    f.owner.data.messages = [];
    f.send(f.target, 'upload_progress', { uploadId: 'queue-bound', delta: true,
        assets: [f.state(file, 32768)] });
    assert.equal(f.owner.data.messages.some(message => message.type === 'upload_progress'), false);
    assert.equal(f.server.uploads.get('queue-bound').durableBytes, 32768);
    f.server.sendToEndpoint = relay;
    f.send(f.owner, 'upload_chunk', { uploadId: 'queue-bound', assetId: second.assetId,
        offset: 0, size: 32768, sha256: second.sha256, data: Buffer.alloc(32768).toString('base64') });
    f.send(f.target, 'upload_progress', { uploadId: 'queue-bound', delta: true,
        assets: [f.state(second, 32768)] });
    const repaired = f.owner.data.messages.find(message => message.type === 'upload_progress' && message.assets.length);
    assert.equal(repaired.delta, false);
    assert.equal(repaired.assets.length, 2, 'repair includes a different asset whose earlier delta could not be queued');
    assert.equal(repaired.durableBytes, 65536);
}
console.log('protocol v12 reliability tests passed');
