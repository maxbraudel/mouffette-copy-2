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
function context(options = {}) {
    let now = 100;
    const server = new MouffetteServer({ port: 0, host: '127.0.0.1',
        monotonicNow: () => now, protocolLogger: () => {}, metricLogger: () => {} });
    // Exercise fairness under intentionally tight credit; production allows a
    // larger healthy bandwidth-delay product and bounds additional delay.
    Object.assign(server.screenShare, { maxInflightFrames: 6, maxBufferedBytes: 256 * 1024 }, options);
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
        ws.screenFeedbackVersion = 1;
        ws.screenMaximumEdge = 3840;
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

// A high RTT must not expire the next display's fairness turn before its
// predecessor can return credit. Hiding a waiter releases its turn immediately.
{
    const c = context({ maxInflightFrames: 1 });
    c.target.screens.push({ id: 1, width: 1920, height: 1080 });
    const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare;
    const receipt = (screenId, sequence) => Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId: entry.streamId, screenId, sequence }));
    assert.equal(relay.handleFrame(c.target, input, frame(entry)), true);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { screenId: 1 })), false);
    c.tick(800);
    assert.equal(relay.handleAcknowledgement(c.owner, output, receipt(0, 1)), true);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 2 })), false,
        'the first callback cannot bypass a waiting display after a healthy 800 ms RTT');
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { screenId: 1, sequence: 2 })), true);
    c.tick(800);
    assert.equal(relay.handleAcknowledgement(c.owner, output, receipt(1, 2)), true);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { screenId: 1, sequence: 3 })), false);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 3 })), true);
    c.tick(800);
    relay.handleAcknowledgement(c.owner, output, receipt(0, 3));
    assert.equal(c.command(c.owner, 'screen_share_subscribe', {
        enabled: true, screens: [{ screenId: 0, maximumEdge: 1280 }],
    }), true);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 4 })), true,
        'a hidden display cannot retain fairness priority');
    assert.equal(c.server.remoteSessions.commandReady(c.session), true);
}

// Capture failures are scoped to the physical screen. Refresh replays them
// after the global state without revoking a healthy screen or rotating grants.
{
    const c = context(); c.target.screens.push({ id: 1, width: 1920, height: 1080 });
    const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare, original = entry.streamId;
    const status = extra => c.command(c.target, 'screen_share_status', {
        streamId: original, reason: 'capture_error', screenId: 0, ...extra });
    assert.equal(relay.handleFrame(c.target, input, frame(entry)), true);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { screenId: 1 })), true);
    assert.equal(c.command(c.target, 'screen_share_status', { streamId: original, reason: 'streaming' }), true);
    assert.equal(status(), true);
    assert.equal(entry.status, 'streaming', 'a per-screen failure cannot overwrite the global status');
    assert.equal(c.owner.ws.messages.at(-1).screenId, 0);
    assert.equal(c.owner.ws.messages.at(-1).reason, 'capture_error');
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 2 })), false);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { screenId: 1, sequence: 2, keyFrame: false })), true);
    assert.equal(input.readyState, WebSocket.OPEN);
    assert.equal(output.readyState, WebSocket.OPEN);
    relay.refreshSession(c.session);
    assert.equal(entry.streamId, original);
    assert.equal(c.owner.ws.messages.at(-2).reason, 'streaming');
    assert.equal(c.owner.ws.messages.at(-2).screenId, undefined);
    assert.equal(c.owner.ws.messages.at(-1).reason, 'capture_error');
    assert.equal(c.owner.ws.messages.at(-1).screenId, 0);
    for (const screenId of [-1, 2, 0.5, '0', null])
        assert.equal(status({ screenId }), false, 'screen-scoped status requires a requested physical screen');
    assert.equal(status({ reason: 'unrecognized' }), false);
    assert.equal(status({ streamId: crypto.randomUUID() }), false);
    assert.equal(status({ generation: entry.generation + 1 }), false);
    for (const source of [c.owner, c.stranger])
        assert.equal(c.command(source, 'screen_share_status', { streamId: original, screenId: 1, reason: 'error' }), false);
    const commandReady = c.server.remoteSessions.commandReady;
    c.server.remoteSessions.commandReady = () => false;
    assert.equal(status({ reason: 'starting' }), false, 'unready control cannot clear a capture failure');
    c.server.remoteSessions.commandReady = commandReady;
    assert.equal(entry.screenStatuses.get(0), 'capture_error');
    assert.equal(status({ reason: 'starting' }), true);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 2, keyFrame: false })), false,
        'recovery starts from an independently decodable frame');
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 3 })), true);
    assert.equal(status({ reason: 'streaming' }), true);
    assert.equal(entry.screenStatuses.get(0), 'streaming');

    assert.equal(status(), true);
    assert.equal(c.command(c.owner, 'screen_share_subscribe', {
        enabled: true, screens: [{ screenId: 1, maximumEdge: 1280 }],
    }), true);
    assert.equal(entry.screenStatuses.has(0), false, 'hiding a screen removes its cached capture error');
    assert.equal(status(), false, 'a hidden screen cannot publish status');
    assert.equal(c.command(c.owner, 'screen_share_subscribe', { enabled: true }), true);
    assert.equal(entry.streamId, original);
    assert.equal(c.owner.ws.messages.at(-1).screenId, undefined, 're-added screen has no stale error replay');
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 4 })), true);
    assert.equal(status(), true);
    c.command(c.target, 'screen_share_consent', { enabled: false });
    c.command(c.target, 'screen_share_consent', { enabled: true });
    assert.notEqual(entry.streamId, original);
    assert.equal(entry.screenStatuses.size, 0, 'revocation removes every old-epoch screen status');
    assert.equal(relay.handleFrame(c.target, input, frame(entry)), true);
}

