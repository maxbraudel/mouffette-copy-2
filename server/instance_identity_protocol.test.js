'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');
const {
    PROTOCOL_VERSION, MAX_INSTANCE_ORDINAL, instanceIdForOrdinal, challengePayload,
    createChallenge, installationIdForPublicKey, endpointIdForInstallation,
    verifyAuthResponse,
} = require('./device_auth');

function socket() {
    return {
        readyState: WebSocket.OPEN, messages: [], closes: [], bufferedAmount: 0,
        send(encoded) { this.messages.push(JSON.parse(encoded)); },
        close(code, reason) {
            this.closes.push({ code, reason });
            this.readyState = WebSocket.CLOSED;
        },
    };
}
function context() {
    let now = 1000;
    const server = new MouffetteServer({ port: 0, protocolLogger: null,
        metricLogger: () => {}, monotonicNow: () => now, epochNow: () => now });
    return { server, advance(ms) { now += ms; }, now: () => now };
}
function candidate(ctx, keys, ordinal = 1, runtimeId = crypto.randomUUID()) {
    const { server } = ctx;
    const id = crypto.randomUUID();
    const challenge = createChallenge(server.serverBootId, ctx.now());
    const publicKeyDer = keys.publicKey.export({ type: 'spki', format: 'der' });
    const instanceId = instanceIdForOrdinal(ordinal);
    const client = { id, ws: socket(), authenticated: false, authChallenge: challenge,
        machineName: null, screens: [], systemUI: [] };
    server.clients.set(id, client);
    const response = {
        type: 'auth_response', protocolVersion: PROTOCOL_VERSION,
        messageId: crypto.randomUUID(), serverBootId: server.serverBootId,
        runtimeId, instanceId, instanceOrdinal: ordinal,
        installationId: installationIdForPublicKey(publicKeyDer),
        publicKey: publicKeyDer.toString('base64url'),
        signature: crypto.sign(null, challengePayload({ ...challenge,
            runtimeId, instanceId, instanceOrdinal: ordinal }), keys.privateKey)
            .toString('base64url'),
    };
    return { client, response, challenge };
}
function authenticate(ctx, value) {
    ctx.server.handleAuthResponse(value.client.id, value.response,
        { epochMs: ctx.now(), monotonicMs: ctx.now() });
    return value.client;
}
function latest(client, type) {
    return client.ws.messages.filter(message => message.type === type).at(-1);
}
function register(ctx, client, ordinal = client.instanceOrdinal) {
    ctx.server.handleEndpointSnapshot(client.id, {
        instanceOrdinal: ordinal, machineName: `Instance ${ordinal}`,
        platform: 'test', screens: [], systemUI: [], volumePercent: null,
    });
}
function envelope(ctx, client, type, values = {}) {
    return { type, protocolVersion: PROTOCOL_VERSION,
        serverBootId: ctx.server.serverBootId, messageId: crypto.randomUUID(),
        connectionGeneration: client.connectionGeneration, ...values };
}

