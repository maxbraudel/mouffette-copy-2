'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { EventEmitter } = require('node:events');
const { MouffetteServer } = require('./server');
const { loadServerConfig } = require('./config');
const { parseScreenFrame } = require('./screen_share_relay');
const { parsePublicationFrame } = require('./screen_publications');

function socket() {
    return Object.assign(new EventEmitter(), { readyState: 1, bufferedAmount: 0, messages: [],
        send(data, options, done) { this.messages.push(Buffer.isBuffer(data) ? data : JSON.parse(data)); done?.(); },
        close(code, reason) { this.closed = { code, reason }; this.readyState = 3; this.emit('close'); },
    });
}
function context(config = {}) {
    let now = 100;
    const server = new MouffetteServer({ config: { ...loadServerConfig({ envFile: '/nonexistent' }), ...config },
        port: 0, host: '127.0.0.1', monotonicNow: () => now, protocolLogger: () => {}, metricLogger: () => {} });
    const relay = server.screenShare;
    const clients = [];
    function client(name, screens = [{ id: 0, width: 3840, height: 2160 }]) {
        const value = { id: name, endpointId: name, runtimeId: `${name}-runtime`, connectionGeneration: 1,
            authenticated: true, ws: socket(), screens };
        server.clients.set(name, value); server.currentTransportByEndpoint.set(name, value); clients.push(value);
        return value;
    }
    function token(client, role, maximumEdge = 3840) {
        assert.equal(relay.issueToken(client.id, crypto.randomUUID(), role
            ? { mediaVersion: 2, role, feedbackVersion: 1, maximumEdge } : {}), true);
        return client.ws.messages.at(-1);
    }
    function connect(client, role, maximumEdge = 3840) {
        const reply = token(client, role, maximumEdge), ws = socket();
        relay.acceptSocket(ws, reply.token); return ws;
    }
    function subscribe(owner, target, screens = undefined) {
        const session = server.remoteSessions.open({ ownerEndpointId: owner.endpointId, targetEndpointId: target.endpointId,
            ownerRuntimeId: owner.runtimeId, targetRuntimeId: target.runtimeId,
            ownerConnectionGeneration: 1, targetConnectionGeneration: 1 }).session;
        relay.handleControl(target.id, { type: 'screen_share_consent', enabled: true });
        const message = { type: 'screen_share_subscribe', remoteSessionId: session.remoteSessionId,
            generation: session.generation, connectionGeneration: 1, enabled: true, ...(screens === undefined ? {} : { screens }) };
        const ok = relay.handleControl(owner.id, message);
        return { ok, session, message, entry: relay.subscriptions.get(session.remoteSessionId) };
    }
    const tick = elapsed => {
        now += elapsed;
        for (const value of clients) value.lastHeartbeatMonotonicAt = now;
        for (const session of server.remoteSessions.sessions.values())
            for (const value of clients) if (session.lastContact.has(value.endpointId)) session.lastContact.set(value.endpointId, now);
    };
    return { server, relay, shared: relay.shared, client, token, connect, subscribe, tick };
}
function frame(publication, extra = {}, bytes = 100) {
    const header = Buffer.from(JSON.stringify({ publicationId: publication.publicationId, screenId: 0, layer: 'main',
        sequence: 1, width: 1920, height: 1080, keyFrame: true, codec: 'h264', bitrateBps: 2_000_000, fps: 30, ...extra }));
    const prefix = Buffer.alloc(6); prefix.write('MSV2'); prefix.writeUInt16BE(header.length, 4);
    const payload = Buffer.alloc(bytes); payload.set([0, 0, 0, 1, 0x67, 1, 0, 0, 0, 1, 0x68, 1, 0, 0, 0, 1, 0x65, 1]);
    return Buffer.concat([prefix, header, payload]);
}
const media = ws => ws.messages.filter(Buffer.isBuffer).map(parseScreenFrame);
const acknowledgements = ws => ws.messages.filter(message => message.type === 'screen_publication_ack');
function ack(c, client, ws, metadata) {
    return c.relay.handleAcknowledgement(client, ws, Buffer.from(JSON.stringify({ type: 'screen_frame_ack',
        streamId: metadata.streamId, screenId: metadata.screenId, sequence: metadata.sequence })));
}

