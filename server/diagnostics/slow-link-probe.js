'use strict';

// Diagnostic, not a regression test: measure control-message starvation on the
// production relay with real WebSockets and a byte-rate-limited TCP downlink.
// Run: node server/diagnostics/slow-link-probe.js [KiB/s, default 256] [downlink|uplink-fallback]
// The synthetic recipient keeps heartbeating and deliberately does not run the
// Qt expiry watchdog, so we can observe messages arriving AFTER its deadlines.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const net = require('node:net');
const { performance } = require('node:perf_hooks');
const WebSocket = require('ws');
const { MouffetteServer } = require('../server');
const { challengePayload, installationIdForPublicKey, endpointIdForInstallation } = require('../device_auth');

const rateKiB = Number(process.argv[2] || 256);
assert(Number.isFinite(rateKiB) && rateKiB >= 32 && rateKiB <= 4096);
const mode = process.argv[3] || 'downlink';
assert(['downlink', 'uplink-fallback'].includes(mode));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function trackedSocket(url) {
    const ws = new WebSocket(url);
    const inbox = [];
    const observations = [];
    let observer = () => {};
    ws.on('message', raw => {
        const message = JSON.parse(raw.toString());
        observations.push({ at: performance.now(), message });
        observer(message);
        inbox.push(message);
    });
    ws.on('error', error => { observations.push({ at: performance.now(), error: error.message }); });
    ws.on('close', (code, reason) => { observations.push({ at: performance.now(), closeCode: code, closeReason: reason.toString() }); });
    return {
        ws, observations,
        observe(callback) { observer = callback; },
        async next(predicate, timeoutMs = 5000) {
            const end = performance.now() + timeoutMs;
            while (performance.now() < end) {
                const index = inbox.findIndex(predicate);
                if (index >= 0) return inbox.splice(index, 1)[0];
                await sleep(10);
            }
            throw new Error('Protocol message timeout');
        },
    };
}

async function connectDevice(url, name) {
    const peer = trackedSocket(url);
    const challenge = await peer.next(message => message.type === 'auth_challenge');
    const keys = crypto.generateKeyPairSync('ed25519');
    const publicKey = keys.publicKey.export({ type: 'spki', format: 'der' });
    const identity = { runtimeId: crypto.randomUUID(), instanceId: 'primary', instanceOrdinal: 1 };
    const installationId = installationIdForPublicKey(publicKey);
    peer.endpointId = endpointIdForInstallation(installationId, identity.instanceId);
    peer.ws.send(JSON.stringify({ type: 'auth_response', protocolVersion: challenge.protocolVersion,
        serverBootId: challenge.serverBootId, messageId: crypto.randomUUID(), ...identity, installationId,
        publicKey: publicKey.toString('base64url'),
        signature: crypto.sign(null, challengePayload({ ...challenge, ...identity }), keys.privateKey).toString('base64url'),
    }));
    const welcome = await peer.next(message => message.type === 'welcome');
    peer.envelope = body => ({ protocolVersion: welcome.protocolVersion,
        serverBootId: welcome.serverBootId, connectionGeneration: welcome.connectionGeneration,
        messageId: crypto.randomUUID(), ...body });
    peer.send = (type, body = {}) => peer.ws.send(JSON.stringify(peer.envelope({ type, ...body })));
    peer.send('endpoint_snapshot', { machineName: name, platform: 'diagnostic', instanceOrdinal: 1,
        screens: [], systemUI: [], volumePercent: null });
    await peer.next(message => message.type === 'endpoint_snapshot_applied');
    let sequence = 0;
    peer.heartbeat = setInterval(() => {
        if (peer.ws.readyState === WebSocket.OPEN)
            peer.send('heartbeat', { sequence: ++sequence, clientMonotonicMs: Math.floor(performance.now()) });
    }, welcome.policy.heartbeatIntervalMs);
    return peer;
}

