'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { EventEmitter, once } = require('node:events');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');
const { parseScreenFrame, MAX_FRAME_BYTES, MAX_BUFFERED_BYTES } = require('./screen_share_relay');

function socket() {
    return Object.assign(new EventEmitter(), {
        readyState: WebSocket.OPEN, bufferedAmount: 0, messages: [],
        send(data, options, done) { this.messages.push(Buffer.isBuffer(data) ? data : JSON.parse(data)); if (done) done(); },
        close() { this.readyState = WebSocket.CLOSED; this.emit('close'); },
    });
}
function context() {
    let now = 100;
    const server = new MouffetteServer({ port: 0, host: '127.0.0.1',
        monotonicNow: () => now, protocolLogger: () => {}, metricLogger: () => {} });
    function client(name) {
        const ws = socket();
        const value = { id: `${name}-transport`, endpointId: name, runtimeId: `${name}-runtime`,
            connectionGeneration: 1, authenticated: true, ws, screens: [{ id: 0, width: 1920, height: 1080 }] };
        server.clients.set(value.id, value);
        server.currentTransportByEndpoint.set(value.endpointId, value);
        return value;
    }
    const owner = client('owner'), target = client('target'), stranger = client('stranger');
    const session = server.remoteSessions.open({ ownerEndpointId: owner.endpointId,
        targetEndpointId: target.endpointId, ownerRuntimeId: owner.runtimeId,
        targetRuntimeId: target.runtimeId, ownerConnectionGeneration: 1, targetConnectionGeneration: 1 }).session;
    const command = (source, type, extra = {}) => server.screenShare.handleControl(source.id, {
        type, remoteSessionId: session.remoteSessionId, generation: session.generation,
        connectionGeneration: source.connectionGeneration, ...extra });
    const enable = () => {
        command(target, 'screen_share_consent', { enabled: true });
        command(owner, 'screen_share_subscribe', { enabled: true });
        return server.screenShare.subscriptions.get(session.remoteSessionId);
    };
    const video = source => {
        const ws = socket();
        server.screenShare.sockets.set(source, ws);
        server.screenShare.refreshForClient(source);
        return ws;
    };
    return { server, owner, target, stranger, session, command, enable, video, tick: ms => { now += ms; } };
}
function frame(entry, extra = {}, bytes = 20) {
    const header = Buffer.from(JSON.stringify({ remoteSessionId: entry.session.remoteSessionId,
        generation: entry.generation, streamId: entry.streamId, screenId: 0, sequence: 1,
        width: 1280, height: 720, keyFrame: true, codec: 'h264', ...extra }));
    const prefix = Buffer.alloc(6); prefix.write('MSV1'); prefix.writeUInt16BE(header.length, 4);
    const data = Buffer.alloc(bytes); data.set([0, 0, 0, 1, 0x65]);
    return Buffer.concat([prefix, header, data]);
}

// No consent, reverse traffic, strangers, obsolete sessions or transports.
{
    const c = context(); const { server, owner, target, stranger, command } = c;
    const ownerVideo = c.video(owner), targetVideo = c.video(target), strangerVideo = c.video(stranger);
    command(owner, 'screen_share_subscribe', { enabled: true });
    const entry = server.screenShare.subscriptions.get(c.session.remoteSessionId);
    assert.equal(entry.enabled, false);
    assert.equal(command(owner, 'screen_share_consent', { enabled: true }), true);
    assert.equal(entry.enabled, false, 'the viewer cannot grant target consent');
    c.enable(); assert.equal(entry.enabled, true);
    assert.equal(server.screenShare.handleFrame(owner, ownerVideo, frame(entry)), false);
    assert.equal(server.screenShare.handleFrame(stranger, strangerVideo, frame(entry)), false);
    assert.equal(server.screenShare.handleFrame(target, targetVideo, frame(entry, { generation: 9 })), false);
    assert.equal(server.screenShare.handleFrame(target, targetVideo, frame(entry, { screenId: 999 })), false);
    assert.equal(server.screenShare.handleFrame(target, targetVideo, frame(entry)), true);
    assert.equal(ownerVideo.messages.length, 1);
    assert.equal(server.screenShare.handleFrame(target, targetVideo, frame(entry)), false, 'duplicate sequence');
    const previous = entry.streamId;
    command(target, 'screen_share_consent', { enabled: false });
    assert.equal(server.screenShare.window(ownerVideo).frames.size, 0, 'revocation releases old receipt epoch');
    assert.equal(server.screenShare.handleFrame(target, targetVideo, frame(entry, { streamId: previous, sequence: 2 })), false);
    command(target, 'screen_share_consent', { enabled: true });
    assert.notEqual(entry.streamId, previous);
    assert.equal(server.screenShare.handleFrame(target, targetVideo, frame(entry, { streamId: previous, sequence: 2 })), false);
    target.connectionGeneration = 2;
    assert.equal(server.screenShare.handleFrame(target, targetVideo, frame(entry, { sequence: 2 })), false);
}