// One input access unit reaches ten independently authorized viewers. A late
// join does not rotate publication identity; the eleventh has no grant.
{
    const c = context(), source = c.client('source'), input = c.connect(source, 'publish');
    const viewers = [];
    let publication;
    for (let i = 0; i < 10; ++i) {
        const owner = c.client(`viewer-${i}`), output = c.connect(owner, 'view'), subscribed = c.subscribe(owner, source);
        assert.equal(subscribed.ok, true);
        publication ||= c.shared.byClient.get(source);
        assert.equal(c.shared.byClient.get(source), publication);
        viewers.push({ owner, output, ...subscribed });
    }
    const extra = c.client('eleventh'); c.connect(extra, 'view');
    assert.equal(c.subscribe(extra, source).ok, false);
    assert.equal(extra.ws.messages.at(-1).reason, 'viewer_capacity');
    input.emit('message', frame(publication), true);
    assert.equal(acknowledgements(input).length, 1);
    for (const viewer of viewers) {
        assert.equal(media(viewer.output).length, 1);
        assert.equal(media(viewer.output)[0].streamId, viewer.entry.streamId);
        assert.equal(media(viewer.output)[0].remoteSessionId, viewer.session.remoteSessionId);
    }
    assert.equal(source.ws.messages.some(message => message.type === 'screen_share_request'), false);
    const first = viewers[0];
    c.relay.handleControl(first.owner.id, { ...first.message, screens: [] });
    assert.equal(c.shared.byClient.get(source).publicationId, publication.publicationId);
    for (const viewer of viewers.slice(1)) c.relay.handleControl(viewer.owner.id, { ...viewer.message, enabled: false });
    assert.equal(c.shared.byClient.has(source), false);
    assert.equal(c.shared.cacheBytes, 0);
    assert.equal(source.ws.messages.at(-1).type, 'screen_publication_request');
    assert.equal(source.ws.messages.at(-1).enabled, false);
}

// Decoder capability and bandwidth are separate. A weak viewer changes layer
// without changing either a fast viewer's profile or the shared source epoch.
{
    const c = context(), source = c.client('source'), input = c.connect(source, 'publish');
    const fast = c.client('fast'), fastWs = c.connect(fast, 'view'), a = c.subscribe(fast, source);
    const slow = c.client('slow'), slowWs = c.connect(slow, 'view'), b = c.subscribe(slow, source);
    const publication = c.shared.byClient.get(source);
    input.emit('message', frame(publication), true);
    ack(c, fast, fastWs, media(fastWs)[0]); ack(c, slow, slowWs, media(slowWs)[0]);
    const report = () => c.relay.handleAcknowledgement(slow, slowWs, Buffer.from(JSON.stringify({
        type: 'screen_view_feedback', remoteSessionId: b.session.remoteSessionId, generation: b.entry.generation,
        streamId: b.entry.streamId, screenId: 0, decodeMs: 250, droppedFrames: 1 })));
    assert.equal(report(), true); c.tick(500); assert.equal(report(), true);
    assert.equal(c.shared.receiver(fastWs).targetBps, 3_000_000);
    assert.equal(c.shared.receiver(slowWs).targetBps, 1_470_000);
    assert.deepEqual(source.ws.messages.filter(message => message.type === 'screen_publication_request').at(-1).screens[0].layers, ['main', 'low']);
    input.emit('message', frame(publication, { layer: 'low', width: 960, height: 540, bitrateBps: 750000 }), true);
    assert.equal(media(slowWs).at(-1).width, 960);
    assert.equal(media(slowWs).at(-1).sequence, 2);
    assert.equal(media(fastWs).length, 1);
    input.emit('message', frame(publication, { sequence: 2, keyFrame: false }), true);
    input.emit('message', frame(publication, { layer: 'low', sequence: 2, keyFrame: false,
        width: 960, height: 540, bitrateBps: 750000 }), true);
    assert.equal(media(fastWs).at(-1).width, 1920);
    assert.equal(media(fastWs).at(-1).sequence, 2);
    assert.equal(media(slowWs).at(-1).sequence, 3);
    assert.equal(acknowledgements(input).length, 4);
    assert.equal(input.messages.some(message => message.type === 'screen_share_feedback'), false);
    assert.equal(a.entry.enabled, true); assert.equal(b.entry.enabled, true);
}

