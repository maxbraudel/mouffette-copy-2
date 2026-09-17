'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const {
    challengePayload, createChallenge, installationIdForPublicKey,
    endpointIdForInstallation, verifyAuthResponse,
} = require('./device_auth');
const { RemoteSessionRegistry } = require('./remote_session_registry');
const { MouffetteServer } = require('./server');

{
    const serverBootId = crypto.randomUUID();
    const runtimeId = crypto.randomUUID();
    const challenge = createChallenge(serverBootId, 1000);
    const keys = crypto.generateKeyPairSync('ed25519');
    const publicKeyDer = keys.publicKey.export({ type: 'spki', format: 'der' });
    const installationId = installationIdForPublicKey(publicKeyDer);
    const instanceId = 'primary';
    const instanceOrdinal = 1;
    const signature = crypto.sign(null,
        challengePayload({ ...challenge, runtimeId, instanceId, instanceOrdinal }), keys.privateKey);
    const response = {
        protocolVersion: 7,
        serverBootId,
        runtimeId,
        instanceId,
        instanceOrdinal,
        publicKey: publicKeyDer.toString('base64url'),
        installationId,
        signature: signature.toString('base64url'),
    };
    const verified = verifyAuthResponse(challenge, response, 1500, 10_000);
    assert.equal(verified.ok, true);
    assert.equal(verified.installationId, installationId);
    assert.equal(verified.endpointId,
        endpointIdForInstallation(installationId, instanceId));
    assert.equal(verifyAuthResponse(challenge, { ...response, runtimeId: crypto.randomUUID() }, 1500, 10_000).ok, false);
    assert.equal(verifyAuthResponse(challenge, {
        ...response,
        installationId: installationIdForPublicKey(Buffer.from('another-key')),
    }, 1500, 10_000).error, 'installation_id_mismatch');
    const forgedSignature = Buffer.from(signature);
    forgedSignature[0] ^= 0xff;
    assert.equal(verifyAuthResponse(challenge, {
        ...response,
        signature: forgedSignature.toString('base64url'),
    }, 1500, 10_000).error, 'invalid_identity_signature');
    assert.equal(verifyAuthResponse(challenge, {
        ...response,
        publicKey: `${response.publicKey}=`,
    }, 1500, 10_000).error, 'invalid_identity_material');
    assert.equal(verifyAuthResponse(challenge, response, 11_001, 10_000).ok, false);

    const differentChallenge = createChallenge(serverBootId, 1000);
    assert.equal(verifyAuthResponse(differentChallenge, response, 1500, 10_000).error,
        'invalid_identity_signature',
    'a response signed for one nonce cannot authenticate a different socket');
}

const committedCleanup = (extra = {}) => ({
    result: 'committed',
    sceneStopped: true,
    uploadsAborted: true,
    cacheQuarantined: true,
    removedFileCount: 0,
    ...extra,
});

const binding = (owner = 'A', target = 'B') => ({
    ownerEndpointId: owner,
    targetEndpointId: target,
    ownerRuntimeId: `runtime-${owner}`,
    targetRuntimeId: `runtime-${target}`,
    ownerConnectionGeneration: 1,
    targetConnectionGeneration: 1,
});

function testSocket() {
    return {
        readyState: WebSocket.OPEN,
        bufferedAmount: 0,
        messages: [],
        send(encoded) { this.messages.push(JSON.parse(encoded)); },
        close() { this.readyState = WebSocket.CLOSED; },
    };
}

function addAuthenticatedClient(server, connectionId, endpointId,
                                connectionGeneration = 1) {
    const ws = testSocket();
    server.clients.set(connectionId, {
        id: connectionId,
        installationId: `installation-${endpointId}`,
        endpointId,
        instanceId: 'primary',
        instanceOrdinal: 1,
        runtimeId: `runtime-${endpointId}`,
        connectionGeneration,
        authenticated: true,
        machineName: endpointId,
        platform: 'test',
        screens: [],
        ws,
    });
    server.currentTransportByEndpoint.set(endpointId, server.clients.get(connectionId));
    server.connectionGenerationSequence = Math.max(server.connectionGenerationSequence,
        server.clients.get(connectionId).connectionGeneration);
    return ws;
}

function addAuthenticationCandidate(server, connectionId, keyPair, runtimeId,
                                    issuedAt, instanceId = 'primary') {
    const ws = testSocket();
    const publicKeyDer = keyPair.publicKey.export({ type: 'spki', format: 'der' });
    const challenge = createChallenge(server.serverBootId, issuedAt);
    const instanceOrdinal = instanceId === 'primary' ? 1 : Number(instanceId.slice(9));
    const installationId = installationIdForPublicKey(publicKeyDer);
    const client = {
        id: connectionId,
        installationId: null,
        endpointId: null,
        instanceId: null,
        runtimeId: null,
        connectionGeneration: 1,
        authenticated: false,
        machineName: null,
        screens: [],
        ws,
        authChallenge: challenge,
        authTimer: null,
    };
    server.clients.set(connectionId, client);
    const response = {
        protocolVersion: 7,
        serverBootId: server.serverBootId,
        runtimeId,
        instanceId,
        instanceOrdinal,
        installationId,
        publicKey: publicKeyDer.toString('base64url'),
        signature: crypto.sign(null,
            challengePayload({ ...challenge, runtimeId, instanceId, instanceOrdinal }), keyPair.privateKey)
            .toString('base64url'),
    };
    return {
        client, ws, response,
        endpointId: endpointIdForInstallation(installationId, instanceId),
    };
}

// Production construction keeps terminal proof capacity at least as large as
// retained OPEN replay capacity. Both intentionally share one static bound.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    assert.equal(server.remoteSessions.maximumOpenRequests,
        server.remoteSessions.maximumTombstones);
    assert.ok(server.remoteSessions.maximumTombstones
        >= server.remoteSessions.maximumOpenRequests);
    assert.throws(() => new RemoteSessionRegistry({
        maximumTombstones: 1,
        maximumOpenRequests: 2,
    }), /maximumTombstones must be at least maximumOpenRequests/);
}

// OPEN and CLOSE use different insertion timelines. Capacity pressure must not
// evict a tombstone still referenced by a retained idempotency record merely
// because an older, unreferenced session happened to close later.
{
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        maximumTombstones: 1,
        maximumOpenRequests: 1,
        openRequestTtlMs: 1000,
        tombstoneTtlMs: 1000,
        now: () => 100,
        idFactory: () => `retention-${++sequence}`,
    });
    const evictedRequestBinding = {
        ...binding('old-owner', 'old-target'),
        requestId: 'old-open-request',
    };
    const retainedRequestBinding = {
        ...binding('new-owner', 'new-target'),
        requestId: 'retained-open-request',
    };
    const oldSession = registry.open(evictedRequestBinding).session;
    const retainedSession = registry.open(retainedRequestBinding).session;
    const close = (session) => {
        const terminating = registry.terminate(
            session.remoteSessionId, 'test_cleanup', 100).session;
        registry.markCleanupPending(
            session.remoteSessionId, terminating.teardownId, 100);
        const closed = registry.acknowledgeCleanup(
            session.remoteSessionId, terminating.teardownId,
            session.targetEndpointId, committedCleanup(), 100);
        assert.equal(closed.ok, true);
    };

    close(retainedSession);
    close(oldSession);
    assert.equal(registry.getTombstone(oldSession.remoteSessionId), null);
    assert.equal(registry.getTombstone(retainedSession.remoteSessionId), retainedSession);

    const replay = registry.open(retainedRequestBinding);
    assert.equal(replay.replay, true);
    assert.equal(replay.requestReplay, true);
    assert.equal(replay.session.remoteSessionId, retainedSession.remoteSessionId);
    assert.equal(registry.sessions.size, 0,
        'a retained terminal OPEN request must never create a replacement session');
}

// One installation may expose several independently addressable endpoints.
// Only an exact endpoint duplicate is subject to the active lease exclusion.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const keys = crypto.generateKeyPairSync('ed25519');
    const now = Date.now();
    const primary = addAuthenticationCandidate(
        server, 'same-install-primary', keys, crypto.randomUUID(), now);
    const secondary = addAuthenticationCandidate(
        server, 'same-install-secondary', keys, crypto.randomUUID(), now,
        'instance-2');

    server.handleAuthResponse('same-install-primary', primary.response, now + 1);
    server.handleAuthResponse('same-install-secondary', secondary.response, now + 2);

    assert.equal(primary.client.authenticated, true);
    assert.equal(secondary.client.authenticated, true);
    assert.equal(primary.client.installationId, secondary.client.installationId);
    assert.notEqual(primary.client.endpointId, secondary.client.endpointId);
    assert.equal(primary.client.instanceId, 'primary');
    assert.notEqual(secondary.client.instanceId, 'primary');
}

function serverSessionContext(prefix) {
    let sequence = 0;
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    server.remoteSessions = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        idFactory: () => `${prefix}-${++sequence}`,
    });
    const ownerSocket = addAuthenticatedClient(server, 'owner-connection', 'A');
    const targetSocket = addAuthenticatedClient(server, 'target-connection', 'B');
    const session = server.remoteSessions.open(binding()).session;
    let beginAttempts = 0;
    let beginCommits = 0;
    const originalBegin = server.beginRemoteSessionTeardown.bind(server);
    server.beginRemoteSessionTeardown = (...arguments_) => {
        ++beginAttempts;
        const began = originalBegin(...arguments_);
        if (began) ++beginCommits;
        return began;
    };
    return {
        server, session, ownerSocket, targetSocket,
        beginAttempts: () => beginAttempts,
        beginCommits: () => beginCommits,
    };
}

function messages(socket, type) {
    return socket.messages.filter(message => message.type === type);
}

// The server consumes an authentication challenge before verification. A
// duplicated response on the same socket and a response replayed onto a new
// socket are both terminal failures, even though the signature was once valid.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const keys = crypto.generateKeyPairSync('ed25519');
    const runtimeId = crypto.randomUUID();
    const now = Date.now();
    const first = addAuthenticationCandidate(
        server, 'auth-first', keys, runtimeId, now);
    server.handleAuthResponse('auth-first', first.response, now + 1);
    assert.equal(first.client.authenticated, true);
    assert.equal(first.client.authChallenge, null);

    server.handleAuthResponse('auth-first', first.response, now + 2);
    assert.equal(messages(first.ws, 'error').at(-1).code,
        'auth_challenge_already_consumed');
    assert.equal(first.ws.readyState, WebSocket.CLOSED);

    const replay = addAuthenticationCandidate(
        server, 'auth-replay', keys, crypto.randomUUID(), now + 2);
    server.handleAuthResponse('auth-replay', first.response, now + 3);
    assert.equal(replay.client.authenticated, false);
    assert.equal(replay.client.authChallenge, null);
    assert.equal(messages(replay.ws, 'error').at(-1).code,
        'invalid_identity_signature');
    assert.equal(replay.ws.readyState, WebSocket.CLOSED);
}