// Receiver consumption, not kernel write completion, bounds outstanding
// work. A forged tuple or receipt from another socket cannot release credit.
{
    const c = context(); const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare;
    for (let sequence = 1; sequence <= relay.maxInflightFrames; ++sequence)
        assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence })), true);
    assert.equal(output.bufferedAmount, 0);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 7 })), false);
    const receipt = (sequence, streamId = entry.streamId) => Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId, screenId: 0, sequence }));
    assert.equal(relay.handleAcknowledgement(c.owner, output, receipt(1000)), true);
    assert.equal(relay.handleAcknowledgement(c.owner, output, receipt(1, crypto.randomUUID())), true);
    assert.equal(relay.window(output).frames.size, relay.maxInflightFrames);
    assert.equal(relay.handleAcknowledgement(c.target, output, receipt(1)), false);
    assert.equal(relay.window(output).frames.size, relay.maxInflightFrames);
    assert.equal(relay.handleAcknowledgement(c.owner, output, receipt(1)), true);
    assert.equal(relay.window(output).frames.size, relay.maxInflightFrames - 1);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 8, keyFrame: false })), false);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 9 })), true);
    c.tick(relay.ackTimeoutMs); relay.sweep();
    assert.equal(output.readyState, WebSocket.CLOSED, 'stale kernel queue is discarded at receipt deadline');
    assert.equal(relay.inflight.size, 0);
}