// A cached IDR can display an immediate preview for a late joiner, but delivery
// numbering must never conceal missing source references before the next P.
{
    const c = context(), source = c.client('source'), input = c.connect(source, 'publish');
    const first = c.client('first'), firstWs = c.connect(first, 'view'); c.subscribe(first, source);
    const publication = c.shared.byClient.get(source);
    input.emit('message', frame(publication), true);
    for (let sequence = 2; sequence <= 4; ++sequence)
        input.emit('message', frame(publication, { sequence, keyFrame: false }), true);
    const late = c.client('late'), lateWs = c.connect(late, 'view'), { entry } = c.subscribe(late, source);
    c.relay.sweep();
    assert.equal(media(lateWs).length, 1);
    input.emit('message', frame(publication, { sequence: 5, keyFrame: false }), true);
    assert.equal(media(firstWs).length, 5);
    assert.equal(media(lateWs).length, 1, 'source P5 depends on missing P2–4 despite local delivery sequence 2');
    assert.equal(entry.screens.get(0).needsKeyframe, true);
    input.emit('message', frame(publication, { sequence: 6 }), true);
    input.emit('message', frame(publication, { sequence: 7, keyFrame: false }), true);
    assert.deepEqual(media(lateWs).map(metadata => metadata.sequence), [1, 2, 3]);
    assert.equal(media(lateWs)[1].keyFrame, true);
    assert.equal(media(lateWs)[2].keyFrame, false);
}

// Legacy viewers require a compatible layer. Optional low failure constrains
// main only for a hard decoder limit, not for another viewer's slow network.
{
    const c = context(), source = c.client('source'), input = c.connect(source, 'publish');
    const modern = c.client('modern'), modernWs = c.connect(modern, 'view'); c.subscribe(modern, source);
    const legacy = c.client('legacy'), legacyWs = c.connect(legacy); c.subscribe(legacy, source);
    const publication = c.shared.byClient.get(source);
    input.emit('message', frame(publication, { width: 3840, height: 2160 }), true);
    assert.equal(media(modernWs).length, 1); assert.equal(media(legacyWs).length, 0);
    assert.equal(c.relay.handleControl(source.id, { type: 'screen_publication_status', publicationId: publication.publicationId,
        screenId: 0, layer: 'low', reason: 'inactive' }), true);
    const request = source.ws.messages.filter(message => message.type === 'screen_publication_request').at(-1);
    assert.equal(request.publicationId, publication.publicationId);
    assert.equal(request.screens[0].maximumEdge, 1920);
    input.emit('message', frame(publication, { sequence: 2 }), true);
    assert.equal(media(legacyWs).at(-1).width, 1920);
}

// Separate role capabilities survive concurrent token requests and isolate a
// failed download from that same endpoint's healthy publication.
{
    const c = context(), both = c.client('both');
    const viewToken = c.token(both, 'view'), publishToken = c.token(both, 'publish');
    const view = socket(), publish = socket();
    c.relay.acceptSocket(view, viewToken.token); c.relay.acceptSocket(publish, publishToken.token);
    assert.equal(view.messages[0].role, 'view'); assert.equal(publish.messages[0].role, 'publish');
    const source = c.client('source'), input = c.connect(source, 'publish'), inbound = c.subscribe(both, source);
    const observer = c.client('observer'), output = c.connect(observer, 'view'), outbound = c.subscribe(observer, both);
    input.emit('message', frame(c.shared.byClient.get(source)), true);
    view.bufferedAmount = c.relay.maxBufferedBytes + 1;
    publish.emit('message', frame(c.shared.byClient.get(both)), true);
    assert.equal(media(output).length, 1); assert.equal(acknowledgements(publish).length, 1);
    ack(c, observer, output, media(output)[0]);
    c.tick(c.relay.ackTimeoutMs); c.relay.sweep();
    assert.equal(view.readyState, 3); assert.equal(publish.readyState, 1);
    assert.equal(inbound.entry.enabled, false); assert.equal(outbound.entry.enabled, true);
    assert.equal(both.ws.readyState, 1);
}