// A new process using the same installation key cannot displace a healthy
// runtime at 2,999 ms. At the exact 3,000 ms boundary the old runtime and all
// its sessions are terminal, the ghost transport is evicted, and the new
// runtime is admitted without treating the transition as a resume.
{
    const keyPair = crypto.generateKeyPairSync('ed25519');
    const oldRuntimeId = crypto.randomUUID();
    const newRuntimeId = crypto.randomUUID();
    const base = Date.now();

    const earlyServer = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const earlyCandidate = addAuthenticationCandidate(
        earlyServer, 'candidate-early', keyPair, newRuntimeId, base);
    const earlyOldSocket = addAuthenticatedClient(
        earlyServer, 'old-early', earlyCandidate.endpointId);
    const earlyOld = earlyServer.clients.get('old-early');
    earlyOld.runtimeId = oldRuntimeId;
    earlyOld.lastHeartbeatAt = base;
    earlyServer.handleAuthResponse(
        'candidate-early', earlyCandidate.response, base + 2_999);
    assert.equal(earlyCandidate.client.authenticated, false);
    assert.equal(messages(earlyCandidate.ws, 'error').at(-1).code,
        'endpoint_already_connected');
    assert.equal(earlyServer.clients.get('old-early'), earlyOld);
    assert.equal(earlyOldSocket.readyState, WebSocket.OPEN);

    let registryNow = base;
    let sequence = 0;
    const boundaryServer = new MouffetteServer({ port: 0, metricLogger: () => {} });
    boundaryServer.remoteSessions = new RemoteSessionRegistry({
        leaseTimeoutMs: 3_000,
        now: () => registryNow,
        idFactory: () => `runtime-boundary-${++sequence}`,
    });
    const boundaryCandidate = addAuthenticationCandidate(
        boundaryServer, 'candidate-boundary', keyPair, newRuntimeId, base);
    addAuthenticatedClient(boundaryServer, 'owner-boundary', 'owner-device');
    const oldSocket = addAuthenticatedClient(
        boundaryServer, 'old-boundary', boundaryCandidate.endpointId);
    const oldClient = boundaryServer.clients.get('old-boundary');
    oldClient.runtimeId = oldRuntimeId;
    oldClient.lastHeartbeatAt = base;
    boundaryServer.connectionGenerationSequence = 1;
    const session = boundaryServer.remoteSessions.open({
        ownerEndpointId: 'owner-device',
        targetEndpointId: boundaryCandidate.endpointId,
        ownerRuntimeId: 'runtime-owner',
        targetRuntimeId: oldRuntimeId,
        ownerConnectionGeneration: 1,
        targetConnectionGeneration: 1,
    }).session;

    registryNow = base + 3_000;
    boundaryServer.handleAuthResponse(
        'candidate-boundary', boundaryCandidate.response, registryNow);
    assert.equal(boundaryCandidate.client.authenticated, true);
    assert.equal(boundaryCandidate.client.connectionGeneration, 2);
    assert.equal(boundaryServer.clients.has('old-boundary'), false);
    assert.equal(oldSocket.readyState, WebSocket.CLOSED);
    assert.equal(session.phase, 'CleanupPending');
    assert.equal(session.generation, 1,
        'a process restart after lease expiry is terminal, never a resume');

    boundaryServer.handleEndpointSnapshot('candidate-boundary', {
        connectionGeneration: 2,
        machineName: 'Restarted target', platform: 'test', instanceOrdinal: 1,
        screens: [], systemUI: [],
        volumePercent: 50,
    });
    assert.equal(session.targetConnectionGeneration, 1,
        'terminal catch-up must not mutate the command-capable session tuple');
    assert.equal(session.targetRuntimeId, oldRuntimeId);
    assert.equal(session.targetTerminalConnectionGeneration, 2);
    assert.equal(session.targetTerminalRuntimeId, newRuntimeId);
    const catchUp = messages(
        boundaryCandidate.ws, 'remote_session_terminating').at(-1);
    assert.equal(catchUp.remoteSessionId, session.remoteSessionId);
    assert.equal(catchUp.generation, 1);
    assert.equal(catchUp.targetConnectionGeneration, 2);
    assert.equal(catchUp.connectionGeneration, 2);

    boundaryServer.handleRemoteSessionTeardownAck('candidate-boundary', {
        remoteSessionId: session.remoteSessionId,
        generation: session.generation,
        connectionGeneration: 2,
        teardownId: session.teardownId,
        ...committedCleanup(),
    });
    assert.equal(boundaryServer.remoteSessions.get(session.remoteSessionId), null);

    boundaryCandidate.client.connectionGeneration = 3;
    const closedBeforeReplay = messages(
        boundaryCandidate.ws, 'remote_session_closed').length;
    boundaryServer.handleEndpointSnapshot('candidate-boundary', {
        connectionGeneration: 3,
        machineName: 'Restarted target again', platform: 'test', instanceOrdinal: 1,
        screens: [], systemUI: [],
        volumePercent: 50,
    });
    assert.equal(messages(boundaryCandidate.ws, 'remote_session_closed').length,
        closedBeforeReplay + 1,
        'the cleanup process can recover a lost CLOSED tombstone');
    const replayedClosed = messages(
        boundaryCandidate.ws, 'remote_session_closed').at(-1);
    assert.equal(replayedClosed.replay, true);
    assert.equal(replayedClosed.targetConnectionGeneration, 3);
    assert.equal(replayedClosed.connectionGeneration, 3);
}

// Heartbeat leases also evict authenticated discovery ghosts that do not own
// any RemoteSession; otherwise a half-open TCP socket can remain listed forever.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const ghostSocket = addAuthenticatedClient(server, 'ghost-connection', 'ghost-device');
    const ghost = server.clients.get('ghost-connection');
    ghost.lastHeartbeatAt = 50_000;
    assert.equal(server.sweepExpiredClientTransports(52_999), 0);
    assert.equal(server.clients.get('ghost-connection'), ghost);
    assert.equal(server.sweepExpiredClientTransports(53_000), 1);
    assert.equal(server.clients.has('ghost-connection'), false);
    assert.equal(ghostSocket.readyState, WebSocket.CLOSED);
}

// Wall-clock corrections affect only the display timestamp. The terminal
// decision is based on the injected monotonic clock and remains fixed.
{
    let monotonic = 10_000;
    let epoch = 1_000_000;
    const server = new MouffetteServer({
        port: 0,
        metricLogger: () => {},
        monotonicNow: () => monotonic,
        epochNow: () => epoch,
    });
    addAuthenticatedClient(server, 'clock-owner', 'A');
    addAuthenticatedClient(server, 'clock-target', 'B');
    const session = server.remoteSessions.open(binding()).session;
    server.handleRemoteSessionDeparture(server.clients.get('clock-owner'));
    assert.equal(session.graceDeadlineAt, 15_000);
    assert.equal(session.graceDeadlineEpochMs, 1_005_000);

    epoch += 24 * 60 * 60 * 1_000;
    monotonic = 14_999;
    server.sweepRemoteSessionLeases();
    assert.equal(session.phase, 'Grace',
        'a forward wall-clock jump must not expire the lease early');
    assert.equal(session.graceDeadlineEpochMs, 1_005_000,
        'the originally advertised wall deadline is not pushed');

    epoch -= 48 * 60 * 60 * 1_000;
    monotonic = 15_000;
    server.sweepRemoteSessionLeases();
    assert.equal(session.phase, 'CleanupPending',
        'the exact monotonic boundary stays terminal after a wall-clock rollback');
}

// OPEN permits healthy targets before the 1,500 ms suspicion threshold;
// at 3,000 ms a silent transport is fenced before any new session exists.
{
    const createOpenServer = () => {
        let monotonic = 10_000;
        const server = new MouffetteServer({
            port: 0,
            metricLogger: () => {},
            monotonicNow: () => monotonic,
            epochNow: () => 1_000_000,
        });
        const ownerSocket = addAuthenticatedClient(
            server, 'open-owner', 'open-A');
        addAuthenticatedClient(server, 'open-target', 'open-B');
        const owner = server.clients.get('open-owner');
        const target = server.clients.get('open-target');
        owner.lastHeartbeatMonotonicAt = monotonic;
        target.lastHeartbeatMonotonicAt = monotonic;
        return {
            server, ownerSocket, target,
            setMonotonic(value) { monotonic = value; },
        };
    };

    const early = createOpenServer();
    early.setMonotonic(11_499);
    early.server.handleRemoteSessionOpen('open-owner', {
        targetEndpointId: 'open-B', connectionGeneration: 1,
        requestId: 'open-before-boundary',
    });
    assert.equal(early.server.remoteSessions.sessions.size, 1);
    assert.equal(messages(early.ownerSocket, 'remote_session_opening').length, 1);
    assert.equal(messages(early.target.ws, 'remote_session_offer').length, 1);

    const boundary = createOpenServer();
    boundary.setMonotonic(13_000);
    boundary.server.handleRemoteSessionOpen('open-owner', {
        targetEndpointId: 'open-B', connectionGeneration: 1,
        requestId: 'open-at-boundary',
    });
    assert.equal(boundary.server.remoteSessions.sessions.size, 0);
    assert.equal(boundary.server.clients.has('open-target'), false);
    assert.equal(boundary.target.ws.readyState, WebSocket.CLOSED);
    assert.equal(messages(boundary.ownerSocket, 'error').at(-1).code,
        'target_offline');
}