// A fixed signature domain and canonical decimal ordinal prevent aliases.
{
    const ctx = context();
    const keys = crypto.generateKeyPairSync('ed25519');
    const value = candidate(ctx, keys, 2);
    const { challenge, response } = value;
    assert.equal(challengePayload({ ...challenge, ...response }).toString(),
        `mouffette-v8\n${challenge.serverBootId}\n${challenge.nonce}\n${response.runtimeId}\ninstance-2\n2`);
    const verify = response => verifyAuthResponse(challenge, response, ctx.now(), 10000);
    assert.equal(verify(response).ok, true);
    assert.equal(verify({ ...response, protocolVersion: 7 }).error, 'protocol_version_mismatch');
    for (const instanceOrdinal of [undefined, null, false, '2', 0, -1, 1.5,
        2147483648, Number.MAX_SAFE_INTEGER, Infinity, NaN]) {
        assert.equal(verify({ ...response, instanceOrdinal }).error, 'invalid_instance_ordinal');
    }
    for (const instanceId of ['primary', 'instance-02', 'instance-+2', 'Instance-2',
        'instance-2\n2', crypto.randomUUID()]) {
        assert.equal(verify({ ...response, instanceId }).error, 'invalid_instance_id');
    }
    assert.equal(verify({ ...response, instanceOrdinal: 3, instanceId: 'instance-3' }).error,
        'invalid_identity_signature', 'changing both canonical identity fields still requires a signature');
    const maximum = candidate(ctx, keys, MAX_INSTANCE_ORDINAL);
    assert.equal(verifyAuthResponse(maximum.challenge, maximum.response, ctx.now(), 10000).ok, true);
    const primary = candidate(ctx, keys);
    const expected = crypto.createHash('sha256').update(
        `mouffette-endpoint-v1\n${primary.response.installationId}\nprimary`).digest('base64url');
    assert.equal(endpointIdForInstallation(primary.response.installationId, 'primary'), expected,
        'instance #1 retains its pre-v7 endpoint hash');
}

// Every ordinal is a full endpoint. Registration cannot change signed identity,
// and retained offline discovery exposes the exact same identity tuple.
{
    const ctx = context();
    const keys = crypto.generateKeyPairSync('ed25519');
    const first = authenticate(ctx, candidate(ctx, keys, 1));
    const second = authenticate(ctx, candidate(ctx, keys, 2));
    assert.equal(first.instanceId, 'primary');
    assert.equal(second.instanceId, 'instance-2');
    assert.equal(first.installationId, second.installationId);
    assert.notEqual(first.endpointId, second.endpointId);
    assert.equal(latest(second, 'welcome').instanceOrdinal, 2);
    assert.equal(first.connectionGeneration, 1);
    assert.equal(second.connectionGeneration, 2);
    register(ctx, first); register(ctx, second);
    const identityKeys = ['installationId', 'endpointId', 'instanceId', 'instanceOrdinal', 'runtimeId'];
    let advertised = ctx.server.presenceEntries().find(entry => entry.endpointId === second.endpointId);
    for (const key of identityKeys) assert.equal(advertised[key], second[key], key);
    const revision = ctx.server.presenceRevision;
    register(ctx, second, 3);
    assert.equal(latest(second, 'error').code, 'invalid_endpoint_snapshot');
    assert.equal(second.instanceOrdinal, 2);
    assert.equal(second.machineName, 'Instance 2');
    const duplicate = candidate(ctx, keys, 2);
    authenticate(ctx, duplicate);
    assert.equal(latest(duplicate.client, 'error').code, 'endpoint_already_connected');
    assert.equal(ctx.server.currentTransportByEndpoint.get(second.endpointId), second);
    ctx.server.retireClientTransport(second, true, 1000, 'test departure');
    advertised = ctx.server.presenceEntries().find(entry => entry.endpointId === second.endpointId);
    assert.equal(advertised.status, 'Disconnected');
    assert.equal(advertised.canAcceptSession, false);
    for (const key of identityKeys) assert.equal(advertised[key], second[key], `offline ${key}`);
    assert.ok(ctx.server.presenceRevision > revision);
    const reopened = authenticate(ctx, candidate(ctx, keys, 2));
    assert.equal(reopened.endpointId, second.endpointId);
    assert.notEqual(reopened.runtimeId, second.runtimeId);
    assert.equal(reopened.connectionGeneration, 3);
    register(ctx, reopened);
    advertised = ctx.server.presenceEntries().find(entry => entry.endpointId === second.endpointId);
    assert.equal(advertised.runtimeId, reopened.runtimeId);
    assert.equal(ctx.server.currentTransportByEndpoint.size, 2);
}