// Eight continuously updating displays share a simulated 1 Mbps recipient.
// The OS reports zero queued bytes throughout: only receipts expose pressure.
// Stable screen callback order must not starve displays beyond the six slots.
{
    const c = context();
    c.target.screens = Array.from({ length: 8 }, (_, id) => ({ id, width: 1920, height: 1080 }));
    const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare, received = Array(8).fill(0), receipts = [];
    let networkAvailableAt = 0;
    for (let time = 0; time <= 2500; time += 20) {
        c.tick(20);
        while (receipts.length && receipts[0].at <= time) {
            const { metadata } = receipts.shift();
            relay.handleAcknowledgement(c.owner, output, Buffer.from(JSON.stringify({
                type: 'screen_frame_ack', streamId: metadata.streamId,
                screenId: metadata.screenId, sequence: metadata.sequence })));
        }
        for (let screenId = 0; screenId < received.length; ++screenId) {
            const packet = frame(entry, { screenId, sequence: time / 20 + 1 }, 6000);
            if (relay.handleFrame(c.target, input, packet)) {
                received[screenId]++;
                networkAvailableAt = Math.max(networkAvailableAt, time) + packet.length / 125;
                receipts.push({ at: networkAvailableAt + 40, metadata: parseScreenFrame(packet) });
            }
        }
        assert.ok(relay.window(output).frames.size <= relay.maxInflightFrames);
        assert.ok(relay.window(output).bytes <= MAX_BUFFERED_BYTES);
    }
    assert.ok(received.every(count => count >= 4), `all displays must advance: ${received}`);
    assert.ok(Math.max(...received) - Math.min(...received) <= 2, `fair admission: ${received}`);
    assert.ok(relay.window(output).waiting.size <= received.length);
}

// Canonical metadata, strict payload bounds, safe integers and codec framing.
{
    const c = context(); c.video(c.owner); c.video(c.target); const entry = c.enable();
    assert.ok(parseScreenFrame(frame(entry)));
    assert.ok(parseScreenFrame(frame(entry, { timestampUs: 1000000 })));
    for (const mutation of [{ width: 3842 }, { height: 3842 }, { width: 1279 }, { generation: 1.5 },
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

// Healthy propagation delay is part of the bandwidth-delay product, not a
// queue to eliminate. A 500 ms path can carry more than six full frames without
// falsely triggering congestion or losing the control session.
{
    const c = context({ maxInflightFrames: 64, maxBufferedBytes: 2048 * 1024 });
    const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare;
    const receipt = sequence => relay.handleAcknowledgement(c.owner, output, Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence })));
    const reports = () => input.messages.filter(message => message.type === 'screen_share_feedback');
    for (let sequence = 1; sequence <= 16; ++sequence)
        assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence }, 50000)), true);
    c.tick(500); relay.sweep();
    assert.equal(reports().length, 0, 'the initial propagation grace is not a 150 ms deadline');
    for (let sequence = 1; sequence <= 16; ++sequence) receipt(sequence);
    assert.equal(relay.window(output).baselineRttMs, 500);
    assert.equal(relay.window(output).frames.size, 0);
    for (let sequence = 17; sequence <= 32; ++sequence)
        assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence }, 50000)), true);
    c.tick(500); relay.sweep();
    for (let sequence = 17; sequence <= 32; ++sequence) receipt(sequence);
    c.tick(500); relay.sweep();
    assert.ok(reports().length > 0);
    assert.ok(reports().every(report => report.congested === false));
    assert.equal(output.messages.length, 32);
    assert.equal(output.readyState, WebSocket.OPEN);
    assert.equal(c.server.remoteSessions.commandReady(c.session), true);
}

// Exhausted frame credit alone is not evidence of congestion. After a real
// low-delay ACK establishes the baseline, additional age stops admission and
// reports pressure before the independent hard receipt timeout.
{
    const c = context(); const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare;
    for (let sequence = 1; sequence <= 6; ++sequence)
        assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence })), true);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 7 })), false);
    assert.equal(entry.feedback.size, 0, 'frame-count credit is not a network measurement');
    c.tick(40);
    for (let sequence = 1; sequence <= 6; ++sequence)
        relay.handleAcknowledgement(c.owner, output, Buffer.from(JSON.stringify({
            type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence })));
    c.tick(relay.feedbackIntervalMs); relay.sweep();
    assert.equal(relay.window(output).baselineRttMs, 40);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 8 })), true);
    c.tick(300);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 9 })), false);
    relay.sweep();
    c.tick(relay.feedbackIntervalMs); relay.sweep();
    const reports = input.messages.filter(message => message.type === 'screen_share_feedback');
    assert.ok(reports.some(report => report.congested === true));
    assert.equal(relay.window(output).frames.size, 1, 'the delayed window admits no additional frame');
    assert.equal(output.readyState, WebSocket.OPEN, 'adaptation precedes hard socket replacement');
    assert.equal(c.server.remoteSessions.commandReady(c.session), true);
}