// A healthy target accepts independent sessions from several controllers.
{
    let monotonic = 20_000;
    const server = new MouffetteServer({
        port: 0,
        metricLogger: () => {},
        monotonicNow: () => monotonic,
        epochNow: () => 2_000_000,
    });
    addAuthenticatedClient(server, 'old-owner', 'old-A');
    const targetSocket = addAuthenticatedClient(server, 'busy-target', 'busy-B');
    const nextOwnerSocket = addAuthenticatedClient(server, 'next-owner', 'next-C');
    for (const client of server.clients.values()) {
        client.lastHeartbeatMonotonicAt = monotonic;
    }
    const oldSession = server.remoteSessions.open({
        ownerEndpointId: 'old-A', targetEndpointId: 'busy-B',
        ownerRuntimeId: 'runtime-old-A', targetRuntimeId: 'runtime-busy-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;

    monotonic = 23_000;
    server.clients.get('busy-target').lastHeartbeatMonotonicAt = monotonic;
    server.clients.get('next-owner').lastHeartbeatMonotonicAt = monotonic;
    oldSession.lastContact.set('busy-B', monotonic);
    server.handleRemoteSessionOpen('next-owner', {
        targetEndpointId: 'busy-B', connectionGeneration: 1,
        requestId: 'open-after-old-controller-expired',
    });
    assert.equal(oldSession.phase, 'Active');
    assert.equal(server.remoteSessions.incomingForTarget('busy-B').length, 2);
    assert.equal(server.remoteSessions.outgoingByOwner.has('next-C'), true);
    assert.equal(messages(nextOwnerSocket, 'remote_session_opening').length, 1);
    assert.equal(messages(targetSocket, 'remote_session_offer').length, 1);
}

// RemoteSession errors include only safe correlation fields. This lets the
// client settle a pending Connecting row without treating it as transport loss.
{
    const context = serverSessionContext('correlated-error');
    const controllerSocket = addAuthenticatedClient(
        context.server, 'controller-connection', 'C');

    context.server.handleRemoteSessionOpen('owner-connection', {
        targetEndpointId: 'offline-device',
        connectionGeneration: 1,
        requestId: 'open-offline',
    });
    const offline = messages(context.ownerSocket, 'error').at(-1);
    assert.equal(offline.scope, 'remote_session');
    assert.equal(offline.code, 'target_offline');
    assert.equal(offline.requestId, 'open-offline');
    assert.equal(offline.targetEndpointId, 'offline-device');
    assert.equal(context.ownerSocket.readyState, WebSocket.OPEN,
        'an offline target is a scoped request failure, not transport loss');

    context.server.handleRemoteSessionClose('controller-connection', {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        requestId: 'foreign-close',
    });
    const forbidden = messages(controllerSocket, 'error').at(-1);
    assert.equal(forbidden.scope, 'remote_session');
    assert.equal(forbidden.code, 'not_a_session_party');
    assert.equal(forbidden.requestId, 'foreign-close');
    assert.equal(forbidden.remoteSessionId, context.session.remoteSessionId);
}

// Correlation identifiers are bounded protocol data, not arbitrary reflected
// strings. Invalid values never cross from one authenticated peer to another.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const ownerSocket = addAuthenticatedClient(server, 'bounded-owner', 'bounded-A');
    const targetSocket = addAuthenticatedClient(server, 'bounded-target', 'bounded-B');
    server.handleRemoteSessionOpen('bounded-owner', {
        targetEndpointId: 'bounded-B', connectionGeneration: 1,
        requestId: `secret\n${'x'.repeat(1024)}`,
    });
    const rejected = messages(ownerSocket, 'error').at(-1);
    assert.equal(rejected.code, 'invalid_request_id');
    assert.equal(Object.hasOwn(rejected, 'requestId'), false);
    assert.equal(messages(targetSocket, 'remote_session_offer').length, 0);
    assert.equal(server.remoteSessions.sessions.size, 0);
}

// Incoming sessions are independent. Cleaning one session does not disturb
// another controller bound to the same target.
{
    let clock = 10_000;
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `exclusive-${++sequence}`,
    });
    const first = registry.open(binding('A', 'B'));
    assert.equal(first.ok, true);

    const second = registry.open(binding('C', 'B'));
    assert.equal(second.ok, true);
    assert.equal(registry.incomingForTarget('B').length, 2);
    assert.equal(registry.outgoingByOwner.has('C'), true);

    assert.equal(registry.open(binding('B', 'C')).ok, true,
        'a target may simultaneously own outgoing sessions');

    const terminating = registry.terminate(first.session.remoteSessionId, 'disconnect', clock);
    assert.equal(terminating.replay, false);
    assert.equal(registry.markCleanupPending(first.session.remoteSessionId,
        terminating.session.teardownId, clock).ok, true);
    const cleanupError = registry.acknowledgeCleanup(
        first.session.remoteSessionId, terminating.session.teardownId, 'B', {
            result: 'cleanup_error',
            sceneStopped: true,
            uploadsAborted: true,
            cacheQuarantined: false,
            errorCode: 'quarantine_failed',
        }, clock);
    assert.equal(cleanupError.ok, false);
    assert.equal(cleanupError.error, 'cleanup_not_committed');
    assert.equal(cleanupError.session.phase, 'CleanupPending');
    assert.equal(cleanupError.session.cleanupError, 'quarantine_failed');
    assert.equal(registry.incomingForTarget('B').length, 2);
    assert.equal(registry.forOwnerTarget('C', 'B'), second.session);

    const closed = registry.acknowledgeCleanup(
        first.session.remoteSessionId, terminating.session.teardownId, 'B',
        committedCleanup({ removedFileCount: 4 }), clock);
    assert.equal(closed.ok, true);
    assert.deepEqual(registry.incomingForTarget('B'), [second.session]);
    assert.equal(registry.outgoingByOwner.has('C'), true);
}

// Discovery exposes presence only; private session topology is never present.
{
    const context = serverSessionContext('list-cleanup-state');
    const observerSocket = addAuthenticatedClient(
        context.server, 'observer-connection', 'C');
    const terminating = context.server.remoteSessions.terminate(
        context.session.remoteSessionId, 'explicit_disconnect');
    assert.equal(terminating.ok, true);
    assert.equal(context.server.remoteSessions.markCleanupPending(
        context.session.remoteSessionId,
        terminating.session.teardownId).ok, true);

    context.server.sendClientList('owner-connection');
    const ownerTarget = messages(context.ownerSocket, 'client_list')
        .at(-1).clients.find(client => client.endpointId === 'B');
    assert.equal(ownerTarget.status, 'Available');
    assert.equal(Object.hasOwn(ownerTarget, 'remoteSessionState'), false);

    context.server.sendClientList('observer-connection');
    const observerTarget = messages(observerSocket, 'client_list')
        .at(-1).clients.find(client => client.endpointId === 'B');
    assert.equal(observerTarget.status, 'Available');
    assert.equal(Object.hasOwn(observerTarget, 'remoteSessionState'), false);
}

// Both peers may enter Grace independently. Each must resume from the exact
// same runtime on a strictly newer transport, and neither resume hides the
// other peer's fixed deadline.
{
    let clock = 20_000;
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `dual-${++sequence}`,
    });
    const opened = registry.open(binding());
    clock = 21_000;
    assert.equal(registry.touch(opened.session.remoteSessionId, 'A', 1, clock).ok, true);
    assert.equal(registry.touch(opened.session.remoteSessionId, 'B', 1, clock).ok, true);
    clock = 21_001;
    registry.markDisconnected('A', clock);
    clock = 21_002;
    registry.markDisconnected('B', clock);
    assert.equal(opened.session.phase, 'Grace');
    assert.deepEqual([...opened.session.graceEndpoints].sort(), ['A', 'B']);
    assert.equal(opened.session.graceDeadlineAt, 24_000);

    clock = 23_999;
    const ownerResume = registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: opened.session.resumeToken, generation: 1,
        connectionGeneration: 2,
    }, clock);
    assert.equal(ownerResume.ok, true, '2999ms remains resumable');
    assert.equal(opened.session.phase, 'Grace');
    assert.deepEqual([...opened.session.graceEndpoints], ['B']);
    assert.equal(registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: opened.session.resumeToken, generation: 2,
        connectionGeneration: 3,
    }, clock).error, 'party_not_in_grace', 'one party cannot replay resume');

    const targetResume = registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'B', runtimeId: 'runtime-B',
        resumeToken: opened.session.resumeToken, generation: 1,
        connectionGeneration: 2,
    }, clock);
    assert.equal(targetResume.ok, true);
    assert.equal(opened.session.phase, 'Active');
    assert.equal(opened.session.graceDeadlineAt, null);
    assert.equal(opened.session.generation, 3);
    assert.equal(registry.touch(opened.session.remoteSessionId, 'A', 1, clock).error,
        'stale_connection_generation');
    assert.equal(registry.acknowledgeState(opened.session.remoteSessionId,
        'A', 2, 3, opened.session.stateRevision), true);
    registry.markDisconnected('A', clock);
    const recoveredOlderObservation = registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: opened.session.resumeToken, generation: 2,
        connectionGeneration: 3,
    }, clock);
    assert.equal(recoveredOlderObservation.ok, true,
        'authenticated recovery reconciles an older observation without rolling commands back');
    assert.equal(opened.session.generation, 4);

}

// Resume proof is bound to both the installation role and runtime. At the
// exact lease boundary it is terminal and returns a transition for the server
// to dispatch teardown exactly once.
{
    let clock = 30_000;
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `resume-${++sequence}`,
    });
    const opened = registry.open(binding());
    clock = 32_999;
    registry.markDisconnected('A', clock);
    const fixedDeadline = opened.session.graceDeadlineAt;
    assert.equal(registry.touch(opened.session.remoteSessionId, 'A', 1, clock).error,
        'resume_required', 'buffered heartbeats cannot substitute for signed resume');
    assert.equal(opened.session.graceDeadlineAt, fixedDeadline,
        'late activity must never push the Grace deadline');
    assert.equal(registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'new-process',
        resumeToken: opened.session.resumeToken, generation: 1,
        connectionGeneration: 2,
    }, clock).error, 'invalid_resume_proof');
    assert.equal(registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: 'wrong-token', generation: 1, connectionGeneration: 2,
    }, clock).error, 'invalid_resume_proof');
    assert.equal(registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: opened.session.resumeToken, generation: 1,
        connectionGeneration: 1,
    }, clock).error, 'stale_connection_generation');

    clock = 33_000;
    const expiredResume = registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: opened.session.resumeToken, generation: 1,
        connectionGeneration: 2,
    }, clock);
    assert.equal(expiredResume.error, 'lease_expired');
    assert.equal(expiredResume.terminalTransition, true);
    assert.equal(opened.session.phase, 'Terminating');
    const teardownId = opened.session.teardownId;
    const repeated = registry.terminate(opened.session.remoteSessionId,
        'another_reason', clock);
    assert.equal(repeated.replay, true);
    assert.equal(repeated.session.teardownId, teardownId,
        'a terminal retry must reuse the same teardown transaction');
    assert.equal(sequence, 3,
        'session id, resume token and exactly one teardown id are generated');
    assert.equal(registry.tick(clock).length, 0,
        'a terminal transition must never be emitted twice by tick');
}