// Snapshot mode sends only a newer retained IDR, no repeated fake freshness;
// ACKs and configured spacing release bounded credit independently per viewer.
{
    const c = context({ screenViewerInitialBps: 64000 }), source = c.client('source'), input = c.connect(source, 'publish');
    const owner = c.client('owner'), output = c.connect(owner, 'view'), { entry } = c.subscribe(owner, source);
    const publication = c.shared.byClient.get(source);
    input.emit('message', frame(publication, { bitrateBps: 750000 }), true);
    input.emit('message', frame(publication, { sequence: 2 }), true);
    assert.equal(media(output).length, 1, 'at most one snapshot in flight for one display');
    c.tick(1000); c.relay.sweep(); assert.equal(media(output).length, 1);
    ack(c, owner, output, media(output)[0]); c.relay.sweep();
    assert.equal(media(output).length, 2);
    assert.equal(media(output)[1].sequence, 2);
    ack(c, owner, output, media(output)[1]); c.tick(1000); c.relay.sweep();
    assert.equal(media(output).length, 2, 'the same IDR is never rendered again as a new delivery');
    c.tick(c.shared.cacheTtlMs); c.relay.sweep();
    assert.equal(c.shared.cacheBytes, 0);
    assert.equal(c.shared.cache.size, 0);
    assert.equal(entry.enabled, true);
}

// A useful IDR taking longer than the ordinary receipt deadline receives a
// bounded serialization allowance. Its debt cannot be forgotten or bypassed.
{
    const c = context({ screenViewerInitialBps: 64000 }), source = c.client('source'), input = c.connect(source, 'publish');
    const owner = c.client('owner'), output = c.connect(owner, 'view'); c.subscribe(owner, source);
    const publication = c.shared.byClient.get(source);
    input.emit('message', frame(publication, {}, 28000), true);
    assert.equal(media(output).length, 1);
    const pending = [...c.relay.window(output).frames.values()][0];
    assert.ok(pending.serializationMs > 3000);
    assert.ok(pending.deadlineAt > 6000);
    const requested = source.ws.messages.filter(message => message.type === 'screen_publication_keyframe').length;
    c.tick(3100); c.relay.sweep();
    assert.equal(output.readyState, 1, 'a valid long snapshot must survive the ordinary three-second receipt timeout');
    assert.equal(source.ws.messages.filter(message => message.type === 'screen_publication_keyframe').length, requested,
        'an in-flight snapshot does not request unused replacement IDRs');
    input.emit('message', frame(publication, { sequence: 2 }, 28000), true);
    assert.equal(media(output).length, 1);
    c.tick(700); ack(c, owner, output, media(output)[0]); c.relay.sweep();
    assert.equal(media(output).length, 2);
    assert.equal(output.readyState, 1);
    c.tick(8000); c.relay.sweep();
    assert.equal(output.readyState, 3, 'the serialization allowance remains a finite deadline');
}

// An impossible snapshot cannot occupy the fairness head indefinitely and
// prevent a smaller, healthy screen from using the same endpoint.
{
    const c = context({ screenViewerInitialBps: 64000 }), source = c.client('source', [
        { id: 0, width: 1920, height: 1080 }, { id: 1, width: 1920, height: 1080 }]);
    const input = c.connect(source, 'publish'), owner = c.client('owner'), output = c.connect(owner, 'view'); c.subscribe(owner, source);
    const publication = c.shared.byClient.get(source);
    input.emit('message', frame(publication, {}, 100000), true);
    assert.equal(media(output).length, 0);
    assert.equal(c.relay.window(output).waiting.size, 0);
    input.emit('message', frame(publication, { screenId: 1 }), true);
    assert.equal(media(output).length, 1);
    assert.equal(media(output)[0].screenId, 1);
}