// A route can change while TCP stays connected. An old low RTT must not
// condemn a new, stable 800 ms path indefinitely; the baseline ages after 30 s.
{
    const c = context(); const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare;
    const receipt = sequence => relay.handleAcknowledgement(c.owner, output, Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence })));
    const heartbeat = () => {
        for (const peer of [c.owner, c.target])
            assert.equal(c.server.remoteSessions.touch(c.session.remoteSessionId,
                peer.endpointId, peer.connectionGeneration).ok, true);
    };
    assert.equal(relay.handleFrame(c.target, input, frame(entry)), true);
    c.tick(40); receipt(1);
    assert.equal(relay.window(output).baselineRttMs, 40);
    for (let second = 0; second < 30; ++second) { c.tick(1000); heartbeat(); }
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence: 2 })), true);
    c.tick(800); heartbeat(); relay.sweep(); receipt(2);
    assert.equal(relay.window(output).baselineRttMs, 800);
    input.messages.length = 0;
    for (let sequence = 3; sequence <= 5; ++sequence) {
        assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence })), true);
        c.tick(800); heartbeat(); relay.sweep(); receipt(sequence);
    }
    const reports = input.messages.filter(message => message.type === 'screen_share_feedback');
    assert.ok(reports.length > 0);
    assert.ok(reports.every(report => report.congested === false));
    assert.equal(output.readyState, WebSocket.OPEN);
    assert.equal(entry.streamId, parseScreenFrame(output.messages[0]).streamId);
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

// Viewer demand is a bounded topology subset. A viewport change preserves the
// stream identity, drops hidden displays immediately, and re-adds them at IDR.
{
    const c = context();
    c.target.screens.push({ id: 1, width: 3840, height: 2160 });
    const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const original = entry.streamId;
    const selection = [{ screenId: 1, maximumEdge: 640 }];
    assert.equal(c.command(c.owner, 'screen_share_subscribe', { enabled: true, screens: selection }), true);
    assert.equal(entry.streamId, original);
    assert.deepEqual(c.target.ws.messages.at(-1).screens, selection);
    assert.equal(c.target.ws.messages.at(-1).receiverMaximumEdge, 3840,
        'decoder capability is independent of a small viewport demand');
    assert.deepEqual(c.owner.ws.messages.at(-1).screens, selection);
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry)), false);
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { screenId: 1, width: 3840, height: 2160 })), true,
        'maximumEdge is a demand hint; the relay does not invalidate an in-flight old encode');
    for (const screens of [[{ screenId: 2, maximumEdge: 640 }], [{ screenId: 1, maximumEdge: 641 }],
        [{ screenId: 1, maximumEdge: 3842 }], [{ screenId: 1, maximumEdge: 640, extra: true }],
        [...selection, ...selection], null])
        assert.equal(c.command(c.owner, 'screen_share_subscribe', { enabled: true, screens }), false);
    assert.equal(c.command(c.target, 'screen_share_subscribe', { enabled: true, screens: [] }), false);
    assert.equal(c.command(c.owner, 'screen_share_subscribe', { enabled: true, screens: [] }), true);
    assert.equal(entry.streamId, original);
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { screenId: 1, sequence: 2 })), false);
    assert.equal(c.command(c.owner, 'screen_share_subscribe', { enabled: true, screens: selection }), true);
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { screenId: 1, sequence: 3, keyFrame: false })), false);
    assert.equal(c.server.screenShare.handleFrame(c.target, input, frame(entry, { screenId: 1, sequence: 4 })), true);
    assert.equal(c.command(c.owner, 'screen_share_subscribe', { enabled: true }), true);
    assert.equal(entry.requestedScreens, null, 'legacy subscribers receive every display');
    assert.equal(output.messages.length, 2);
}