// A socket can remain open while its event loop/heartbeat is frozen. A late
// heartbeat at 2999ms is still accepted; at >=3000ms it cannot restore Active.
{
    let clock = 50_000;
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `freeze-${++sequence}`,
    });
    const opened = registry.open(binding());
    clock = 51_499;
    assert.equal(registry.markDegraded(clock, 1500).length, 0);
    clock = 51_500;
    assert.equal(registry.markDegraded(clock, 1500).length, 2,
        'both silent peers become degraded at 1500ms');
    assert.equal(registry.touch(opened.session.remoteSessionId, 'B', 1, clock).ok, true);
    clock = 52_999;
    assert.equal(registry.tick(clock).length, 0,
        'an open but frozen socket remains recoverable at 2999ms');
    clock = 53_000;
    const lateHeartbeat = registry.touch(
        opened.session.remoteSessionId, 'A', 1, clock);
    assert.equal(lateHeartbeat.error, 'lease_expired');
    assert.equal(lateHeartbeat.terminalTransition, true);
    assert.equal(opened.session.phase, 'Terminating');
    assert.equal(opened.session.lastContact.get('A'), 50_000,
        'the terminal heartbeat must not advance lastContact');
    assert.equal(registry.touch(opened.session.remoteSessionId, 'A', 1, clock).error,
        'session_terminal');
    assert.equal(registry.tick(clock).length, 0);
}

// When both peers vanish and do not resume, the earliest strict contact lease
// closes the shared session exactly once.
{
    let clock = 60_000;
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `offline-${++sequence}`,
    });
    const opened = registry.open(binding());
    registry.markDisconnected('A', clock);
    registry.markDisconnected('B', clock);
    clock = 62_999;
    assert.equal(registry.tick(clock).length, 0);
    clock = 63_000;
    const expired = registry.tick(clock);
    assert.equal(expired.length, 1);
    assert.equal(expired[0].remoteSessionId, opened.session.remoteSessionId);
    const teardownId = expired[0].teardownId;
    assert.equal(registry.tick(clock).length, 0);
    assert.equal(registry.markDisconnected('A', clock).length, 0);
    assert.equal(opened.session.teardownId, teardownId);
}

// Lost acknowledgements, duplicates, and late messages replay the committed
// tombstone without reopening the session or freeing a different target.
{
    let clock = 70_000;
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `ack-${++sequence}`,
    });
    const opened = registry.open(binding());
    assert.equal(registry.markCleanupPending(
        opened.session.remoteSessionId, null, clock).error, 'invalid_teardown');
    assert.equal(registry.acknowledgeCleanup(
        opened.session.remoteSessionId, undefined, 'B', committedCleanup(), clock).error,
    'invalid_teardown_ack', 'an ACK cannot close an Active session');
    const firstTermination = registry.terminate(
        opened.session.remoteSessionId, 'explicit_disconnect', clock);
    const duplicateTermination = registry.terminate(
        opened.session.remoteSessionId, 'late_duplicate', clock);
    assert.equal(firstTermination.replay, false);
    assert.equal(duplicateTermination.replay, true);
    assert.equal(duplicateTermination.session.teardownId,
        firstTermination.session.teardownId);
    assert.equal(registry.markCleanupPending(
        opened.session.remoteSessionId, firstTermination.session.teardownId, clock).ok, true);

    const result = committedCleanup({ removedFileCount: 9, quarantinedBytes: 1234 });
    const committed = registry.acknowledgeCleanup(
        opened.session.remoteSessionId, firstTermination.session.teardownId,
        'B', result, clock);
    assert.equal(committed.ok, true);
    assert.equal(committed.replay, false);
    assert.equal(registry.incomingForTarget('B').length, 0);

    // Simulate the server->client CLOSED response being lost: the target sends
    // the same commit ACK again and receives the same tombstone result.
    const retry = registry.acknowledgeCleanup(
        opened.session.remoteSessionId, firstTermination.session.teardownId,
        'B', result, clock);
    assert.equal(retry.ok, true);
    assert.equal(retry.replay, true);
    assert.equal(retry.session.teardownId, firstTermination.session.teardownId);
    assert.equal(registry.acknowledgeCleanup(
        opened.session.remoteSessionId, firstTermination.session.teardownId,
        'A', result, clock).ok, false, 'the owner cannot forge a target ACK');
    assert.equal(registry.acknowledgeCleanup(
        opened.session.remoteSessionId, firstTermination.session.teardownId,
        'B', { ...result, cacheQuarantined: false }, clock).ok, false,
    'a malformed duplicate cannot replay success');
    assert.equal(registry.acknowledgeCleanup(
        opened.session.remoteSessionId, 'obsolete-teardown', 'B', result, clock).ok, false);
    assert.equal(registry.touch(opened.session.remoteSessionId, 'A', 1, clock).error,
        'session_terminal');
    assert.equal(registry.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: opened.session.resumeToken, generation: 1,
        connectionGeneration: 2,
    }, clock).error, 'session_not_resumable');
    assert.equal(registry.terminate(opened.session.remoteSessionId,
        'late_close', clock).replay, true);
}

// Cleanup delivery retries use the same immutable teardown transaction. The
// delay doubles up to a cap, while the owner/target index remains fail-closed
// until the target commits its acknowledgement.
{
    let clock = 80_000;
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        cleanupRetryInitialMs: 100,
        cleanupRetryMaxMs: 250,
        now: () => clock,
        idFactory: () => `cleanup-retry-${++sequence}`,
    });
    const opened = registry.open(binding());
    const terminating = registry.terminate(
        opened.session.remoteSessionId, 'explicit_disconnect', clock);
    const teardownId = terminating.session.teardownId;
    const firstDispatch = registry.recordCleanupDispatch(
        opened.session.remoteSessionId, teardownId, clock);
    assert.equal(firstDispatch.ok, true);
    assert.equal(firstDispatch.retryDelay, 100);
    assert.equal(registry.markCleanupPending(
        opened.session.remoteSessionId, teardownId, clock).ok, true);

    clock = 80_099;
    assert.equal(registry.dueCleanupRetries(clock).length, 0);
    clock = 80_100;
    assert.deepEqual(registry.dueCleanupRetries(clock), [opened.session]);
    const secondDispatch = registry.recordCleanupDispatch(
        opened.session.remoteSessionId, teardownId, clock);
    assert.equal(secondDispatch.retryDelay, 200);

    clock = 80_299;
    assert.equal(registry.dueCleanupRetries(clock).length, 0);
    clock = 80_300;
    assert.equal(registry.dueCleanupRetries(clock).length, 1);
    const thirdDispatch = registry.recordCleanupDispatch(
        opened.session.remoteSessionId, teardownId, clock);
    assert.equal(thirdDispatch.retryDelay, 250,
        'the exponential retry delay is capped');
    assert.equal(opened.session.teardownId, teardownId);
    assert.equal(opened.session.generation, 1);
    assert.equal(registry.forOwnerTarget('A', 'B'), opened.session,
        'retry scheduling must retain the owner/target exclusion index');
    assert.equal(registry.open(binding()).error, 'session_cleanup_pending');

    clock = 80_550;
    assert.equal(registry.dueCleanupRetries(clock).length, 1);
    assert.equal(registry.acknowledgeCleanup(
        opened.session.remoteSessionId, teardownId, 'B', committedCleanup(), clock).ok,
    true);
    assert.equal(registry.dueCleanupRetries(clock).length, 0,
        'a committed cleanup removes all future retry work');
    assert.equal(registry.forOwnerTarget('A', 'B'), null);
}

// Grace does not mask a second peer whose still-open socket stopped sending
// heartbeats earlier. The earliest per-party lastContact deadline wins.
{
    let clock = 90_000;
    let sequence = 0;
    const registry = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `mixed-freeze-${++sequence}`,
    });
    const opened = registry.open(binding());
    clock = 92_000;
    assert.equal(registry.touch(opened.session.remoteSessionId, 'A', 1, clock).ok, true);
    registry.markDisconnected('A', clock);
    assert.equal(opened.session.graceDeadlineAt, 95_000);
    clock = 93_000;
    const expired = registry.tick(clock);
    assert.equal(expired.length, 1,
        'B last contacted at 90000 and expires before A Grace deadline');
    assert.equal(expired[0].teardownReason, 'lease_expired');
}

// Server integration: a heartbeat received at/after its strict deadline
// dispatches teardown once. Subsequent late heartbeats see CleanupPending and
// cannot execute the teardown side effects again.
{
    const context = serverSessionContext('server-heartbeat');
    const now = Date.now();
    context.session.lastContact.set('A', now - 3000);
    context.session.lastContact.set('B', now);

    context.server.handleHeartbeat('owner-connection', {
        connectionGeneration: 1,
        sequence: 1,
        clientMonotonicMs: 10,
    });
    assert.equal(context.session.phase, 'CleanupPending');
    assert.equal(context.beginAttempts(), 1);
    assert.equal(context.beginCommits(), 1);
    assert.equal(context.session.teardownDispatchStarted, true);

    context.server.handleHeartbeat('owner-connection', {
        connectionGeneration: 1,
        sequence: 2,
        clientMonotonicMs: 11,
    });
    assert.equal(context.beginAttempts(), 1,
        'late heartbeat retries must not call beginRemoteSessionTeardown again');
    assert.equal(context.beginCommits(), 1);
    assert.equal(messages(context.ownerSocket, 'heartbeat_ack').length, 2,
        'the authenticated transport itself remains usable after session expiry');
    for (const acknowledgement of messages(context.ownerSocket, 'heartbeat_ack')) {
        assert.equal(Number.isSafeInteger(
            acknowledgement.serverReceiveMonotonicMs), true);
        assert.equal(Number.isSafeInteger(
            acknowledgement.serverTransmitMonotonicMs), true);
        assert.ok(acknowledgement.serverTransmitMonotonicMs
            >= acknowledgement.serverReceiveMonotonicMs);
        assert.equal(acknowledgement.serverMonotonicMs,
            acknowledgement.serverTransmitMonotonicMs,
            'the compatibility timestamp is the NTP transmit timestamp');
    }
}

// Server integration: an exact-boundary resume failure follows the same
// one-shot teardown path. A repeated same-process resume catches up the
// terminal state on the replacement transport without re-running teardown.
{
    const context = serverSessionContext('server-resume');
    const now = Date.now();
    context.session.phase = 'Grace';
    context.session.graceEndpoints.add('A');
    context.session.graceDeadlineAt = now;
    context.session.lastContact.set('A', now - 3000);
    context.session.lastContact.set('B', now);
    context.server.clients.get('owner-connection').connectionGeneration = 2;

    const resumeMessage = {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        resumeToken: context.session.resumeToken,
    };
    context.server.handleRemoteSessionResume('owner-connection', resumeMessage);
    assert.equal(context.session.phase, 'CleanupPending');
    assert.equal(context.beginAttempts(), 1);
    assert.equal(context.beginCommits(), 1);
    assert.equal(messages(context.ownerSocket, 'error').at(-1).code, 'lease_expired');

    context.server.handleRemoteSessionResume('owner-connection', resumeMessage);
    assert.equal(context.beginAttempts(), 1);
    const catchUp = messages(
        context.ownerSocket, 'remote_session_terminating').at(-1);
    assert.equal(catchUp.replay, true);
    assert.equal(catchUp.ownerConnectionGeneration, 2);
    assert.equal(catchUp.targetConnectionGeneration, 1);
    assert.equal(catchUp.connectionGeneration, 2);
    assert.equal(messages(context.ownerSocket, 'error').length, 1,
        'terminal catch-up replaces a misleading session_not_resumable error');
}