// Equal low-bandwidth snapshot streams rotate fairly even when each image
// takes longer to serialize than the nominal snapshot interval.
{
    const c = context({ screenViewerInitialBps: 64000, screenViewerMaxBps: 64000 }), source = c.client('source', [
        { id: 0, width: 1920, height: 1080 }, { id: 1, width: 1920, height: 1080 }]);
    const input = c.connect(source, 'publish'), owner = c.client('owner'), output = c.connect(owner, 'view'); c.subscribe(owner, source);
    const publication = c.shared.byClient.get(source), received = [0, 0], receipts = [];
    let delivered = 0;
    for (let elapsed = 0; elapsed <= 12000; elapsed += 100) {
        for (let i = receipts.length - 1; i >= 0; --i) {
            if (receipts[i].at <= elapsed) { ack(c, owner, output, receipts[i].metadata); receipts.splice(i, 1); }
        }
        if (elapsed % 500 === 0)
            for (let screenId = 0; screenId < 2; ++screenId)
                input.emit('message', frame(publication, { screenId, sequence: elapsed / 500 + 1 }, 12000), true);
        c.relay.sweep();
        const all = media(output);
        while (delivered < all.length) {
            const metadata = all[delivered++]; received[metadata.screenId]++;
            receipts.push({ at: elapsed + 1600, metadata });
        }
        c.tick(100);
    }
    assert.ok(received.every(count => count >= 3), `both snapshot screens progress: ${received}`);
    assert.ok(Math.abs(received[0] - received[1]) <= 1, `fair rotation: ${received}`);
}

// A disposable receive-channel reconnection must not reset a learned slow
// path to the initial aggressive bitrate and repeat the same timeout forever.
{
    const c = context(), source = c.client('source'); c.connect(source, 'publish');
    const owner = c.client('owner'), output = c.connect(owner, 'view'), { entry } = c.subscribe(owner, source);
    for (let i = 0; i < 5; ++i) { c.shared.recordFeedback(entry, 0, { congested: true }); c.tick(500); }
    const learned = c.shared.receiver(output).targetBps;
    assert.ok(learned < 600000);
    const replacement = c.connect(owner, 'view');
    assert.equal(c.shared.receiver(replacement).targetBps, learned);
    assert.equal(c.relay.window(replacement).baselineRttMs, null);
    assert.equal(output.readyState, 3);
    assert.equal(c.shared.receiver(replacement).nextSendAt, 0);
}

// Global egress is paced across independent receivers; fabricated low bitrate
// hints never authorize sending more bytes than the configured aggregate cap.
{
    const c = context({ screenServerEgressBps: 128000 }), source = c.client('source'), input = c.connect(source, 'publish');
    const viewers = ['a', 'b'].map(name => {
        const owner = c.client(name), output = c.connect(owner, 'view'); c.subscribe(owner, source); return { owner, output };
    });
    const publication = c.shared.byClient.get(source);
    for (let sequence = 1; sequence <= 100; ++sequence) {
        input.emit('message', frame(publication, { sequence, bitrateBps: 10000 }, 1000), true);
        for (const { owner, output } of viewers) {
            const latest = media(output).at(-1); if (latest) ack(c, owner, output, latest);
        }
        c.tick(20);
    }
    const bytes = viewers.reduce((sum, viewer) => sum + viewer.output.messages.filter(Buffer.isBuffer)
        .reduce((total, packet) => total + packet.length, 0), 0);
    assert.ok(bytes <= 128000 * 2.15 / 8 + 1500, `bounded pacing burst, observed ${bytes}`);
    for (const viewer of viewers) assert.ok(media(viewer.output).length >= 5);
}

