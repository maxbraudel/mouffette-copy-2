'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { MouffetteServer } = require('./server');
const { RemoteSessionRegistry } = require('./remote_session_registry');
const { SCENE_PHASES } = require('./scene_run_registry');

const cleanup = { result: 'committed', sceneStopped: true, uploadsAborted: true,
    cacheQuarantined: true, removedFileCount: 0 };
const snapshot = { machineName: 'fixture', platform: 'test', instanceOrdinal: 1,
    screens: [], systemUI: [], volumePercent: 50 };
const initialSnapshot = { screens: [], systemUI: [], volumePercent: 50,
    revision: 1, capturedAtEpochMs: 1000000 };
function fixture() {
    let now = 1000;
    const server = new MouffetteServer({ port: 0, monotonicNow: () => now,
        epochNow: () => 1000000 + now, metricLogger: () => {}, protocolLogger: () => {} });
    const add = (id, generation = 1) => {
        const ws = { readyState: 1, bufferedAmount: 0, messages: [],
            send(data) { this.messages.push(JSON.parse(data)); }, close() { this.readyState = 3; } };
        const client = { id, endpointId: id, installationId: `installation-${id}`,
            instanceId: 'primary', instanceOrdinal: 1, runtimeId: `runtime-${id}`,
            connectionGeneration: generation, authenticated: true, machineName: id,
            platform: 'test', screens: [], lastHeartbeatAt: 1000000 + now,
            lastHeartbeatMonotonicAt: now, ws };
        server.clients.set(id, client);
        server.currentTransportByEndpoint.set(id, client);
        server.connectionGenerationSequence = Math.max(server.connectionGenerationSequence, generation);
        return client;
    };
    const send = (id, type, fields = {}) => server.handleMessage(id, {
        type, protocolVersion: 8, serverBootId: server.serverBootId,
        messageId: crypto.randomUUID(), connectionGeneration: server.clients.get(id).connectionGeneration,
        ...fields,
    });
    const open = (owner = 'A', target = 'B') => server.remoteSessions.open({
        ownerEndpointId: owner, targetEndpointId: target,
        ownerRuntimeId: `runtime-${owner}`, targetRuntimeId: `runtime-${target}`,
        ownerConnectionGeneration: server.clients.get(owner).connectionGeneration,
        targetConnectionGeneration: server.clients.get(target).connectionGeneration,
    }).session;
    return { server, add, send, open, advance(value) { now = value; } };
}
const ofType = (client, type) => client.ws.messages.filter(message => message.type === type);

// Two missed heartbeat intervals start one 3 s recovery period for the Live session.
// A lost resume result is replayable; the peer can recover its older observation.
{
    const f = fixture(); const a = f.add('A'); const b = f.add('B'); const session = f.open();
    const run = f.server.sceneRuns.prepare({ remoteSessionId: session.remoteSessionId,
        generation: 1, sceneRunId: 'live-run', revision: 1, digest: 'digest',
        manifest: [], scene: {}, ownerEndpointId: 'A', targetEndpointId: 'B' }).run;
    run.phase = SCENE_PHASES.LIVE;
    f.advance(2000); f.send('B', 'heartbeat', { sequence: 1 });
    f.advance(2500); f.send('B', 'heartbeat', { sequence: 2 }); f.server.sweepRemoteSessionLeases();
    assert.equal(a.ws.readyState, 3);
    assert.equal(session.phase, 'Grace');
    const presence = f.server.presenceEntries().find(entry => entry.endpointId === 'A');
    assert.equal(presence.status, 'Degraded');
    assert.equal(presence.canAcceptSession, false);
    assert.equal(presence.reason, 'transport_lost');
    assert.equal(run.phase, SCENE_PHASES.LIVE);
    assert.equal(f.server.remoteSessions.validUntil(session), 5500);
    f.advance(3500); f.send('B', 'heartbeat', { sequence: 2 });
    f.advance(4500); f.send('B', 'heartbeat', { sequence: 3 });
    f.advance(5499); const rebound = f.add('A', 2);
    const resume = { requestId: 'resume-A', remoteSessionId: session.remoteSessionId,
        generation: 1, resumeToken: session.resumeToken };
    f.send('A', 'remote_session_resume', resume);
    assert.equal(session.generation, 2);
    assert.equal(session.phase, 'Active');
    assert.equal(session.ownerKnownGeneration, 1, 'send is not an applied ACK');
    f.send('A', 'remote_session_resume', resume);
    assert.equal(session.generation, 2);
    assert.equal(ofType(rebound, 'remote_session_resumed').at(-1).replay, true);
    f.send('A', 'remote_session_state_ack', { remoteSessionId: session.remoteSessionId,
        generation: 2, stateRevision: session.stateRevision });
    assert.equal(session.ownerKnownGeneration, 2);
    f.send('B', 'remote_session_state_ack', { remoteSessionId: session.remoteSessionId,
        generation: 2, stateRevision: session.stateRevision });
    f.server.handleRemoteSessionDeparture(b);
    f.add('B', 2);
    f.send('B', 'remote_session_resume', { ...resume, requestId: 'resume-B' });
    assert.equal(session.generation, 3, 'the dropped gen2 frame cannot strand B');
    assert.equal(session.phase, 'Active');
    for (const id of ['A', 'B']) f.send(id, 'remote_session_state_ack', {
        remoteSessionId: session.remoteSessionId, generation: 3, stateRevision: session.stateRevision });
    f.send('B', 'heartbeat', { sequence: 3 });
    const lease = ofType(f.server.clients.get('B'), 'heartbeat_ack').at(-1).sessionStates[0];
    assert.equal(lease.validUntilServerMonotonicMs, 9999);
    assert.equal(lease.serverMonotonicMs, 5499);
}