// A live server resume accepts exactly the generation last delivered to that
// role, then advances both roles that receive the resumed state. A later
// transport cannot roll that synchronized generation back.
{
    const context = serverSessionContext('server-generation-resume');
    context.server.remoteSessions.markDisconnected('A');
    const ownerClient = context.server.clients.get('owner-connection');
    ownerClient.connectionGeneration = 2;
    const baseResume = {
        remoteSessionId: context.session.remoteSessionId,
        resumeToken: context.session.resumeToken,
    };
    context.server.handleRemoteSessionResume('owner-connection', {
        ...baseResume,
        generation: 2,
    });
    assert.equal(messages(context.ownerSocket, 'error').at(-1).code,
        'stale_remote_session_generation');
    assert.equal(context.session.phase, 'Grace');

    context.server.handleRemoteSessionResume('owner-connection', {
        ...baseResume,
        generation: 1,
    });
    assert.equal(context.session.phase, 'Active');
    assert.equal(context.session.generation, 2);
    assert.equal(context.session.ownerKnownGeneration, 1);
    assert.equal(context.session.targetKnownGeneration, 1,
        'enqueueing a frame does not acknowledge client application');
    assert.equal(messages(context.targetSocket, 'remote_session_resumed').length, 1);

    context.server.remoteSessions.markDisconnected('A');
    ownerClient.connectionGeneration = 3;
    context.server.handleRemoteSessionResume('owner-connection', {
        ...baseResume,
        generation: 1,
    });
    assert.equal(context.session.phase, 'Active');
    assert.equal(context.session.generation, 3);
}

// Explicit close, lost CLOSED delivery, duplicate close, and duplicate cleanup
// ACK all replay protocol results while the destructive teardown body runs once.
{
    const context = serverSessionContext('server-close');
    const closeMessage = {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        requestId: 'close-request',
    };
    context.server.handleRemoteSessionClose('owner-connection', closeMessage);
    assert.equal(context.beginAttempts(), 1);
    assert.equal(context.beginCommits(), 1);
    const teardownId = context.session.teardownId;

    context.server.handleRemoteSessionClose('owner-connection', closeMessage);
    assert.equal(context.beginAttempts(), 1);
    assert.equal(context.beginCommits(), 1);
    assert.equal(messages(context.ownerSocket, 'remote_session_terminating').at(-1).replay,
        true);

    const ackMessage = {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        teardownId,
        ...committedCleanup({ quarantinedBytes: 42 }),
    };
    context.server.handleRemoteSessionTeardownAck('target-connection', ackMessage);
    assert.equal(context.server.remoteSessions.get(context.session.remoteSessionId), null);
    assert.equal(messages(context.ownerSocket, 'remote_session_closed').length, 2);

    // Treat the first CLOSED response as lost and retry both possible source
    // messages. Neither path re-enters teardown or mutates a new session.
    context.server.handleRemoteSessionTeardownAck('target-connection', ackMessage);
    assert.equal(messages(context.ownerSocket, 'remote_session_closed').length, 3);
    assert.equal(messages(context.ownerSocket, 'remote_session_closed').at(-1).cleanupState,
        'confirmed');
    context.server.handleRemoteSessionClose('owner-connection', closeMessage);
    assert.equal(messages(context.ownerSocket, 'remote_session_closed').length, 4);
    assert.equal(messages(context.ownerSocket, 'remote_session_closed').at(-1).replay, true);
    assert.equal(context.beginAttempts(), 1);

    context.server.handleRemoteSessionTeardownAck('target-connection', {
        ...ackMessage,
        generation: ackMessage.generation + 1,
        requestId: 'stale-cleanup-ack',
    });
    const staleAck = messages(context.targetSocket, 'error').at(-1);
    assert.equal(staleAck.scope, 'remote_session');
    assert.equal(staleAck.code, 'stale_remote_session_generation');
    assert.equal(staleAck.requestId, 'stale-cleanup-ack');
    assert.equal(staleAck.remoteSessionId, context.session.remoteSessionId);
    assert.equal(Object.hasOwn(staleAck, 'resumeToken'), false);
}

// A same-runtime authenticated transport replacement may close the exact
// non-terminal session without resuming command authority. The active binding
// remains immutable; only terminal delivery advances to the new transport.
{
    let monotonic = 300_000;
    let epoch = Date.now();
    const server = new MouffetteServer({
        port: 0,
        metricLogger: () => {},
        monotonicNow: () => monotonic,
        epochNow: () => epoch,
    });
    const keys = crypto.generateKeyPairSync('ed25519');
    const runtimeId = crypto.randomUUID();
    const replacement = addAuthenticationCandidate(
        server, 'replacement-owner', keys, runtimeId, epoch);
    const oldSocket = addAuthenticatedClient(
        server, 'old-owner-transport', replacement.endpointId);
    const oldClient = server.clients.get('old-owner-transport');
    oldClient.runtimeId = runtimeId;
    oldClient.lastHeartbeatAt = epoch;
    oldClient.lastHeartbeatMonotonicAt = monotonic;
    server.connectionGenerationSequence = 1;
    const targetSocket = addAuthenticatedClient(
        server, 'replacement-close-target', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: replacement.endpointId,
        targetEndpointId: 'B',
        ownerRuntimeId: runtimeId,
        targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1,
        targetConnectionGeneration: 1,
    }).session;

    monotonic += 1;
    epoch += 1;
    server.handleAuthResponse('replacement-owner', replacement.response, {
        monotonicMs: monotonic,
        epochMs: epoch,
    });
    assert.equal(replacement.client.authenticated, true);
    assert.equal(replacement.client.connectionGeneration, 2);
    assert.equal(server.clients.has('old-owner-transport'), false);
    assert.equal(oldSocket.readyState, WebSocket.CLOSED);
    assert.equal(session.phase, 'Grace');

    const close = {
        remoteSessionId: session.remoteSessionId,
        generation: session.generation,
        connectionGeneration: 2,
        requestId: 'same-runtime-rebound-close',
    };
    server.handleRemoteSessionClose('old-owner-transport', {
        ...close,
        connectionGeneration: 1,
        requestId: 'retired-transport-close',
    });
    assert.equal(session.phase, 'Grace',
        'a retired transport cannot mutate the session');
    server.handleRemoteSessionClose('replacement-owner', {
        ...close,
        connectionGeneration: 1,
        requestId: 'old-generation-close',
    });
    const stale = messages(replacement.ws, 'error').at(-1);
    assert.equal(stale.code, 'stale_connection_generation');
    assert.equal(stale.requestId, 'old-generation-close');
    assert.equal(stale.remoteSessionId, session.remoteSessionId);
    assert.equal(session.phase, 'Grace');

    server.handleRemoteSessionClose('replacement-owner', close);
    assert.equal(session.phase, 'CleanupPending');
    assert.equal(session.ownerConnectionGeneration, 1,
        'CLOSE recovery must not grant command authority to the new transport');
    assert.equal(session.ownerTerminalConnectionGeneration, 2);
    assert.equal(session.ownerTerminalRuntimeId, runtimeId);
    const terminal = messages(
        replacement.ws, 'remote_session_terminating').at(-1);
    assert.equal(terminal.requestId, close.requestId);
    assert.equal(terminal.connectionGeneration, 2);
    assert.equal(terminal.ownerConnectionGeneration, 2);
    assert.equal(terminal.targetConnectionGeneration, 1);
    assert.equal(messages(targetSocket, 'remote_session_terminating').length, 1);
}

// Matching the endpoint is insufficient: a replacement process cannot close a
// non-terminal session that belongs to another runtime.
{
    const context = serverSessionContext('different-runtime-close');
    const owner = context.server.clients.get('owner-connection');
    owner.runtimeId = 'different-runtime';
    owner.connectionGeneration = 2;
    context.server.connectionGenerationSequence = 2;
    context.server.handleRemoteSessionClose('owner-connection', {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 2,
        requestId: 'different-runtime-close-request',
    });
    const rejected = messages(context.ownerSocket, 'error').at(-1);
    assert.equal(rejected.code, 'invalid_resume_proof');
    assert.equal(rejected.requestId, 'different-runtime-close-request');
    assert.equal(rejected.remoteSessionId, context.session.remoteSessionId);
    assert.equal(context.session.phase, 'Active');
    assert.equal(context.session.ownerTerminalConnectionGeneration, undefined);
    assert.equal(messages(context.targetSocket, 'remote_session_terminating').length, 0);
}

// Once a CLOSED tombstone expires, CLOSE returns a correlated authoritative
// absence instead of leaving the caller waiting for a terminal replay forever.
{
    let clock = 400_000;
    let sequence = 0;
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    server.remoteSessions = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        tombstoneTtlMs: 100,
        now: () => clock,
        idFactory: () => `expired-close-${++sequence}`,
    });
    const ownerSocket = addAuthenticatedClient(server, 'expired-close-owner', 'A');
    addAuthenticatedClient(server, 'expired-close-target', 'B');
    const session = server.remoteSessions.open(binding()).session;
    const terminating = server.remoteSessions.terminate(
        session.remoteSessionId, 'explicit_disconnect', clock);
    server.remoteSessions.markCleanupPending(
        session.remoteSessionId, terminating.session.teardownId, clock);
    server.remoteSessions.acknowledgeCleanup(
        session.remoteSessionId, terminating.session.teardownId,
        'B', committedCleanup(), clock);
    clock += 100;
    server.remoteSessions.tick(clock);
    assert.equal(server.remoteSessions.getTombstone(session.remoteSessionId), null);

    server.handleRemoteSessionClose('expired-close-owner', {
        remoteSessionId: session.remoteSessionId,
        generation: session.generation,
        connectionGeneration: 1,
        requestId: 'expired-tombstone-close',
    });
    const absent = messages(ownerSocket, 'error').at(-1);
    assert.equal(absent.code, 'unknown_remote_session');
    assert.equal(absent.scope, 'remote_session');
    assert.equal(absent.requestId, 'expired-tombstone-close');
    assert.equal(absent.remoteSessionId, session.remoteSessionId);
}