// Pressure never builds a stale GOP backlog, and IDRs larger than the queue
// watermark are accepted only when there is no older buffered data.
{
    const c = context(); const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { keyFrame: false })), false);
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { sequence: 2 })), true);
    c.server.screenShare.handleAcknowledgement(c.owner, output, Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence: 2 })));
    output.bufferedAmount = MAX_BUFFERED_BYTES + 1;
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { sequence: 3, keyFrame: false })), false);
    output.bufferedAmount = 0;
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { sequence: 4, keyFrame: false })), false);
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { sequence: 5 }, 700000)), true);
    assert.equal(output.messages.length, 2);
    c.server.screenShare.handleAcknowledgement(c.owner, output, Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence: 5 })));
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { sequence: 7, keyFrame: false })), false, 'missing source access unit needs an IDR');
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { sequence: 8 })), true);
    const keyframes = c.target.ws.messages.filter(message => message.type === 'screen_share_keyframe');
    assert.equal(keyframes.length, 1, 'keyframe requests are rate limited');
}

// A reused physical screen ID after a hotplug requires a new grant. Unchanged
// periodic topology snapshots preserve the current encoder/GOP.
{
    const c = context(); c.video(c.owner); const input = c.video(c.target), entry = c.enable();
    const original = entry.streamId;
    c.server.screenShare.refreshSession(c.session);
    assert.equal(entry.streamId, original);
    c.target.screens = [{ id: 0, width: 1080, height: 1920 }];
    c.server.screenShare.refreshForClient(c.target);
    assert.notEqual(entry.streamId, original);
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { streamId: original })), false);
    c.command(c.target, 'screen_share_status', { streamId: entry.streamId, reason: 'capture_error' });
    c.server.screenShare.refreshSession(c.session);
    assert.equal(c.owner.ws.messages.at(-1).reason, 'capture_error');
    assert.equal(entry.enabled, true, 'transient capture errors preserve the request');
    c.server.screenShare.revokeClient(c.target);
    assert.equal(c.server.screenShare.subscriptions.size, 0);
    assert.equal(input.readyState, WebSocket.CLOSED);
}

// Receiver consumption, not kernel write completion, bounds outstanding
// work. A forged tuple or receipt from another socket cannot release credit.
{
    const c = context(); const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare;
    for (let sequence = 1; sequence <= 3; ++sequence)
        assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence })), true);
    assert.equal(output.bufferedAmount, 0);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 4 })), false);
    const receipt = (sequence, streamId = entry.streamId) => Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId, screenId: 0, sequence }));
    assert.equal(relay.handleAcknowledgement(c.owner, output, receipt(1000)), true);
    assert.equal(relay.handleAcknowledgement(c.owner, output, receipt(1, crypto.randomUUID())), true);
    assert.equal(relay.window(output).frames.size, 3);
    assert.equal(relay.handleAcknowledgement(c.target, output, receipt(1)), false);
    assert.equal(relay.window(output).frames.size, 3);
    assert.equal(relay.handleAcknowledgement(c.owner, output, receipt(1)), true);
    assert.equal(relay.window(output).frames.size, 2);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 5, keyFrame: false })), false);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 6 })), true);
    c.tick(1500); relay.sweep();
    assert.equal(output.readyState, WebSocket.CLOSED, 'stale kernel queue is discarded at receipt deadline');
    assert.equal(relay.inflight.size, 0);
}

// Four continuously updating displays share a simulated 1 Mbps recipient.
// The OS reports zero queued bytes throughout: only receipts expose pressure.
// Stable screen callback order must not starve the fourth display.
{
    const c = context();
    c.target.screens = [0, 1, 2, 3].map(id => ({ id, width: 1920, height: 1080 }));
    const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare, received = [0, 0, 0, 0], receipts = [];
    let networkAvailableAt = 0;
    for (let time = 0; time <= 2500; time += 20) {
        c.tick(20);
        while (receipts.length && receipts[0].at <= time) {
            const { metadata } = receipts.shift();
            relay.handleAcknowledgement(c.owner, output, Buffer.from(JSON.stringify({
                type: 'screen_frame_ack', streamId: metadata.streamId,
                screenId: metadata.screenId, sequence: metadata.sequence })));
        }
        for (let screenId = 0; screenId < 4; ++screenId) {
            const packet = frame(entry, { screenId, sequence: time / 20 + 1 }, 6000);
            if (relay.handleFrame(c.target, input, packet)) {
                received[screenId]++;
                networkAvailableAt = Math.max(networkAvailableAt, time) + packet.length / 125;
                receipts.push({ at: networkAvailableAt + 40, metadata: parseScreenFrame(packet) });
            }
        }
        assert.ok(relay.window(output).frames.size <= 3);
        assert.ok(relay.window(output).bytes <= MAX_BUFFERED_BYTES);
    }
    assert.ok(received.every(count => count >= 8), `all displays must advance: ${received}`);
    assert.ok(Math.max(...received) - Math.min(...received) <= 2, `fair admission: ${received}`);
    assert.ok(relay.window(output).waiting.size <= 4);
}

