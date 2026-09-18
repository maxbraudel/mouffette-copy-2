'use strict';

// Diagnostic, not a regression test: measure control-message starvation on the
// production relay with real WebSockets and a byte-rate-limited TCP downlink.
// Run: node server/diagnostics/slow-link-probe.js [KiB/s, default 256] [downlink|uplink] [file count: 1|32|256]
// Both directions use dedicated data sockets; control heartbeat latency and
// the final per-file SHA-256 are verified. Receiver durability is synthetic,
// so this probe complements the Qt disk checkpoint tests.
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
assert(['downlink', 'uplink'].includes(mode));
const fileCount = Number(process.argv[4] || 1);
assert([1, 32, 256].includes(fileCount));
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
    let upload, targetData;
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
        for (const peer of [owner, target]) {
            peer.send('request_upload_channel');
            const token = await peer.next(message => message.type === 'upload_channel_token');
            const peerUrl = (mode === 'downlink' ? peer === target : peer === owner) ? proxyUrl : directUrl;
            peer.data = trackedSocket(`${peerUrl}?channel=upload&token=${encodeURIComponent(token.token)}`);
            await peer.data.next(message => message.type === 'upload_channel_ready');
        }
        upload = owner.data; targetData = target.data;
        const totalSize = 1024 * 1024;
        const fileSize = totalSize / fileCount;
        const sourceFiles = Array.from({ length: fileCount }, (_, index) => {
            const bytes = Buffer.alloc(fileSize, index % 256);
            const sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
            return { bytes, assetId: crypto.randomUUID(), fileId: sha256, sha256,
                name: 'diagnostic.png', extension: 'png', size: fileSize,
                mediaIds: [crypto.randomUUID()], sent: 0, confirmed: 0, received: 0,
                destination: Buffer.alloc(fileSize) };
        });
        const uploadId = crypto.randomUUID();
        const sendUpload = body => upload.ws.send(JSON.stringify(owner.envelope({ ...session, uploadId, ...body })));
        const sendTarget = body => targetData.ws.send(JSON.stringify(target.envelope({ ...session, uploadId, ...body })));
        const inventory = () => sourceFiles.map(file => ({ assetId: file.assetId,
            offset: file.received, size: file.size, sha256: file.sha256 }));
        sendUpload({ type: 'upload_start', files: sourceFiles.map(({ bytes, sent, confirmed,
            received, destination, ...file }) => file) });
        await targetData.next(message => message.type === 'upload_start');
        sendTarget({ type: 'upload_ready', assets: inventory() });
        const ready = await upload.next(message => message.type === 'upload_ready');
        let windowBytes = ready.windowBytes;
        let sentBytes = 0, confirmedBytes = 0, receivedBytes = 0;
        targetData.observe(message => {
            if (message.type !== 'upload_chunk') return;
            const file = sourceFiles.find(file => file.assetId === message.assetId);
            assert.equal(message.offset, file.received);
            const decoded = Buffer.from(message.data, 'base64');
            assert.equal(decoded.length, message.size);
            decoded.copy(file.destination, file.received);
            file.received += decoded.length; receivedBytes += decoded.length;
            sendTarget({ type: 'upload_progress', delta: true,
                assets: [{ assetId: file.assetId, offset: file.received, size: file.size, sha256: file.sha256 }] });
        });
        upload.observe(message => {
            if (message.type !== 'upload_progress') return;
            windowBytes = message.windowBytes;
            for (const entry of message.assets) {
                const file = sourceFiles.find(file => file.assetId === entry.assetId);
                confirmedBytes += entry.offset - file.confirmed; file.confirmed = entry.offset;
            }
        });
        const start = performance.now();
        limited = true;
        // One acknowledged block per pump makes queue occupancy measurable and
        // handles every batch size without turning latency into an idle error.
        while (confirmedBytes < totalSize) {
            assert(performance.now() - start < 90000, 'transfer exceeds bounded probe runtime');
            if (sentBytes === confirmedBytes) {
                const file = sourceFiles.find(file => file.sent < file.size);
                if (file) {
                    const size = Math.min(32768, windowBytes, file.size - file.sent);
                    sendUpload({ type: 'upload_chunk', assetId: file.assetId, offset: file.sent, size,
                        sha256: file.sha256, data: file.bytes.subarray(file.sent, file.sent + size).toString('base64') });
                    file.sent += size; sentBytes += size;
                }
            }
            await sleep(2);
        }
        sendUpload({ type: 'upload_complete', assets: inventory() });
        await targetData.next(message => message.type === 'upload_complete');
        for (const file of sourceFiles) {
            assert.equal(crypto.createHash('sha256').update(file.destination).digest('hex'), file.sha256);
        }
        sendTarget({ type: 'upload_finished', assets: inventory() });
        await upload.next(message => message.type === 'upload_finished');
        for (const peer of peers) {
            assert.equal(peer.ws.readyState, WebSocket.OPEN);
            assert.equal(peer.observations.some(event => event.message
                && server.uploadDataMessageTypes.has(event.message.type)), false,
                'upload data must never be received on control');
        }
        const limitedPeer = mode === 'downlink' ? target : owner;
        const heartbeatEvents = limitedPeer.observations.filter(event => event.at >= start
            && event.message?.type === 'heartbeat_ack');
        const heartbeatTimes = [start, ...heartbeatEvents.map(event => event.at), performance.now()];
        const maxHeartbeatGapMs = Math.max(...heartbeatTimes.slice(1).map((at, index) => at - heartbeatTimes[index]));
        assert(maxHeartbeatGapMs < server.config.transportTimeoutMs);
        assert.equal(server.remoteSessions.commandReady(server.remoteSessions.get(session.remoteSessionId)), true);
        console.log(`AUDIT_RESULT ${JSON.stringify({ mode, rateKiB, fileCount,
            protocolVersion: server.protocolVersion, sha256Verified: true, receivedBytes, deliveredWireBytes,
            durationMs: Math.round(performance.now() - start), maxHeartbeatGapMs: Math.round(maxHeartbeatGapMs),
            uploadChunksOnControl: 0, note: 'Real WebSockets and shaped TCP; synthetic receiver durability, no Qt playback.' })}`);
    } finally {
        clearInterval(drain);
        for (const peer of peers) { clearInterval(peer.heartbeat); peer.ws.terminate(); }
        upload?.ws.terminate();
        targetData?.ws.terminate();
        for (const socket of sockets) socket.destroy();
        await new Promise(resolve => proxy.close(resolve));
        clearInterval(server.uploadCleanupInterval);
        clearInterval(server.leaseSweepInterval);
        for (const client of server.wss.clients) client.terminate();
        await new Promise(resolve => server.wss.close(resolve));
    }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