// If the initial target delivery is missed, the lease sweep replays only the
// teardown envelope. Destructive server-side teardown remains one-shot and the
// session cannot be replaced until the target ACK is committed.
{
    let clock = 200_000;
    const context = serverSessionContext('server-cleanup-retry');
    context.server.remoteSessions.now = () => clock;
    context.server.remoteSessions.cleanupRetryInitialMs = 100;
    context.server.remoteSessions.cleanupRetryMaxMs = 250;
    context.targetSocket.readyState = WebSocket.CLOSED;
    const closeMessage = {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        requestId: 'retry-close-request',
    };

    context.server.handleRemoteSessionClose('owner-connection', closeMessage);
    const teardownId = context.session.teardownId;
    assert.equal(context.session.phase, 'CleanupPending');
    assert.equal(context.session.cleanupDispatchAttempts, 1,
        'a failed target send still advances the retry backoff');
    assert.equal(messages(context.targetSocket, 'remote_session_terminating').length, 0);
    assert.equal(context.beginCommits(), 1);
    assert.equal(context.server.remoteSessions.forOwnerTarget('A', 'B'), context.session);

    clock = 200_099;
    context.server.sweepRemoteSessionLeases(clock);
    assert.equal(context.session.cleanupDispatchAttempts, 1);
    context.targetSocket.readyState = WebSocket.OPEN;
    clock = 200_100;
    context.server.sweepRemoteSessionLeases(clock);
    const replay = messages(
        context.targetSocket, 'remote_session_terminating').at(-1);
    assert.equal(replay.replay, true);
    assert.equal(replay.phase, 'CleanupPending');
    assert.equal(replay.remoteSessionId, context.session.remoteSessionId);
    assert.equal(replay.teardownId, teardownId);
    assert.equal(replay.generation, context.session.generation);
    assert.equal(context.session.cleanupDispatchAttempts, 2);
    assert.equal(context.beginCommits(), 1,
        'retry delivery must not re-run scene/upload/removal teardown');
    assert.equal(messages(context.ownerSocket, 'remote_session_terminating').length, 1,
        'periodic retries are sent only to the ACK-authoritative target');

    context.server.handleRemoteSessionTeardownAck('target-connection', {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        teardownId,
        ...committedCleanup(),
    });
    assert.equal(context.server.remoteSessions.get(context.session.remoteSessionId), null);
    assert.equal(context.server.remoteSessions.forOwnerTarget('A', 'B'), null);
    const targetRetryCount = messages(
        context.targetSocket, 'remote_session_terminating').length;
    clock = 210_000;
    context.server.sweepRemoteSessionLeases(clock);
    assert.equal(messages(context.targetSocket, 'remote_session_terminating').length,
        targetRetryCount, 'no teardown retry survives a committed ACK');
}

// Replaying the exact OPEN request after it entered teardown returns only the
// old terminal transaction to the owner. It must never emit a terminal-phase
// offer to the target; a genuinely new requestId can open after cleanup.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const ownerSocket = addAuthenticatedClient(server, 'open-replay-owner', 'A');
    const targetSocket = addAuthenticatedClient(server, 'open-replay-target', 'B');
    const openMessage = {
        targetEndpointId: 'B',
        connectionGeneration: 1,
        requestId: 'terminal-open-request',
    };
    server.handleRemoteSessionOpen('open-replay-owner', openMessage);
    const oldSession = server.remoteSessions.forOwnerTarget('A', 'B');
    assert.ok(oldSession);
    assert.equal(messages(targetSocket, 'remote_session_offer').length, 1);

    server.handleRemoteSessionClose('open-replay-owner', {
        remoteSessionId: oldSession.remoteSessionId,
        generation: oldSession.generation,
        connectionGeneration: 1,
        requestId: 'close-terminal-open-request',
    });
    assert.equal(oldSession.phase, 'CleanupPending');
    const offerCount = messages(targetSocket, 'remote_session_offer').length;
    server.handleRemoteSessionOpen('open-replay-owner', openMessage);
    assert.equal(messages(targetSocket, 'remote_session_offer').length, offerCount,
        'a CleanupPending request replay must not emit another offer');
    const cleanupReplay = messages(
        ownerSocket, 'remote_session_terminating').at(-1);
    assert.equal(cleanupReplay.replay, true);
    assert.equal(cleanupReplay.requestId, openMessage.requestId);
    assert.equal(cleanupReplay.remoteSessionId, oldSession.remoteSessionId);

    server.handleRemoteSessionTeardownAck('open-replay-target', {
        remoteSessionId: oldSession.remoteSessionId,
        generation: oldSession.generation,
        connectionGeneration: 1,
        teardownId: oldSession.teardownId,
        ...committedCleanup(),
    });
    const closedCount = messages(ownerSocket, 'remote_session_closed').length;
    server.handleRemoteSessionOpen('open-replay-owner', openMessage);
    assert.equal(messages(targetSocket, 'remote_session_offer').length, offerCount,
        'a Closed request replay must not emit another offer');
    assert.equal(messages(ownerSocket, 'remote_session_closed').length, closedCount + 1);
    const closedReplay = messages(ownerSocket, 'remote_session_closed').at(-1);
    assert.equal(closedReplay.replay, true);
    assert.equal(closedReplay.requestId, openMessage.requestId);
    assert.equal(closedReplay.remoteSessionId, oldSession.remoteSessionId);

    server.handleRemoteSessionOpen('open-replay-owner', {
        ...openMessage,
        requestId: 'fresh-open-request',
    });
    const freshSession = server.remoteSessions.forOwnerTarget('A', 'B');
    assert.ok(freshSession);
    assert.notEqual(freshSession.remoteSessionId, oldSession.remoteSessionId);
    assert.equal(messages(targetSocket, 'remote_session_offer').length, offerCount + 1);
}

// OPEN replay never migrates a non-terminal command binding to a replacement
// transport. Grace requires the proof-bearing Resume path; an abandoned
// Opening is terminalized because no resume token was ever delivered.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const ownerSocket = addAuthenticatedClient(
        server, 'replay-phase-owner', 'A');
    const targetSocket = addAuthenticatedClient(
        server, 'replay-phase-target', 'B');
    const openMessage = {
        targetEndpointId: 'B',
        connectionGeneration: 1,
        requestId: 'grace-replay-request',
    };
    server.handleRemoteSessionOpen('replay-phase-owner', openMessage);
    const session = server.remoteSessions.forOwnerTarget('A', 'B');
    assert.ok(session);
    assert.equal(server.remoteSessions.accept({
        remoteSessionId: session.remoteSessionId,
        targetEndpointId: 'B',
        targetRuntimeId: 'runtime-B',
        generation: 1,
        connectionGeneration: 1,
    }).ok, true);
    server.remoteSessions.markDisconnected('A');
    assert.equal(session.phase, 'Grace');

    const initialOfferCount = messages(
        targetSocket, 'remote_session_offer').length;
    server.clients.get('replay-phase-owner').connectionGeneration = 2;
    server.handleRemoteSessionOpen('replay-phase-owner', {
        ...openMessage,
        connectionGeneration: 2,
    });
    const graceError = messages(ownerSocket, 'error').at(-1);
    assert.equal(graceError.code, 'session_requires_resume');
    assert.equal(graceError.requestId, openMessage.requestId);
    assert.equal(graceError.remoteSessionId, session.remoteSessionId);
    assert.equal(session.phase, 'Grace');
    assert.equal(messages(targetSocket, 'remote_session_offer').length,
        initialOfferCount, 'Grace replay must never be relayed as an offer');
}

{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const ownerSocket = addAuthenticatedClient(
        server, 'abandoned-opening-owner', 'A');
    const targetSocket = addAuthenticatedClient(
        server, 'abandoned-opening-target', 'B');
    const openMessage = {
        targetEndpointId: 'B',
        connectionGeneration: 1,
        requestId: 'abandoned-opening-request',
    };
    server.handleRemoteSessionOpen('abandoned-opening-owner', openMessage);
    const session = server.remoteSessions.forOwnerTarget('A', 'B');
    assert.ok(session);
    assert.equal(session.phase, 'Opening');
    const initialOfferCount = messages(
        targetSocket, 'remote_session_offer').length;
    const initialOpeningCount = messages(
        ownerSocket, 'remote_session_opening').length;

    server.clients.get('abandoned-opening-owner').connectionGeneration = 2;
    server.handleRemoteSessionOpen('abandoned-opening-owner', {
        ...openMessage,
        connectionGeneration: 2,
    });
    assert.equal(session.phase, 'CleanupPending');
    assert.equal(session.teardownDispatchStarted, true);
    const cleanupError = messages(ownerSocket, 'error').at(-1);
    assert.equal(cleanupError.code, 'session_cleanup_pending');
    assert.equal(cleanupError.requestId, openMessage.requestId);
    assert.equal(cleanupError.remoteSessionId, session.remoteSessionId);
    assert.equal(messages(targetSocket, 'remote_session_offer').length,
        initialOfferCount,
        'an Opening from an old transport must not be offered again');
    assert.equal(messages(ownerSocket, 'remote_session_opening').length,
        initialOpeningCount,
        'the replacement transport must not receive stale Opening state');
}

// Defensive race coverage: even if an Active session has not yet observed the
// old socket departure, duplicate OPEN cannot bypass Resume on generation 2.
{
    const server = new MouffetteServer({ port: 0, metricLogger: () => {} });
    const ownerSocket = addAuthenticatedClient(
        server, 'active-replay-owner', 'A');
    addAuthenticatedClient(server, 'active-replay-target', 'B');
    const openMessage = {
        targetEndpointId: 'B',
        connectionGeneration: 1,
        requestId: 'active-replay-request',
    };
    server.handleRemoteSessionOpen('active-replay-owner', openMessage);
    const session = server.remoteSessions.forOwnerTarget('A', 'B');
    assert.equal(server.remoteSessions.accept({
        remoteSessionId: session.remoteSessionId,
        targetEndpointId: 'B',
        targetRuntimeId: 'runtime-B',
        generation: 1,
        connectionGeneration: 1,
    }).ok, true);
    server.clients.get('active-replay-owner').connectionGeneration = 2;
    server.handleRemoteSessionOpen('active-replay-owner', {
        ...openMessage,
        connectionGeneration: 2,
    });
    const error = messages(ownerSocket, 'error').at(-1);
    assert.equal(error.code, 'session_requires_resume');
    assert.equal(error.requestId, openMessage.requestId);
    assert.equal(error.remoteSessionId, session.remoteSessionId);
    assert.equal(messages(ownerSocket, 'remote_session_opened').length, 0);
    assert.equal(session.phase, 'Active');
}