// The deadline is not restarted by departure, reconnect, or the healthy peer.
{
    const f = fixture(); f.add('A'); const b = f.add('B'); const session = f.open();
    f.advance(2000); f.send('B', 'heartbeat', { sequence: 1 });
    f.advance(2500); f.send('B', 'heartbeat', { sequence: 2 }); f.server.sweepRemoteSessionLeases();
    f.advance(3500); f.send('B', 'heartbeat', { sequence: 2 });
    f.advance(4500); f.send('B', 'heartbeat', { sequence: 3 });
    f.advance(5499); f.send('B', 'heartbeat', { sequence: 4 });
    assert.equal(session.phase, 'Grace');
    f.advance(5500); f.server.sweepRemoteSessionLeases();
    assert.equal(f.server.remoteSessions.sessions.has(session.remoteSessionId), false);
    assert.equal(f.server.remoteSessions.cleanupJobs.has(session.remoteSessionId), true);
    assert.equal(ofType(b, 'remote_session_closed').at(-1).cleanupState, 'pending');
    f.add('A', 2);
    f.send('A', 'remote_session_resume', { requestId: 'late', remoteSessionId: session.remoteSessionId,
        generation: 1, resumeToken: session.resumeToken });
    assert.equal(session.phase, 'CleanupPending');
}

// Metadata updates do not replay historical closes; disabling is correlated,
// idempotent, and blocks new outgoing work before the socket is closed.
{
    const f = fixture(); const a = f.add('A'); f.add('B');
    f.send('A', 'endpoint_snapshot', snapshot);
    for (let i = 0; i < 9; ++i) {
        const session = f.open();
        f.send('A', 'remote_session_close', { remoteSessionId: session.remoteSessionId, generation: 1 });
        f.send('B', 'remote_session_teardown_ack', { remoteSessionId: session.remoteSessionId,
            generation: 1, teardownId: session.teardownId, ...cleanup });
    }
    a.ws.messages = [];
    f.send('A', 'endpoint_snapshot', snapshot);
    assert.deepEqual(a.ws.messages.map(message => message.type), ['endpoint_snapshot_applied', 'client_list']);
    f.send('A', 'endpoint_disable', { requestId: 'disable-A' });
    f.send('A', 'endpoint_disable', { requestId: 'disable-A' });
    const disabled = ofType(a, 'endpoint_disable_started').at(-1);
    assert.equal(disabled.requestId, 'disable-A'); assert.equal(disabled.replay, true);
    f.send('A', 'remote_session_open', { targetEndpointId: 'B', requestId: 'disabled-open' });
    assert.equal(ofType(a, 'error').at(-1).code, 'endpoint_draining');
}