// Same-installation endpoints use the normal admission path in both roles;
// another incoming session does not make the target unavailable.
{
    const ctx = context();
    const keys = crypto.generateKeyPairSync('ed25519');
    const peers = [1, 2, 3].map(ordinal => authenticate(ctx, candidate(ctx, keys, ordinal)));
    for (const peer of peers) register(ctx, peer);
    for (const owner of [peers[0], peers[2]]) {
        ctx.server.handleMessage(owner.id, envelope(ctx, owner, 'remote_session_open', {
            targetEndpointId: peers[1].endpointId, requestId: crypto.randomUUID(),
        }));
        assert.ok(latest(owner, 'remote_session_opening'));
        assert.equal(latest(owner, 'error'), undefined);
    }
    assert.equal(ctx.server.remoteSessions.incomingByTarget.get(peers[1].endpointId).size, 2);
    ctx.server.handleMessage(peers[1].id, envelope(ctx, peers[1], 'remote_session_open', {
        targetEndpointId: peers[0].endpointId, requestId: crypto.randomUUID(),
    }));
    assert.ok(latest(peers[1], 'remote_session_opening'));
    assert.equal(ctx.server.remoteSessions.sessions.size, 3);
    assert.ok(ctx.server.presenceEntries().every(entry => entry.canAcceptSession));
}

// Rebinding one endpoint after other endpoints creates generation gaps. Old
// control/upload sockets, one-use upload tokens and close envelopes stay fenced.
{
    const ctx = context();
    const keys = crypto.generateKeyPairSync('ed25519');
    const old = authenticate(ctx, candidate(ctx, keys, 1));
    const peer = authenticate(ctx, candidate(ctx, keys, 2));
    authenticate(ctx, candidate(ctx, keys, 3));
    register(ctx, old); register(ctx, peer);
    const session = ctx.server.remoteSessions.open({
        ownerEndpointId: old.endpointId, targetEndpointId: peer.endpointId,
        ownerRuntimeId: old.runtimeId, targetRuntimeId: peer.runtimeId,
        ownerConnectionGeneration: old.connectionGeneration,
        targetConnectionGeneration: peer.connectionGeneration,
    }).session;
    assert.equal(ctx.server.remoteSessions.accept({
        remoteSessionId: session.remoteSessionId, targetEndpointId: peer.endpointId,
        targetRuntimeId: peer.runtimeId, connectionGeneration: peer.connectionGeneration,
        generation: session.generation,
    }).ok, true);
    const token = ctx.server.issueUploadChannelToken(old.id);
    const uploadSocket = socket();
    ctx.server.registerUploadSocket(old, uploadSocket);
    const replacement = authenticate(ctx, candidate(ctx, keys, 1, old.runtimeId));
    assert.equal(replacement.connectionGeneration, 4);
    assert.equal(ctx.server.resolveClientId(old.endpointId), replacement.id);
    assert.equal(ctx.server.clients.has(old.id), false);
    assert.equal(ctx.server.consumeUploadChannelToken(token), null);
    assert.equal(uploadSocket.readyState, WebSocket.CLOSED);
    assert.equal(ctx.server.handleControlSocketMessage(old, old.ws,
        envelope(ctx, replacement, 'endpoint_disable')), false);
    const staleUploadSocket = socket();
    ctx.server.handleUploadChannelMessage(old, staleUploadSocket,
        envelope(ctx, replacement, 'upload_start'));
    assert.equal(staleUploadSocket.readyState, WebSocket.CLOSED);
    assert.equal(replacement.draining, undefined);
    ctx.server.forgetCurrentTransport(old);
    assert.equal(ctx.server.currentTransportByEndpoint.get(old.endpointId), replacement,
        'late close/error from a retired socket cannot remove the current binding');
    ctx.server.handleRemoteSessionClose(replacement.id, {
        remoteSessionId: session.remoteSessionId, generation: session.generation,
        connectionGeneration: old.connectionGeneration, requestId: 'stale-close',
    });
    assert.equal(latest(replacement, 'error').code, 'stale_connection_generation');
    assert.equal(session.phase, 'Grace');
    assert.equal(ctx.server.currentTransportByEndpoint.size, 3);
}