// Both roles can recover terminal delivery after a same-process transport
// replacement. Each recipient gets its current top-level/local-role transport
// generation, while the immutable command tuple and peer generation remain
// unchanged. CLOSED tombstones replay the same way without resurrection.
{
    const context = serverSessionContext('terminal-catch-up');
    context.server.handleRemoteSessionClose('owner-connection', {
        remoteSessionId: context.session.remoteSessionId,
        generation: 1,
        connectionGeneration: 1,
        requestId: 'terminal-catch-up-close',
    });
    const teardownId = context.session.teardownId;

    const ownerClient = context.server.clients.get('owner-connection');
    ownerClient.connectionGeneration = 2;
    const ownerReconciliationStart = context.ownerSocket.messages.length;
    context.server.handleEndpointSnapshot('owner-connection', {
        connectionGeneration: 2,
        machineName: 'Owner rebound', platform: 'test', instanceOrdinal: 1,
        screens: [], systemUI: [],
        volumePercent: 50,
    });
    assert.deepEqual(context.ownerSocket.messages
        .slice(ownerReconciliationStart).map(message => message.type), [
        'endpoint_snapshot_applied',
        'remote_session_terminating',
        'client_list',
    ], 'live terminal catch-up must precede the registering socket client list');
    const ownerTerminal = messages(
        context.ownerSocket, 'remote_session_terminating').at(-1);
    assert.equal(ownerTerminal.replay, true);
    assert.equal(ownerTerminal.connectionGeneration, 2);
    assert.equal(ownerTerminal.ownerConnectionGeneration, 2);
    assert.equal(ownerTerminal.targetConnectionGeneration, 1);
    assert.equal(context.session.ownerConnectionGeneration, 1);

    const targetClient = context.server.clients.get('target-connection');
    targetClient.connectionGeneration = 2;
    const targetReconciliationStart = context.targetSocket.messages.length;
    context.server.handleEndpointSnapshot('target-connection', {
        connectionGeneration: 2,
        machineName: 'Target rebound', platform: 'test', instanceOrdinal: 1,
        screens: [], systemUI: [],
        volumePercent: 50,
    });
    assert.deepEqual(context.targetSocket.messages
        .slice(targetReconciliationStart).map(message => message.type), [
        'endpoint_snapshot_applied',
        'remote_session_terminating',
        'client_list',
    ], 'target cleanup replay must precede its registering socket client list');
    const targetTerminal = messages(
        context.targetSocket, 'remote_session_terminating').at(-1);
    assert.equal(targetTerminal.replay, true);
    assert.equal(targetTerminal.connectionGeneration, 2);
    assert.equal(targetTerminal.ownerConnectionGeneration, 1);
    assert.equal(targetTerminal.targetConnectionGeneration, 2);
    assert.equal(context.session.targetConnectionGeneration, 1);

    context.server.handleRemoteSessionTeardownAck('target-connection', {
        remoteSessionId: context.session.remoteSessionId,
        generation: 1,
        connectionGeneration: 2,
        teardownId,
        ...committedCleanup(),
    });
    const ownerClosed = messages(
        context.ownerSocket, 'remote_session_closed').at(-1);
    const targetClosed = messages(
        context.targetSocket, 'remote_session_closed').at(-1);
    assert.equal(ownerClosed.connectionGeneration, 2);
    assert.equal(ownerClosed.ownerConnectionGeneration, 2);
    assert.equal(ownerClosed.targetConnectionGeneration, 1);
    assert.equal(targetClosed.connectionGeneration, 2);
    assert.equal(targetClosed.ownerConnectionGeneration, 1);
    assert.equal(targetClosed.targetConnectionGeneration, 2);

    ownerClient.connectionGeneration = 3;
    const closedCount = messages(context.ownerSocket, 'remote_session_closed').length;
    const tombstoneReconciliationStart = context.ownerSocket.messages.length;
    context.server.handleEndpointSnapshot('owner-connection', {
        connectionGeneration: 3,
        machineName: 'Owner rebound again', platform: 'test', instanceOrdinal: 1,
        screens: [], systemUI: [],
        volumePercent: 50,
    });
    assert.deepEqual(context.ownerSocket.messages
        .slice(tombstoneReconciliationStart).map(message => message.type), [
        'endpoint_snapshot_applied',
        'remote_session_terminating',
        'remote_session_closed',
        'client_list',
    ], 'the full tombstone replay must precede the registering socket client list');
    assert.equal(messages(context.ownerSocket, 'remote_session_closed').length,
        closedCount + 1);
    const replayedClosed = messages(
        context.ownerSocket, 'remote_session_closed').at(-1);
    assert.equal(replayedClosed.replay, true);
    assert.equal(replayedClosed.connectionGeneration, 3);
    assert.equal(replayedClosed.ownerConnectionGeneration, 3);
    assert.equal(replayedClosed.targetConnectionGeneration, 1);

    ownerClient.runtimeId = 'different-owner-process';
    ownerClient.connectionGeneration = 4;
    const terminalCountBeforeForeignRuntime = messages(
        context.ownerSocket, 'remote_session_terminating').length;
    const closedCountBeforeForeignRuntime = messages(
        context.ownerSocket, 'remote_session_closed').length;
    const emptyReconciliationStart = context.ownerSocket.messages.length;
    context.server.handleEndpointSnapshot('owner-connection', {
        connectionGeneration: 4,
        machineName: 'Different owner process', platform: 'test', instanceOrdinal: 1,
        screens: [], systemUI: [],
        volumePercent: 50,
    });
    assert.deepEqual(context.ownerSocket.messages
        .slice(emptyReconciliationStart).map(message => message.type), [
        'endpoint_snapshot_applied',
        'client_list',
    ], 'without a runtime-matching terminal state the barrier contains no replay');
    assert.equal(messages(context.ownerSocket, 'remote_session_terminating').length,
        terminalCountBeforeForeignRuntime);
    assert.equal(messages(context.ownerSocket, 'remote_session_closed').length,
        closedCountBeforeForeignRuntime);
}

// Terminal frames are runtime-scoped even outside the explicit replay path,
// and a RESUME may only catch up the exact requested session rather than hide
// an error behind an unrelated terminal session owned by the same device.
{
    const context = serverSessionContext('terminal-runtime-scope');
    const owner = context.server.clients.get('owner-connection');
    owner.runtimeId = 'different-owner-process';
    owner.connectionGeneration = 2;
    const terminal = context.server.remoteSessions.terminate(
        context.session.remoteSessionId, 'lease_expired');
    context.server.beginRemoteSessionTeardown(terminal.session);
    assert.equal(messages(context.ownerSocket, 'remote_session_terminating').length, 0,
        'a replacement owner process must not receive the prior runtime terminal state');
    assert.equal(messages(context.targetSocket, 'remote_session_terminating').length, 1);

    context.server.handleRemoteSessionResume('owner-connection', {
        remoteSessionId: 'some-other-session',
        generation: 1,
        resumeToken: context.session.resumeToken,
    });
    assert.equal(messages(context.ownerSocket, 'remote_session_terminating').length, 0,
        'an unrelated terminal session must not satisfy catch-up');
    assert.equal(messages(context.ownerSocket, 'error').at(-1).code,
        'session_not_resumable');
}

// Either authenticated party may initiate a clean shutdown. The target can
// therefore close its incoming session, while a third device remains unable
// to terminate it and caller-provided reasons cannot escape the safe set.
{
    const context = serverSessionContext('target-clean-close');
    const attackerSocket = addAuthenticatedClient(
        context.server, 'attacker-connection', 'C');
    const close = {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        requestId: 'target-clean-quit',
        reason: 'clean_shutdown',
    };
    context.server.handleRemoteSessionClose('target-connection', {
        ...close,
        generation: close.generation + 1,
        requestId: 'target-stale-close',
    });
    assert.equal(messages(context.targetSocket, 'error').at(-1).code,
        'stale_remote_session_generation');
    assert.equal(context.session.phase, 'Active');
    assert.equal(context.beginAttempts(), 0);

    context.server.handleRemoteSessionClose('target-connection', close);
    assert.equal(context.session.phase, 'CleanupPending');
    assert.equal(context.session.teardownReason, 'clean_shutdown');
    assert.equal(context.beginAttempts(), 1);
    assert.equal(context.beginCommits(), 1);

    context.server.handleRemoteSessionClose('target-connection', {
        ...close,
        requestId: 'target-clean-quit-retry',
        reason: 'untrusted-reason',
    });
    assert.equal(context.session.teardownReason, 'clean_shutdown',
        'a retry cannot replace the first terminal reason');
    assert.equal(context.beginAttempts(), 1);

    context.server.handleRemoteSessionClose('attacker-connection', close);
    assert.equal(messages(attackerSocket, 'error').at(-1).code,
        'not_a_session_party');
    assert.equal(context.beginAttempts(), 1);

    context.server.handleRemoteSessionTeardownAck('target-connection', {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        teardownId: context.session.teardownId,
        ...committedCleanup(),
    });
    assert.equal(context.server.remoteSessions.get(context.session.remoteSessionId), null);
    context.server.handleRemoteSessionClose('target-connection', close);
    assert.equal(messages(context.targetSocket, 'remote_session_closed').at(-1).replay,
        true, 'the target party can replay its clean close tombstone');
    assert.equal(context.beginAttempts(), 1);
}

{
    const context = serverSessionContext('target-peer-close');
    context.server.handleRemoteSessionClose('target-connection', {
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        connectionGeneration: 1,
        reason: 'arbitrary-client-text',
    });
    assert.equal(context.session.teardownReason, 'peer_close');
}

// A server restart creates a fresh registry. Even if device/runtime/token
// values are replayed verbatim, no session from the previous boot exists.
{
    let clock = 80_000;
    let sequence = 0;
    const firstBoot = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `boot-one-${++sequence}`,
    });
    const opened = firstBoot.open(binding());
    firstBoot.markDisconnected('A', clock);

    const secondBoot = new RemoteSessionRegistry({
        leaseTimeoutMs: 3000,
        now: () => clock,
        idFactory: () => `boot-two-${++sequence}`,
    });
    const replay = secondBoot.resume({
        remoteSessionId: opened.session.remoteSessionId,
        endpointId: 'A', runtimeId: 'runtime-A',
        resumeToken: opened.session.resumeToken, generation: 1,
        connectionGeneration: 2,
    }, clock);
    assert.equal(replay.error, 'session_not_resumable');
    assert.equal(secondBoot.sessions.size, 0);
    assert.equal(secondBoot.incomingByTarget.size, 0);
}

function cursorContext(prefix) {
    const context = serverSessionContext(prefix);
    context.server.clients.get('target-connection').screens = [{
        id: 3, x: -3840, y: 200, width: 3840, height: 2160, primary: false,
    }];
    context.cursorMessage = (overrides = {}) => ({
        type: 'remote_session_cursor',
        protocolVersion: 7,
        serverBootId: context.server.serverBootId,
        messageId: crypto.randomUUID(),
        connectionGeneration: 1,
        remoteSessionId: context.session.remoteSessionId,
        generation: context.session.generation,
        sequence: 1,
        visible: true,
        screenId: 3,
        x: 3839,
        y: 2159,
        ...overrides,
    });
    return context;
}

