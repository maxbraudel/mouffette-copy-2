'use strict';

// Real Node coordinator, driven only over this test process's standard input.
// Faults are injected at the network boundary, not inside either state machine.
const readline = require('node:readline');
const { MouffetteServer } = require('../../../server/server');
const relay = new MouffetteServer({ port: Number(process.argv[2] || 0), host: "127.0.0.1", protocolLogger: () => {}, metricLogger: () => {} });
const dropped = new Map();
const droppedIncoming = new Map();
const rawDrops = new Map();
const suppressedProofs = new Set();
const dropAfterProgress = new Set();
const send = relay.sendToEndpoint.bind(relay);
relay.sendToEndpoint = (endpoint, payload) => {
    if (payload.type === 'upload_progress' && payload.durableBytes > 0
        && dropAfterProgress.delete(endpoint)) {
        process.stdout.write(`TEST_UPLOAD_CUT ${payload.durableBytes}\n`);
        for (const client of relay.clients.values())
            if (client.endpointId === endpoint) client.ws.terminate();
        return true;
    }
    const key = `${endpoint}:${payload.type}`;
    const remaining = dropped.get(key) || 0;
    if (remaining > 0) {
        dropped.set(key, remaining - 1);
        process.stdout.write(`TEST_DROPPED ${key}\n`);
        return true; // queued by the transport, never applied by the recipient
    }
    if (payload.type === 'heartbeat_ack' && suppressedProofs.has(endpoint))
        payload = { ...payload, sessionStates: [] };
    return send(endpoint, payload);
};
const receive = relay.handleMessage.bind(relay);
relay.handleMessage = (clientId, payload, ...args) => {
    if (process.env.MOUFFETTE_TEST_AUDIO_LEGACY === '1'
        && (payload.type.startsWith('audio_') || payload.type === 'request_audio_channel'))
        process.stdout.write('TEST_UNEXPECTED_AUDIO\n');
    const endpoint = relay.clients.get(clientId)?.endpointId;
    const key = `${endpoint}:${payload.type}`;
    const rule = droppedIncoming.get(key);
    if (rule && rule.count > 0 && (payload.stateRevision || 0) >= rule.minimumRevision) {
        --rule.count;
        process.stdout.write(`TEST_DROPPED ${key}\n`);
        return;
    }
    return receive(clientId, payload, ...args);
};
relay.start();
// Includes direct ws.send paths such as registration and upload token replies.
relay.wss.on('connection', socket => {
    const rawSend = socket.send.bind(socket);
    socket.send = (data, ...args) => {
        let payload;
        try { payload = JSON.parse(data.toString()); } catch { return rawSend(data, ...args); }
        if (process.env.MOUFFETTE_TEST_AUDIO_LEGACY === '1' && payload.type === 'welcome') {
            delete payload.audioVersion;
            data = JSON.stringify(payload);
        }
        const client = [...relay.clients.values()].find(candidate => candidate.ws === socket);
        const key = `${client?.endpointId}:${payload.type}`;
        const count = rawDrops.get(key) || 0;
        if (count > 0) {
            rawDrops.set(key, count - 1);
            process.stdout.write(`TEST_DROPPED ${key}\n`);
            return;
        }
        return rawSend(data, ...args);
    };
});
relay.wss.once('listening', () => {
    process.stdout.write(`TEST_READY ${relay.wss.address().port}\n`);
});
readline.createInterface({ input: process.stdin }).on('line', line => {
    const command = JSON.parse(line);
    if (command.action === 'drop') {
        for (const endpoint of command.endpoints || []) {
            for (const client of relay.clients.values()) {
                if (client.endpointId === endpoint) client.ws.terminate();
            }
        }
    } else if (command.action === 'dropRawFrame') {
        rawDrops.set(`${command.endpoint}:${command.type}`, command.count || 1);
    } else if (command.action === 'dropFrame') {
        dropped.set(`${command.endpoint}:${command.type}`, command.count || 1);
    } else if (command.action === 'dropIncoming') {
        droppedIncoming.set(`${command.endpoint}:${command.type}`, {
            count: command.count || 1, minimumRevision: command.minimumRevision || 0,
        });
    } else if (command.action === 'suppressSessionProof') {
        suppressedProofs.add(command.endpoint);
        for (const client of relay.clients.values()) {
            if (client.endpointId !== command.endpoint) continue;
            const rawSend = client.ws.send.bind(client.ws);
            client.ws.send = (data, ...args) => {
                const payload = JSON.parse(data.toString());
                if (payload.type === 'heartbeat_ack') {
                    payload.sessionStates = [];
                    data = JSON.stringify(payload);
                }
                return rawSend(data, ...args);
            };
        }
    } else if (command.action === 'dropAfterUploadProgress') {
        dropAfterProgress.add(command.endpoint);
    } else if (command.action === 'testRemovalInstruction') {
        const session = relay.remoteSessions.get(command.sessionId);
        if (session) relay.sendToEndpoint(session.targetEndpointId, {
            type: 'upload_remove', remoteSessionId: session.remoteSessionId,
            generation: session.generation, connectionGeneration: session.ownerConnectionGeneration,
            ownerEndpointId: session.ownerEndpointId, targetEndpointId: session.targetEndpointId,
            removalId: command.removalId, uploadId: 'receipt_test_upload',
            assetId: 'a'.repeat(64), sha256: 'a'.repeat(64), fileId: 'a'.repeat(64),
            offset: 1, size: 1, extension: 'png',
        });
    } else if (command.action === 'shutdown') {
        for (const client of relay.clients.values()) client.ws.terminate();
        if (relay.leaseSweepInterval) clearInterval(relay.leaseSweepInterval);
        if (relay.uploadCleanupInterval) clearInterval(relay.uploadCleanupInterval);
        relay.wss.close(() => process.exit(0));
    }
    process.stdout.write(`TEST_DONE ${command.id || ''}\n`);
});