// Global retention accounts for the entire backing packet (including header),
// evicts oldest IDRs, and is purged by consent and topology epochs.
{
    const c = context(), source = c.client('source'), input = c.connect(source, 'publish');
    const owner = c.client('owner'); c.connect(owner, 'view'); c.subscribe(owner, source);
    const publication = c.shared.byClient.get(source);
    c.shared.cacheLimit = 1000;
    const data = frame(publication, {}, 600); input.emit('message', data, true);
    assert.equal(c.shared.cacheBytes, data.length);
    const retained = [...c.shared.cache.values()][0];
    assert.equal(retained.payload.buffer.byteLength, retained.payload.length,
        'a small cached payload must not pin a larger shared Node allocation slab');
    c.relay.handleControl(source.id, { type: 'screen_share_consent', enabled: false });
    assert.equal(c.shared.cacheBytes, 0); assert.equal(c.shared.byClient.size, 0);
    input.emit('message', data, true);
    assert.equal(acknowledgements(input).length, 1, 'revoked publication produces no source receipt');
    c.relay.handleControl(source.id, { type: 'screen_share_consent', enabled: true });
    const next = c.shared.byClient.get(source);
    assert.notEqual(next.publicationId, publication.publicationId);
    source.screens = [{ id: 0, width: 1920, height: 1080 }]; c.relay.refreshForClient(source);
    assert.notEqual(c.shared.byClient.get(source).publicationId, next.publicationId);
}

// Both role and immutable authority are checked before any source receipt.
{
    const c = context(), source = c.client('source'), input = c.connect(source, 'publish');
    const owner = c.client('owner'), output = c.connect(owner, 'view'); c.subscribe(owner, source);
    const publication = c.shared.byClient.get(source), data = frame(publication);
    const stranger = c.client('stranger'), strangerInput = c.connect(stranger, 'publish');
    strangerInput.emit('message', data, true);
    assert.equal(acknowledgements(strangerInput).length, 0);
    input.emit('message', frame(publication, { publicationId: crypto.randomUUID() }), true);
    assert.equal(acknowledgements(input).length, 0);
    output.emit('message', data, true); assert.equal(output.readyState, 3);
    for (const extra of [{ layer: 'forged' }, { sequence: 0 }, { width: 3842 }, { width: 641 },
        { fps: 121 }, { bitrateBps: 400000001 }, { timestampUs: -1 }, { extra: true }])
        assert.equal(parsePublicationFrame(frame(publication, extra)), null);
    const forgedKeyframe = Buffer.from(data); forgedKeyframe[forgedKeyframe.length - 100 + 4] = 0x61;
    assert.equal(parsePublicationFrame(forgedKeyframe), null, 'cache requires SPS, PPS and IDR, not only a trusted flag');
}

// One endpoint's budget is shared across its visible displays and source
// publications. Upload reservation reduces only that endpoint's download path.
{
    const c = context(), owner = c.client('owner'), output = c.connect(owner, 'view');
    for (let i = 0; i < 2; ++i) {
        const source = c.client(`source-${i}`, [{ id: 0, width: 1920, height: 1080 }, { id: 1, width: 1920, height: 1080 }]);
        c.connect(source, 'publish'); c.subscribe(owner, source);
    }
    assert.equal(c.shared.screenCount(output), 4);
    assert.equal(c.shared.budget(output), 3_000_000);
    c.server.uploads.set('active', { targetEndpointId: owner.endpointId });
    assert.equal(c.shared.budget(output), 1_200_000);
    for (const publication of c.shared.byClient.values())
        for (const screen of publication.screens.values()) assert.deepEqual(screen.demand.layers, ['main', 'low']);
}

// Explicit rollout disable negotiates the legacy combined channel, even when
// a modern peer offered split-role capabilities.
{
    const c = context({ screenSharedEnabled: false }), client = c.client('legacy-mode');
    const reply = c.token(client, 'view'); assert.equal(reply.role, undefined);
    const ws = socket(); c.relay.acceptSocket(ws, reply.token);
    assert.equal(ws.messages[0].mediaVersion, undefined); assert.equal(ws.messages[0].role, undefined);
    assert.equal(c.relay.publisherSocket(client), ws);
}

console.log('screen_publications: all tests passed');