// Duplicate ACCEPT preserves the latest sequence and snapshot. Reconciliation
// includes full state and authoritative absence, while ACKs reflect application.
{
    const f = fixture(); const a = f.add('A'); f.add('B');
    f.send('A', 'remote_session_open', { targetEndpointId: 'B', requestId: 'open' });
    const session = f.server.remoteSessions.forOwnerTarget('A', 'B');
    const accept = { remoteSessionId: session.remoteSessionId, generation: 1, snapshot: initialSnapshot };
    f.send('B', 'remote_session_accept', accept);
    assert.equal(f.server.remoteSessions.commandReady(session), false);
    const initialRevision = session.stateRevision;
    f.send('A', 'remote_session_state_ack', { remoteSessionId: session.remoteSessionId,
        generation: 1, stateRevision: initialRevision });
    assert.equal(f.server.remoteSessions.commandReady(session), false, 'one ACK is insufficient');
    assert.equal(f.server.validateSessionMessage('A', { remoteSessionId: session.remoteSessionId,
        generation: 1, connectionGeneration: 1 }).error, 'remote_session_sync_pending');
    f.send('B', 'remote_session_state_ack', { remoteSessionId: session.remoteSessionId,
        generation: 1, stateRevision: initialRevision });
    assert.equal(f.server.remoteSessions.commandReady(session), true);
    assert.equal(session.stateRevision, initialRevision, 'ready notification cannot create an ACK loop');
    assert.equal(ofType(a, 'remote_session_lease_state').at(-1).commandReady, true);
    session.snapshotSequence = 5;
    session.latestTargetSnapshot = { generation: 1, snapshot: { ...initialSnapshot, revision: 5 } };
    f.send('B', 'remote_session_accept', accept);
    assert.equal(session.snapshotSequence, 5);
    assert.equal(ofType(a, 'remote_session_opened').at(-1).snapshot.revision, 5);
    f.send('A', 'remote_session_open', { targetEndpointId: 'B', requestId: 'open' });
    assert.equal(ofType(a, 'remote_session_opened').at(-1).snapshotSequence, 5);
    assert.equal(ofType(a, 'remote_session_opened').at(-1).snapshot.revision, 5);
    f.send('A', 'remote_session_reconcile', { requestId: 'reconcile', sessions: [
        { remoteSessionId: session.remoteSessionId, generation: 1, stateRevision: 1 },
        { remoteSessionId: 'absent', generation: 1, stateRevision: 1 },
    ] });
    const result = ofType(a, 'remote_session_reconciled').at(-1);
    assert.equal(result.complete, true); assert.deepEqual(result.absentSessionIds, ['absent']);
    assert.equal(result.sessions[0].snapshot.revision, 5);
    assert.equal(result.sessions[0].stateRevision, session.stateRevision);
}

// Cleanup admission is per pair: another incoming controller remains available;
// unresolved proof consumes bounded capacity and cannot be silently evicted.
{
    const f = fixture(); f.add('A'); const b = f.add('B'); f.add('C'); const session = f.open();
    f.send('A', 'remote_session_close', { remoteSessionId: session.remoteSessionId, generation: 1 });
    f.send('A', 'remote_session_open', { targetEndpointId: 'B', requestId: 'same-pair' });
    assert.equal(ofType(f.server.clients.get('A'), 'error').at(-1).code, 'session_cleanup_pending');
    f.send('C', 'remote_session_open', { targetEndpointId: 'B', requestId: 'independent-pair' });
    assert.equal(f.server.remoteSessions.forOwnerTarget('C', 'B').phase, 'Opening');
    assert.equal(f.server.presenceEntries().find(entry => entry.endpointId === 'B').canAcceptSession, true);
    f.send('B', 'remote_session_teardown_ack', { remoteSessionId: session.remoteSessionId,
        generation: 1, teardownId: session.teardownId, ...cleanup });
    assert.equal(ofType(b, 'remote_session_closed').at(-1).cleanupState, 'confirmed');
    f.send('A', 'remote_session_open', { targetEndpointId: 'B', requestId: 'after-quarantine' });
    assert.equal(f.server.remoteSessions.forOwnerTarget('A', 'B').phase, 'Opening');
    const registry = new RemoteSessionRegistry({ maximumSessions: 1 });
    const binding = { ownerEndpointId: 'A', targetEndpointId: 'B', ownerRuntimeId: 'a', targetRuntimeId: 'b' };
    const first = registry.open(binding).session;
    registry.terminate(first.remoteSessionId, 'test');
    registry.markCleanupPending(first.remoteSessionId, first.teardownId);
    assert.equal(registry.open({ ...binding, ownerEndpointId: 'C' }).error, 'session_capacity_exceeded');
    assert.equal(registry.cleanupJobs.size, 1);
}

