'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { EventEmitter } = require('node:events');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');
const { PROTOCOL_VERSION, challengePayload, installationIdForPublicKey } = require('./device_auth');

// Exercise the actual registered WebSocket callbacks with a controlled event
// order. A failed socket's close event can arrive after another socket resumed.
(async () => {
    let monotonic = 1000;
    const server = new MouffetteServer({ port: 0, host: '127.0.0.1',
        monotonicNow: () => monotonic, protocolLogger: null, metricLogger: () => {} });
    server.start();
    await new Promise(resolve => server.wss.once('listening', resolve));
    const keys = crypto.generateKeyPairSync('ed25519');
    const runtimeId = crypto.randomUUID();
    const makeEnvelope = (client, type, body = {}) => ({
        type, protocolVersion: PROTOCOL_VERSION, serverBootId: server.serverBootId,
        connectionGeneration: client.connectionGeneration,
        messageId: crypto.randomUUID(), ...body,
    });
    function attach(keyPair, runtime, machineName) {
        const ws = Object.assign(new EventEmitter(), {
            readyState: WebSocket.OPEN, bufferedAmount: 0, messages: [],
            send(data) { this.messages.push(JSON.parse(data)); },
            close() { this.readyState = WebSocket.CLOSED; },
        });
        server.wss.emit('connection', ws, { url: '/' });
        const client = ws.mouffetteClient;
        const challenge = ws.messages.find(message => message.type === 'auth_challenge');
        const publicKeyDer = keyPair.publicKey.export({ type: 'spki', format: 'der' });
        server.handleControlSocketMessage(client, ws, {
            type: 'auth_response', protocolVersion: PROTOCOL_VERSION,
            messageId: crypto.randomUUID(), serverBootId: server.serverBootId,
            runtimeId: runtime, instanceId: 'primary', instanceOrdinal: 1,
            installationId: installationIdForPublicKey(publicKeyDer),
            publicKey: publicKeyDer.toString('base64url'),
            signature: crypto.sign(null, challengePayload({ ...challenge,
                runtimeId: runtime, instanceId: 'primary', instanceOrdinal: 1 }),
            keyPair.privateKey).toString('base64url'),
        });
        assert.equal(client.authenticated, true);
        server.handleControlSocketMessage(client, ws, makeEnvelope(client, 'endpoint_snapshot', {
            machineName, platform: 'test', instanceOrdinal: 1,
            screens: [], systemUI: [], volumePercent: null,
        }));
        return client;
    }
    try {
        const old = attach(keys, runtimeId, 'Old metadata');
        const target = attach(crypto.generateKeyPairSync('ed25519'), crypto.randomUUID(), 'Target');
        server.handleControlSocketMessage(old, old.ws, makeEnvelope(old, 'remote_session_open', {
            targetEndpointId: target.endpointId, requestId: crypto.randomUUID(),
        }));
        const session = server.remoteSessions.sessionsForEndpoint(old.endpointId)[0];
        assert.ok(session);
        server.handleControlSocketMessage(target, target.ws, makeEnvelope(target, 'remote_session_accept', {
            remoteSessionId: session.remoteSessionId, generation: session.generation,
            snapshot: { revision: 1, capturedAtEpochMs: Date.now(),
                screens: [], systemUI: [], volumePercent: null },
        }));
        assert.equal(session.phase, 'Active');

        monotonic += 100;
        old.ws.readyState = WebSocket.CLOSING;
        old.ws.emit('error', new Error('controlled transport failure before delayed close'));
        assert.equal(session.phase, 'Grace');
        assert.equal(server.clients.has(old.id), false);
        assert.equal(server.currentTransportByEndpoint.has(old.endpointId), false);

        const replacement = attach(keys, runtimeId, 'Current metadata');
        server.handleControlSocketMessage(replacement, replacement.ws,
            makeEnvelope(replacement, 'remote_session_resume', {
                remoteSessionId: session.remoteSessionId, generation: session.generation,
                resumeToken: session.resumeToken, requestId: crypto.randomUUID(),
            }));
        assert.equal(session.phase, 'Active');
        assert.equal(session.ownerConnectionGeneration, replacement.connectionGeneration);
        const before = JSON.stringify({ presence: server.endpointPresence.get(old.endpointId),
            stateRevision: session.stateRevision, generation: session.generation,
            graceEndpoints: [...session.graceEndpoints] });
        monotonic += 100;
        old.ws.readyState = WebSocket.CLOSED;
        old.ws.emit('close');
        old.ws.emit('error', new Error('late duplicate error'));
        old.ws.emit('close');
        assert.equal(session.phase, 'Active');
        assert.equal(server.currentTransportByEndpoint.get(old.endpointId), replacement);
        assert.equal(server.clients.get(replacement.id), replacement);
        assert.equal(server.resolveClientId(old.endpointId), replacement.id);
        assert.equal(JSON.stringify({ presence: server.endpointPresence.get(old.endpointId),
            stateRevision: session.stateRevision, generation: session.generation,
            graceEndpoints: [...session.graceEndpoints] }), before,
        'obsolete socket callbacks must not mutate resumed sessions or current presence');
    } finally {
        for (const client of server.clients.values()) clearTimeout(client.authTimer);
        clearInterval(server.uploadCleanupInterval);
        clearInterval(server.leaseSweepInterval);
        await new Promise(resolve => server.wss.close(resolve));
    }
    console.log('transport retirement callback tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