async function main() {
    const server = new MouffetteServer({ port: 0, host: '127.0.0.1', protocolLogger: () => {}, metricLogger: () => {} });
    const peers = [];
    const sockets = new Set();
    let limited = false;
    let deliveredWireBytes = 0;
    const queues = new Map();
    const proxy = net.createServer(downstream => {
        const upstream = net.connect(server.wss.address().port, '127.0.0.1');
        sockets.add(downstream); sockets.add(upstream);
        const queue = [];
        const destination = mode === 'downlink' ? downstream : upstream;
        const source = mode === 'downlink' ? upstream : downstream;
        queues.set(destination, queue);
        destination.pipe(source); // The reverse direction stays unimpaired.
        source.on('data', data => {
            if (!limited) destination.write(data);
            else queue.push(data);
        });
        for (const socket of [upstream, downstream]) {
            socket.on('error', () => {});
            socket.on('close', () => { upstream.destroy(); downstream.destroy(); queues.delete(destination); });
        }
    });
    const drain = setInterval(() => {
        if (!limited) return;
        for (const [socket, queue] of queues) {
            let budget = Math.floor(rateKiB * 1024 / 50);
            while (budget > 0 && queue.length && !socket.destroyed) {
                const count = Math.min(budget, queue[0].length);
                socket.write(queue[0].subarray(0, count));
                deliveredWireBytes += count;
                budget -= count;
                if (count === queue[0].length) queue.shift();
                else queue[0] = queue[0].subarray(count);
            }
        }
    }, 20);
    let upload;
    try {
        server.start();
        await new Promise(resolve => server.wss.once('listening', resolve));
        await new Promise(resolve => proxy.listen(0, '127.0.0.1', resolve));
        const directUrl = `ws://127.0.0.1:${server.wss.address().port}`;
        const proxyUrl = `ws://127.0.0.1:${proxy.address().port}`;
        const owner = await connectDevice(mode === 'downlink' ? directUrl : proxyUrl, 'audit-owner'); peers.push(owner);
        const target = await connectDevice(mode === 'downlink' ? proxyUrl : directUrl, 'audit-target'); peers.push(target);
        owner.send('remote_session_open', { targetEndpointId: target.endpointId, requestId: crypto.randomUUID() });
        const offer = await target.next(message => message.type === 'remote_session_offer');
        const session = { remoteSessionId: offer.remoteSessionId, generation: offer.generation };
        target.send('remote_session_accept', { ...session,
            snapshot: { screens: [], systemUI: [], volumePercent: null, revision: 1, capturedAtEpochMs: Date.now() } });
        const opened = await owner.next(message => message.type === 'remote_session_opened');
        await target.next(message => message.type === 'remote_session_opened');
        for (const peer of peers) peer.send('remote_session_state_ack', { ...session, stateRevision: opened.stateRevision });
        await owner.next(message => message.type === 'remote_session_lease_state' && message.commandReady);
        await target.next(message => message.type === 'remote_session_lease_state' && message.commandReady);
        if (mode === 'downlink') {
            owner.send('request_upload_channel');
            const token = await owner.next(message => message.type === 'upload_channel_token');
            upload = trackedSocket(`${directUrl}?channel=upload&token=${encodeURIComponent(token.token)}`);
            await upload.next(message => message.type === 'upload_channel_ready');
        } else {
            upload = owner; // Production's permitted control-socket fallback.
        }
        const size = 1024 * 1024;
        const bytes = Buffer.alloc(size, 0x4d);
        const sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
        const uploadId = crypto.randomUUID();
        const assetId = crypto.randomUUID();
        const sendUpload = body => upload.ws.send(JSON.stringify(owner.envelope({ ...session, uploadId, ...body })));
        sendUpload({ type: 'upload_start', files: [{ assetId, fileId: sha256, sha256,
            name: 'diagnostic.png', extension: 'png', size, mediaIds: [crypto.randomUUID()] }] });
        await target.next(message => message.type === 'upload_start');
        target.send('upload_ready', { ...session, uploadId, assets: [{ assetId, offset: 0, size, sha256 }] });
        await owner.next(message => message.type === 'upload_ready');
        // Start just after a proof-bearing ACK, the most generous alignment.
        const limitedPeer = mode === 'downlink' ? target : owner;
        await limitedPeer.next(message => message.type === 'heartbeat_ack' && message.sessionStates?.length > 0);
        const start = performance.now();
        let receivedBytes = 0;
        target.observe(message => {
            if (message.type !== 'upload_chunk') return;
            receivedBytes += message.size;
            // A synthetic durable receiver: no disk/decoding delay is needed
            // to exhibit the starvation. Never complete/promote this dummy PNG.
            target.send('upload_progress', { ...session, uploadId,
                assets: [{ assetId, offset: receivedBytes, size, sha256 }] });
        });
        limited = true;
        for (let offset = 0; offset < size; offset += 128 * 1024) {
            sendUpload({ type: 'upload_chunk', assetId, offset, size: 128 * 1024,
                sha256, data: bytes.subarray(offset, offset + 128 * 1024).toString('base64') });
        }
        if (mode === 'uplink-fallback') {
            const end = performance.now() + 10000;
            while (owner.ws.readyState !== WebSocket.CLOSED && performance.now() < end) await sleep(20);
            const closed = owner.observations.find(event => event.at >= start && event.closeCode);
            const stored = server.remoteSessions.get(session.remoteSessionId);
            const result = { mode, uplinkKiBPerSecond: rateKiB, protocolVersion: server.protocolVersion,
                dedicatedSenderSocket: false, receivedPayloadBytes: receivedBytes, intendedPayloadBytes: size,
                deliveredWireBytes, ownerControlClosed: owner.ws.readyState === WebSocket.CLOSED,
                closedAfterMs: closed ? Math.round(closed.at - start) : null,
                closeCode: closed?.closeCode, closeReason: closed?.closeReason,
                transportAbortThresholdMs: server.config.leaseTimeoutMs,
                serverSessionPhase: stored?.phase,
                note: 'Real production server closes the fallback sender despite continuing upload bytes; no synthetic watchdog.' };
            assert.equal(result.ownerControlClosed, true);
            assert(receivedBytes < size);
            console.log(`AUDIT_RESULT ${JSON.stringify(result)}`);
            return;
        }
        const ack = await target.next(message => message.type === 'heartbeat_ack', 60000);
        const after = target.observations.filter(event => event.at >= start && event.message);
        const chunks = after.filter(event => event.message.type === 'upload_chunk');
        const ackEvent = after.find(event => event.message === ack);
        const times = [start, ...after.map(event => event.at)];
        const maxMessageGap = Math.max(...times.slice(1).map((at, index) => at - times[index]));
        const heartbeatDelay = ackEvent.at - start;
        const proofBudget = server.config.leaseTimeoutMs + server.config.sessionRecoveryTimeoutMs;
        const stored = server.remoteSessions.get(session.remoteSessionId);
        const result = {
            mode, downlinkKiBPerSecond: rateKiB, protocolVersion: server.protocolVersion,
            uploadUsedDedicatedSenderSocket: true,
            uploadChunksReceivedOnTargetControlSocket: chunks.length, receivedPayloadBytes: receivedBytes,
            deliveredWireBytes, firstChunkAfterMs: Math.round(chunks[0]?.at - start),
            maxCompleteMessageGapMs: Math.round(maxMessageGap), nextHeartbeatAckAfterMs: Math.round(heartbeatDelay),
            transportAbortThresholdMs: server.config.leaseTimeoutMs, maximumSessionProofBudgetMs: proofBudget,
            exceedsQtTransportAbortThreshold: maxMessageGap >= server.config.leaseTimeoutMs,
            exceedsQtSessionProofBudget: heartbeatDelay >= proofBudget,
            relayStillConsidersSessionCommandReady: server.remoteSessions.commandReady(stored),
            note: 'Real relay and TCP shaping; Qt watchdog outcomes inferred from measured deadlines. No Qt client or disk validation in this probe.',
        };
        assert.equal(receivedBytes, size);
        assert.equal(result.relayStillConsidersSessionCommandReady, true);
        console.log(`AUDIT_RESULT ${JSON.stringify(result)}`);
    } finally {
        clearInterval(drain);
        for (const peer of peers) { clearInterval(peer.heartbeat); peer.ws.terminate(); }
        upload?.ws.terminate();
        for (const socket of sockets) socket.destroy();
        await new Promise(resolve => proxy.close(resolve));
        clearInterval(server.uploadCleanupInterval);
        clearInterval(server.leaseSweepInterval);
        for (const client of server.wss.clients) client.terminate();
        await new Promise(resolve => server.wss.close(resolve));
    }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