// Canonical metadata, strict payload bounds, safe integers and codec framing.
{
    const c = context(); c.video(c.owner); c.video(c.target); const entry = c.enable();
    assert.ok(parseScreenFrame(frame(entry)));
    assert.ok(parseScreenFrame(frame(entry, { timestampUs: 1000000 })));
    for (const mutation of [{ width: 1921 }, { height: 1921 }, { width: 1279 }, { generation: 1.5 },
        { sequence: 0 }, { sequence: Number.MAX_SAFE_INTEGER + 1 }, { keyFrame: 'yes' },
        { codec: 'av1' }, { timestampUs: -1 }, { streamId: 'forged' }, { token: 'secret' }])
        assert.equal(parseScreenFrame(frame(entry, mutation)), null);
    assert.equal(parseScreenFrame(frame(entry, {}, MAX_FRAME_BYTES + 1)), null);
    assert.equal(parseScreenFrame(Buffer.from('MSV1')), null);
    const wrongMagic = frame(entry); wrongMagic[0] = 0;
    assert.equal(parseScreenFrame(wrongMagic), null);
    const avcc = frame(entry); avcc[avcc.length - 20] = 2;
    assert.equal(parseScreenFrame(avcc), null);
}

// Screen credentials are one-shot, short-lived and tied to the exact admitted
// control object, never just its endpoint name.
{
    const c = context(); const relay = c.server.screenShare;
    relay.issueToken(c.target.id, 'first'); const first = c.target.ws.messages.at(-1).token;
    assert.equal(relay.consumeToken(first), c.target);
    assert.equal(relay.consumeToken(first), null);
    relay.issueToken(c.target.id, 'expired'); const expired = c.target.ws.messages.at(-1).token;
    c.tick(15000); assert.equal(relay.consumeToken(expired), null);
    relay.issueToken(c.target.id, 'stale'); const stale = c.target.ws.messages.at(-1).token;
    c.server.currentTransportByEndpoint.set(c.target.endpointId, c.stranger);
    assert.equal(relay.consumeToken(stale), null);
}

async function socketIntegration() {
    const c = context(); const { server } = c;
    server.start();
    await once(server.wss, 'listening');
    const base = `ws://127.0.0.1:${server.wss.address().port}`;
    const peers = [];
    try {
        const open = async client => {
            server.screenShare.issueToken(client.id, crypto.randomUUID());
            const token = client.ws.messages.at(-1).token;
            const ws = new WebSocket(`${base}/?channel=screen&token=${token}`);
            peers.push(ws);
            const [encoded, binary] = await once(ws, 'message');
            assert.equal(binary, false);
            const ready = JSON.parse(encoded);
            assert.equal(ready.type, 'screen_channel_ready');
            assert.equal(ready.protocolVersion, server.protocolVersion);
            assert.equal(ready.serverBootId, server.serverBootId);
            assert.equal(ready.connectionGeneration, client.connectionGeneration);
            assert.equal(ready.endpointId, client.endpointId);
            assert.match(ready.messageId, /^[0-9a-f-]{36}$/);
            return { ws, token };
        };
        const owner = await open(c.owner), target = await open(c.target);
        const entry = c.enable(); const packet = frame(entry);
        const received = once(owner.ws, 'message');
        const acknowledged = once(target.ws, 'message');
        target.ws.send(packet);
        const [receipt, ackBinary] = await acknowledged;
        assert.equal(ackBinary, false);
        const ack = JSON.parse(receipt);
        assert.equal(ack.type, 'screen_frame_ack');
        assert.equal(ack.streamId, entry.streamId);
        assert.equal(ack.sequence, 1);
        assert.equal(ack.connectionGeneration, 1);
        const [relayed, binary] = await received;
        assert.equal(binary, true); assert.deepEqual(relayed, packet);
        owner.ws.send(JSON.stringify({ type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence: 1 }));
        const replay = new WebSocket(`${base}/?channel=screen&token=${target.token}`); peers.push(replay);
        const [code] = await once(replay, 'close'); assert.equal(code, 1008);
        const closed = once(target.ws, 'close');
        const serverClosed = once(server.screenShare.sockets.get(c.target), 'close');
        target.ws.send(JSON.stringify({ type: 'screen_share_consent', enabled: true }));
        const [invalidCode] = await closed; assert.equal(invalidCode, 1008);
        await serverClosed;
        assert.equal(entry.enabled, false);
    } finally {
        for (const peer of peers) peer.terminate();
        for (const peer of server.wss.clients) peer.terminate();
        clearInterval(server.uploadCleanupInterval); clearInterval(server.leaseSweepInterval);
        await new Promise(resolve => server.wss.close(resolve));
    }
}

socketIntegration().then(() => console.log('screen_share_protocol: all tests passed'))
    .catch(error => { console.error(error); process.exitCode = 1; });