// A free ordinal reuses the endpoint, never a previous runtime's sessions.
// This applies even when the old transport has already left the live index.
{
    const ctx = context();
    const keys = crypto.generateKeyPairSync('ed25519');
    const owner = authenticate(ctx, candidate(ctx, keys, 1));
    const oldTarget = authenticate(ctx, candidate(ctx, keys, 2));
    register(ctx, owner); register(ctx, oldTarget);
    const session = ctx.server.remoteSessions.open({
        ownerEndpointId: owner.endpointId, targetEndpointId: oldTarget.endpointId,
        ownerRuntimeId: owner.runtimeId, targetRuntimeId: oldTarget.runtimeId,
        ownerConnectionGeneration: owner.connectionGeneration,
        targetConnectionGeneration: oldTarget.connectionGeneration,
    }).session;
    assert.equal(ctx.server.remoteSessions.accept({
        remoteSessionId: session.remoteSessionId, targetEndpointId: oldTarget.endpointId,
        targetRuntimeId: oldTarget.runtimeId, connectionGeneration: oldTarget.connectionGeneration,
        generation: session.generation,
    }).ok, true);
    const collision = candidate(ctx, keys, 2);
    authenticate(ctx, collision);
    assert.equal(latest(collision.client, 'error').code, 'endpoint_already_connected');
    assert.equal(session.phase, 'Active');
    ctx.server.handleRemoteSessionDeparture(oldTarget, ctx.now());
    ctx.server.retireClientTransport(oldTarget, true, 1000, 'process exited');
    assert.equal(session.phase, 'Grace');
    const restarted = authenticate(ctx, candidate(ctx, keys, 2));
    assert.equal(restarted.endpointId, oldTarget.endpointId);
    assert.notEqual(restarted.runtimeId, oldTarget.runtimeId);
    assert.equal(session.phase, 'CleanupPending');
    assert.equal(session.teardownReason, 'runtime_restarted');
    assert.equal(session.targetRuntimeId, oldTarget.runtimeId);
    assert.equal(session.targetConnectionGeneration, oldTarget.connectionGeneration);
    assert.equal(ctx.server.currentTransportByEndpoint.get(owner.endpointId), owner);
}

// No historical endpoint counters survive churn. Counter exhaustion preserves
// the live transport, and only a fresh boot may reset the generation namespace.
{
    const ctx = context();
    const keys = crypto.generateKeyPairSync('ed25519');
    for (let ordinal = 1; ordinal <= 200; ++ordinal) {
        const client = authenticate(ctx, candidate(ctx, keys, ordinal));
        assert.equal(client.connectionGeneration, ordinal);
        ctx.server.retireClientTransport(client, true, 1000, 'bounded churn');
        assert.equal(ctx.server.currentTransportByEndpoint.size, 0);
        assert.equal(ctx.server.clients.size, 0);
    }
    assert.equal(Object.hasOwn(ctx.server, 'connectionGenerationByEndpoint'), false);
    const live = authenticate(ctx, candidate(ctx, keys));
    ctx.server.connectionGenerationSequence = Number.MAX_SAFE_INTEGER;
    const overflow = candidate(ctx, keys, 1, live.runtimeId);
    authenticate(ctx, overflow);
    assert.equal(latest(overflow.client, 'error').code, 'connection_generation_exhausted');
    assert.equal(ctx.server.currentTransportByEndpoint.get(live.endpointId), live);
    assert.equal(live.ws.readyState, WebSocket.OPEN);
    const reboot = context();
    const afterBoot = authenticate(reboot, candidate(reboot, keys));
    assert.equal(afterBoot.connectionGeneration, 1);
    assert.notEqual(reboot.server.serverBootId, ctx.server.serverBootId);
}

console.log('instance identity protocol v8 tests passed');