// Only an exact receipt measures downstream delivery, never an unknown tuple.
// Coalesced feedback holds metadata only and includes receiver upload pressure.
{
    const c = context(); const output = c.video(c.owner), input = c.video(c.target), entry = c.enable();
    const relay = c.server.screenShare;
    const receipt = sequence => Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence }));
    const reports = () => input.messages.filter(message => message.type === 'screen_share_feedback');
    assert.equal(relay.handleFrame(c.target, input, frame(entry)), true);
    c.tick(120);
    relay.handleAcknowledgement(c.owner, output, receipt(999));
    assert.equal(reports().length, 0);
    relay.handleAcknowledgement(c.owner, output, receipt(1));
    assert.equal(reports().length, 1);
    assert.equal(reports()[0].deliveryRttMs, 120);
    assert.equal(reports()[0].bufferedBytes, 0);
    assert.equal(reports()[0].connectionGeneration, c.target.connectionGeneration);
    assert.equal(reports()[0].uploadActive, false);
    c.server.uploads.set('active', { targetEndpointId: c.owner.endpointId });
    output.bufferedAmount = relay.maxBufferedBytes + 1;
    for (let sequence = 2; sequence < 100; ++sequence)
        assert.equal(relay.handleFrame(c.target, input, frame(entry, { sequence })), false);
    assert.equal(entry.feedback.size, 1);
    assert.equal(reports().length, 1, 'congestion feedback is coalesced');
    c.tick(relay.feedbackIntervalMs); relay.sweep();
    assert.equal(reports().length, 2);
    assert.equal(reports()[1].congested, true);
    assert.equal(reports()[1].uploadActive, true);
    relay.sweep(); assert.equal(reports().length, 2, 'sweep does not repeat stale feedback');
    assert.equal(c.server.remoteSessions.commandReady(c.session), true, 'video congestion keeps session intact');
}

// Decoder feedback has no capacity to grant authority or release receipts.
{
    const c = context(); const output = c.video(c.owner), input = c.video(c.target), stranger = c.video(c.stranger), entry = c.enable();
    const relay = c.server.screenShare;
    const feedbackStreamId = entry.streamId;
    const feedback = extra => Buffer.from(JSON.stringify({ type: 'screen_view_feedback',
        remoteSessionId: c.session.remoteSessionId, generation: c.session.generation,
        streamId: feedbackStreamId, screenId: 0, decodeMs: 200, droppedFrames: 1, ...extra }));
    assert.equal(relay.handleFrame(c.target, input, frame(entry)), true);
    assert.equal(relay.handleAcknowledgement(c.stranger, stranger, feedback()), false);
    assert.equal(relay.handleAcknowledgement(c.target, input, feedback()), false);
    for (const invalid of [{ decodeMs: -1 }, { decodeMs: 60001 }, { droppedFrames: 1.5 }, { extra: true }])
        assert.equal(relay.handleAcknowledgement(c.owner, output, feedback(invalid)), false);
    for (const stale of [{ generation: c.session.generation + 1 }, { streamId: crypto.randomUUID() },
        { screenId: 999 }, { remoteSessionId: 'unknown' }])
        assert.equal(relay.handleAcknowledgement(c.owner, output, feedback(stale)), true);
    assert.equal(entry.feedback.size, 0, 'stale telemetry is discarded, never accepted as a sample');
    assert.equal(relay.handleAcknowledgement(c.owner, output, feedback()), true);
    assert.equal(relay.window(output).frames.size, 1);
    let reports = input.messages.filter(message => message.type === 'screen_share_feedback');
    assert.equal(reports.length, 1);
    assert.equal(reports[0].congested, true);
    assert.equal(reports[0].deliveryRttMs, 0, 'no fabricated RTT before the first receipt');
    for (let i = 0; i < 100; ++i) relay.handleAcknowledgement(c.owner, output, feedback());
    assert.equal(input.messages.filter(message => message.type === 'screen_share_feedback').length, 1);
    const commandReady = c.server.remoteSessions.commandReady;
    c.server.remoteSessions.commandReady = () => false;
    c.tick(relay.feedbackIntervalMs);
    assert.equal(relay.handleAcknowledgement(c.owner, output, feedback()), true);
    assert.equal(input.messages.filter(message => message.type === 'screen_share_feedback').length, 1);
    c.server.remoteSessions.commandReady = commandReady;
    c.command(c.target, 'screen_share_consent', { enabled: false });
    assert.equal(entry.feedback.size, 0);
    assert.equal(relay.handleAcknowledgement(c.owner, output, feedback()), true);
    assert.equal(entry.feedback.size, 0);
}