// STOP acknowledgements remain valid after logical session closure.
{
    const f = fixture(); f.add('A'); f.add('B'); const session = f.open();
    const run = f.server.sceneRuns.prepare({ remoteSessionId: session.remoteSessionId,
        generation: 1, sceneRunId: 'stop-run', revision: 1, digest: 'digest', manifest: [],
        scene: {}, ownerEndpointId: 'A', targetEndpointId: 'B' }).run;
    run.phase = SCENE_PHASES.LIVE;
    f.send('A', 'remote_session_close', { remoteSessionId: session.remoteSessionId, generation: 1 });
    for (const id of ['A', 'B']) f.send(id, 'stopped', { remoteSessionId: session.remoteSessionId,
        generation: 1, sceneRunId: run.sceneRunId, digest: run.digest, success: true });
    assert.equal(f.server.sceneRuns.get(run.sceneRunId), null);
    assert.equal(run.stoppedEndpoints.size, 2);
    f.send('B', 'remote_session_teardown_ack', { remoteSessionId: session.remoteSessionId,
        generation: 1, teardownId: session.teardownId, ...cleanup });
    f.send('A', 'stopped', { remoteSessionId: session.remoteSessionId,
        generation: 1, sceneRunId: run.sceneRunId, digest: run.digest, success: true });
    assert.equal(ofType(f.server.clients.get('A'), 'stopped').at(-1).replay, true);
}

// One fixed recovery period starts at the first interruption. Returning only
// one party or losing the final applied-state ACK cannot renew that period.
{
    let now = 1000;
    let epoch = 1700000000000;
    const registry = new RemoteSessionRegistry({ leaseTimeoutMs: 4500, recoveryTimeoutMs: 3000,
        monotonicNow: () => now, epochNow: () => epoch });
    const session = registry.open({ ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'a', targetRuntimeId: 'b' }).session;
    now = 1100;
    registry.markDisconnected('A');
    assert.equal(registry.validUntil(session), 4100);
    now = 2100;
    registry.markDisconnected('B');
    registry.markDisconnected('A'); // duplicate loss, no new budget
    assert.equal(registry.validUntil(session), 4100);
    epoch -= 86400000;
    now = 3100;
    assert.ok(registry.resume({ remoteSessionId: session.remoteSessionId, endpointId: 'B',
        runtimeId: 'b', resumeToken: session.resumeToken, generation: 1,
        connectionGeneration: 7, requestId: 'B-resume' }).ok);
    assert.equal(registry.validUntil(session), 4100);
    now = 4099;
    assert.ok(registry.resume({ remoteSessionId: session.remoteSessionId, endpointId: 'A',
        runtimeId: 'a', resumeToken: session.resumeToken, generation: 1,
        connectionGeneration: 9, requestId: 'A-resume' }).ok);
    assert.ok(registry.acknowledgeState(session.remoteSessionId, 'A', 9,
        session.generation, session.stateRevision));
    assert.equal(registry.commandReady(session), false);
    assert.equal(registry.validUntil(session), 4100);
    now = 4100;
    assert.equal(registry.acknowledgeState(session.remoteSessionId, 'B', 7,
        session.generation, session.stateRevision), false, 'an ACK at the boundary is too late');
    assert.equal(registry.tick().length, 1);
    assert.equal(registry.tick().length, 0, 'expiry is emitted only once');
    assert.equal(session.phase, 'Terminating');
}

// A fully acknowledged recovery completes the interruption. Only a subsequent
// independent loss gets a new period. Sleep cannot postpone silent detection.
{
    let now = 1000;
    const registry = new RemoteSessionRegistry({ leaseTimeoutMs: 4500, recoveryTimeoutMs: 3000,
        now: () => now });
    const session = registry.open({ ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'a', targetRuntimeId: 'b' }).session;
    now = 1100;
    registry.markDisconnected('A');
    now = 2000;
    registry.touch(session.remoteSessionId, 'B', 1);
    assert.ok(registry.resume({ remoteSessionId: session.remoteSessionId, endpointId: 'A',
        runtimeId: 'a', resumeToken: session.resumeToken, generation: 1,
        connectionGeneration: 2, requestId: 'resume' }).ok);
    for (const [id, generation] of [['A', 2], ['B', 1]])
        registry.acknowledgeState(session.remoteSessionId, id, generation,
            session.generation, session.stateRevision);
    assert.ok(registry.commandReady(session));
    assert.equal(session.graceDeadlineAt, null);
    now = 2100;
    registry.markDisconnected('A');
    assert.equal(registry.validUntil(session), 5100);
    now = 10000; // loop suspended: detection belongs to the missed heartbeat, not wake-up
    registry.markDegraded(now, 1500);
    assert.equal(registry.validUntil(session), 5100);
    assert.equal(registry.tick().length, 1);
}

console.log('connection recovery v7 tests passed');