// Cursor telemetry has its own v5 route and is private to an active session.
// Negative desktop origins do not affect physical, screen-local coordinates.
{
    const context = cursorContext('cursor-relay');
    const observer = addAuthenticatedClient(context.server, 'observer-connection', 'C');
    context.server.remoteSessions.open(binding('C', 'B'));
    const protocolLog = [];
    context.server.protocolLogger = event => protocolLog.push(event);
    context.server.handleMessage('target-connection', context.cursorMessage());
    const sample = messages(context.ownerSocket, 'remote_session_cursor').at(-1);
    assert.ok(sample, 'a target cursor reaches the owner through the full v5 dispatcher');
    assert.equal(sample.remoteSessionId, context.session.remoteSessionId);
    assert.equal(sample.generation, context.session.generation);
    assert.equal(sample.ownerEndpointId, 'A');
    assert.equal(sample.targetEndpointId, 'B');
    assert.equal(sample.connectionGeneration, 1);
    assert.equal(sample.ownerConnectionGeneration, 1);
    assert.equal(sample.targetConnectionGeneration, 1);
    assert.equal(sample.phase, 'Active');
    assert.equal(sample.sequence, 1);
    assert.equal(sample.visible, true);
    assert.equal(sample.screenId, 3);
    assert.equal(sample.x, 3839);
    assert.equal(sample.y, 2159);
    assert.equal(messages(observer, 'remote_session_cursor').length, 0,
        'a different owner of the same target must not receive this session sample');
    assert.equal(messages(context.targetSocket, 'remote_session_cursor').length, 0);
    assert.equal(protocolLog.length, 0, 'high-frequency cursor samples do not flood protocol logs');

    context.server.handleMessage('target-connection', context.cursorMessage({
        sequence: 2, visible: false, screenId: -1, x: 0, y: 0,
    }));
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').at(-1).visible, false);
    context.server.handleMessage('target-connection', context.cursorMessage({ type: 'cursor_update' }));
    assert.equal(messages(context.targetSocket, 'error').at(-1).code, 'removed_message_type',
        'restoring cursors must not revive the unauthenticated legacy relay');
}

// Reject foreign senders, stale generations and non-active sessions without
// accepting a sample or consuming its sequence.
{
    const context = cursorContext('cursor-authorization');
    const observer = addAuthenticatedClient(context.server, 'observer-connection', 'C');
    context.server.handleMessage('owner-connection', context.cursorMessage());
    assert.equal(messages(context.ownerSocket, 'error').at(-1).code, 'not_session_target');
    context.server.handleMessage('observer-connection', context.cursorMessage());
    assert.equal(messages(observer, 'error').at(-1).code, 'not_a_session_party');
    context.server.handleMessage('target-connection', context.cursorMessage({ generation: 2 }));
    assert.equal(messages(context.targetSocket, 'error').at(-1).code,
        'stale_remote_session_generation');
    context.server.handleMessage('target-connection', context.cursorMessage({ connectionGeneration: 2 }));
    assert.equal(messages(context.targetSocket, 'error').at(-1).code,
        'stale_connection_generation');
    for (const phase of ['Opening', 'Grace', 'CleanupPending', 'Closed']) {
        context.session.phase = phase;
        context.server.handleMessage('target-connection', context.cursorMessage());
        assert.equal(messages(context.targetSocket, 'error').at(-1).code,
            'remote_session_not_active');
    }
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 0);
    assert.equal(context.session.cursorSample, undefined);
}

// Cursor shape is strict and bounded against the target's registered screens.
// Invalid samples do not poison a later valid update with the same sequence.
{
    const context = cursorContext('cursor-validation');
    for (const invalid of [
        { sequence: 0 }, { sequence: 1.5 }, { sequence: Number.MAX_SAFE_INTEGER + 1 },
        { visible: 1 }, { visible: null }, { screenId: 0 }, { screenId: '3' },
        { screenId: -1 }, { x: -1 }, { y: -1 }, { x: 3840 }, { y: 2160 },
        { x: 1.5 }, { y: Infinity }, { x: null }, { x: '1' },
        { visible: false },
        { visible: false, screenId: -1, x: 1, y: 0 },
        { extra: true },
    ]) {
        context.server.handleMessage('target-connection', context.cursorMessage(invalid));
        assert.equal(messages(context.targetSocket, 'error').at(-1).code,
            'invalid_remote_session_cursor', JSON.stringify(invalid));
        assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 0);
    }
    const missing = context.cursorMessage();
    delete missing.visible;
    context.server.handleMessage('target-connection', missing);
    assert.equal(messages(context.targetSocket, 'error').at(-1).code,
        'invalid_remote_session_cursor');
    context.server.handleMessage('target-connection', context.cursorMessage({ x: 0, y: 0 }));
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 1);
}

// A resumed session can restart the cursor sequence, but samples from the old
// session or transport generation must never overwrite its new position.
{
    const context = cursorContext('cursor-resume');
    context.server.handleMessage('target-connection', context.cursorMessage({ sequence: 9 }));
    context.server.handleMessage('target-connection', context.cursorMessage({ sequence: 9, x: 0 }));
    context.server.handleMessage('target-connection', context.cursorMessage({ sequence: 8, x: 1 }));
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 1);
    assert.equal(messages(context.targetSocket, 'error').length, 0,
        'duplicate ephemeral samples are silently ignored');
    context.server.remoteSessions.markDisconnected('B');
    context.server.clients.get('target-connection').connectionGeneration = 2;
    context.server.handleRemoteSessionResume('target-connection', {
        remoteSessionId: context.session.remoteSessionId,
        resumeToken: context.session.resumeToken,
        generation: 1,
    });
    assert.equal(context.session.generation, 2);
    context.server.handleMessage('target-connection', context.cursorMessage({
        connectionGeneration: 2, sequence: 1, x: 20, y: 30,
    }));
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 2);
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').at(-1).x, 20);
    context.server.handleMessage('target-connection', context.cursorMessage({
        connectionGeneration: 2, generation: 1, sequence: 10,
    }));
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 2);
    assert.equal(messages(context.targetSocket, 'error').at(-1).code,
        'stale_remote_session_generation');
}

// Cursor traffic never adds to a congested owner's backlog. Fresh repeated
// samples recover stationary/hidden state once the connection has drained.
{
    const context = cursorContext('cursor-backpressure');
    context.ownerSocket.bufferedAmount = 64 * 1024 + 1;
    context.server.handleMessage('target-connection', context.cursorMessage());
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 0);
    context.ownerSocket.bufferedAmount = 0;
    context.server.handleMessage('target-connection', context.cursorMessage());
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 0,
        'a stale queued sample cannot reappear after backpressure');
    context.server.handleMessage('target-connection', context.cursorMessage({ sequence: 2 }));
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 1);
    context.ownerSocket.bufferedAmount = 64 * 1024 + 1;
    context.server.handleMessage('target-connection', context.cursorMessage({
        sequence: 3, visible: false, screenId: -1, x: 0, y: 0,
    }));
    context.ownerSocket.bufferedAmount = 64 * 1024;
    context.server.handleMessage('target-connection', context.cursorMessage({
        sequence: 4, visible: false, screenId: -1, x: 0, y: 0,
    }));
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').length, 2);
    assert.equal(messages(context.ownerSocket, 'remote_session_cursor').at(-1).visible, false);
}

// Full display state is coalesced under congestion and always precedes cursor
// coordinates which refer to the replacement topology.
{
    const context = cursorContext('snapshot-backpressure');
    const { server, session, ownerSocket, targetSocket } = context;
    const observer = addAuthenticatedClient(server, 'snapshot-observer', 'observer');
    const snapshotMessage = (sequence, screens) => ({
        ...context.cursorMessage(),
        type: 'remote_session_snapshot',
        snapshotSequence: sequence,
        snapshot: { screens, systemUI: [], volumePercent: 42,
            revision: sequence, capturedAtEpochMs: 1234 },
    });
    ownerSocket.bufferedAmount = 64 * 1024 + 1;
    server.handleRemoteSessionSnapshot('target-connection', snapshotMessage(2, []));
    const replacement = [{ id: 3, x: 0, y: 0, width: 1920, height: 1080, primary: true }];
    server.handleRemoteSessionSnapshot('target-connection', snapshotMessage(3, replacement));
    assert.equal(messages(ownerSocket, 'remote_session_snapshot').length, 0);
    assert.equal(session.pendingTargetSnapshot.snapshotSequence, 3);
    server.handleRemoteSessionCursor('target-connection', context.cursorMessage({ x: 10, y: 20 }));
    assert.equal(messages(ownerSocket, 'remote_session_cursor').length, 0);
    ownerSocket.bufferedAmount = 0;
    server.handleRemoteSessionCursor('target-connection', context.cursorMessage({ sequence: 2, x: 10, y: 20 }));
    assert.equal(session.pendingTargetSnapshot, undefined);
    assert.deepEqual(ownerSocket.messages.slice(-2).map(m => m.type),
        ['remote_session_snapshot', 'remote_session_cursor']);
    assert.deepEqual(messages(ownerSocket, 'remote_session_snapshot').at(-1).snapshot.screens, replacement);
    assert.equal(messages(observer, 'remote_session_snapshot').length, 0);
    // Bounds follow the session snapshot even before discovery catches up.
    server.handleRemoteSessionCursor('target-connection', context.cursorMessage({ sequence: 3 }));
    assert.equal(targetSocket.messages.at(-1).code, 'invalid_remote_session_cursor');
    server.handleRemoteSessionSnapshot('target-connection', snapshotMessage(4, []));
    assert.deepEqual(messages(ownerSocket, 'remote_session_snapshot').at(-1).snapshot.screens, []);
    server.handleRemoteSessionSnapshot('target-connection', snapshotMessage(3, replacement));
    assert.equal(targetSocket.messages.at(-1).code, 'invalid_remote_session_snapshot');
    ownerSocket.bufferedAmount = 64 * 1024 + 1;
    server.handleRemoteSessionSnapshot('target-connection', snapshotMessage(5, replacement));
    session.phase = 'Terminating';
    ownerSocket.bufferedAmount = 0;
    server.flushRemoteSessionSnapshot(session);
    assert.equal(session.pendingTargetSnapshot, undefined);
    assert.equal(messages(ownerSocket, 'remote_session_snapshot').length, 2);
}

console.log('remote session protocol tests passed');