// Capabilities are attached to a one-shot channel token. Legacy publishers
// never receive new message types and legacy viewers never receive 4K frames.
{
    const c = context(); const relay = c.server.screenShare;
    for (const capability of [{ feedbackVersion: 2 }, { maximumEdge: 3842 }, { maximumEdge: 641 }])
        assert.equal(relay.issueToken(c.target.id, crypto.randomUUID(), capability), false);
    assert.equal(relay.issueToken(c.target.id, 'legacy'), true);
    const legacySocket = socket();
    assert.equal(relay.consumeToken(c.target.ws.messages.at(-1).token, legacySocket), c.target);
    assert.equal(legacySocket.screenFeedbackVersion, 0);
    assert.equal(legacySocket.screenMaximumEdge, 1920);
    assert.equal(relay.issueToken(c.target.id, 'modern', { feedbackVersion: 1, maximumEdge: 3840 }), true);
    const modernSocket = socket();
    assert.equal(relay.consumeToken(c.target.ws.messages.at(-1).token, modernSocket), c.target);
    assert.equal(modernSocket.screenFeedbackVersion, 1);
    assert.equal(modernSocket.screenMaximumEdge, 3840);
    const output = c.video(c.owner), input = c.video(c.target);
    output.screenMaximumEdge = 1920;
    input.screenFeedbackVersion = 0;
    const entry = c.enable();
    assert.deepEqual(c.target.ws.messages.at(-1).screens, [{ screenId: 0, maximumEdge: 1920 }]);
    assert.equal(c.target.ws.messages.at(-1).receiverMaximumEdge, 1920);
    assert.equal(relay.handleFrame(c.target, input, frame(entry, { width: 3840, height: 2160 })), false);
    assert.equal(relay.handleFrame(c.target, input, frame(entry)), true);
    c.tick(20);
    relay.handleAcknowledgement(c.owner, output, Buffer.from(JSON.stringify({
        type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence: 1 })));
    c.tick(relay.feedbackIntervalMs); relay.sweep();
    assert.equal(input.messages.filter(message => message.type === 'screen_share_feedback').length, 0);
}

async function socketIntegration() {
    const c = context(); const { server } = c;
    server.start();
    await once(server.wss, 'listening');
    const base = `ws://127.0.0.1:${server.wss.address().port}`;
    const peers = [];
    try {
        const open = async client => {
            server.screenShare.issueToken(client.id, crypto.randomUUID(), { feedbackVersion: 1, maximumEdge: 3840 });
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
            assert.equal(ready.feedbackVersion, 1);
            assert.equal(ready.maximumEdge, 3840);
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
        const feedbackReceived = once(target.ws, 'message');
        c.tick(120);
        owner.ws.send(JSON.stringify({ type: 'screen_frame_ack', streamId: entry.streamId, screenId: 0, sequence: 1 }));
        const [feedbackData, feedbackBinary] = await feedbackReceived;
        assert.equal(feedbackBinary, false);
        const feedback = JSON.parse(feedbackData);
        assert.equal(feedback.type, 'screen_share_feedback');
        assert.equal(feedback.deliveryRttMs, 120);
        assert.equal(feedback.connectionGeneration, c.target.connectionGeneration);
        assert.equal(feedback.remoteSessionId, c.session.remoteSessionId);
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
