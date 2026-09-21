'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');
const { PROTOCOL_VERSION, endpointIdForInstallation } = require('./device_auth');
const { MAX_PROFILE_PICTURE_BYTES, normalizeClientProfile, isProfilePictureJpeg } = require('./client_profile');

// A real, encoder-produced 250x250 RGB JPEG. Tests also mutate its markers to
// cover content validation independently from the extension or claimed hash.
const jpegBase64 = '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAYEBQYFBAYGBQYHBwYIChAKCgkJChQODwwQFxQYGBcUFhYaHSUfGhsjHBYWICwgIyYnKSopGR8tMC0oMCUoKSj/2wBDAQcHBwoIChMKChMoGhYaKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCj/wAARCAD6APoDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAb/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/8QAFgEBAQEAAAAAAAAAAAAAAAAAAAUG/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACEQMRAD8AhgGqQgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAH/9k=';
const jpeg = Buffer.from(jpegBase64, 'base64');
const hash = crypto.createHash('sha256').update(jpeg).digest('hex');

function context() {
    let now = 1000;
    return { server: new MouffetteServer({ port: 0, protocolLogger: null,
        metricLogger: () => {}, monotonicNow: () => now, epochNow: () => now }),
        now: () => now, advance: ms => { now += ms; } };
}
function peer(ctx) {
    const installationId = crypto.randomBytes(32).toString('base64url');
    const client = {
        id: crypto.randomUUID(), installationId,
        endpointId: endpointIdForInstallation(installationId, 'primary'),
        instanceId: 'primary', instanceOrdinal: 1, runtimeId: crypto.randomUUID(),
        connectionGeneration: ++ctx.server.connectionGenerationSequence,
        authenticated: true, lastHeartbeatAt: ctx.now(), lastHeartbeatMonotonicAt: ctx.now(),
        ws: { readyState: WebSocket.OPEN, bufferedAmount: 0, messages: [],
            send(raw) { this.messages.push(JSON.parse(raw)); },
            close() { this.readyState = WebSocket.CLOSED; } },
    };
    ctx.server.clients.set(client.id, client);
    ctx.server.currentTransportByEndpoint.set(client.endpointId, client);
    return client;
}
function envelope(ctx, client, type, fields = {}) {
    return { type, protocolVersion: PROTOCOL_VERSION, serverBootId: ctx.server.serverBootId,
        messageId: crypto.randomUUID(), connectionGeneration: client.connectionGeneration, ...fields };
}
function snapshot(ctx, client, profile = {}) {
    ctx.server.handleMessage(client.id, envelope(ctx, client, 'endpoint_snapshot', {
        requestId: crypto.randomUUID(), instanceOrdinal: 1, machineName: 'Hostname',
        platform: 'test', screens: [], systemUI: [], volumePercent: null, ...profile,
    }));
}
function latest(client, type) {
    return client.ws.messages.filter(message => message.type === type).at(-1);
}
function request(ctx, requester, owner, requestedHash = hash, extra = {}) {
    const requestId = crypto.randomUUID();
    ctx.server.handleMessage(requester.id, envelope(ctx, requester, 'profile_picture_request', {
        requestId, endpointId: owner.endpointId, profilePictureHash: requestedHash, ...extra,
    }));
    return requestId;
}

assert.equal(isProfilePictureJpeg(jpeg), true);
assert.deepEqual(normalizeClientProfile({}), { username: '', profilePictureJpeg: '', profilePictureHash: '' });
assert.deepEqual(normalizeClientProfile({ username: '  Zoé 🎬  ', profilePictureJpeg: jpegBase64 }),
    { username: 'Zoé 🎬', profilePictureJpeg: jpegBase64, profilePictureHash: hash });
assert.equal(normalizeClientProfile({ username: '🎬'.repeat(64) }).error, undefined);
assert.equal(normalizeClientProfile({ username: `  ${'🎬'.repeat(64)}  ` }).username, '🎬'.repeat(64));
assert.equal(normalizeClientProfile({ username: ' '.repeat(80) }).username, '');
for (const username of [null, false, 12, {}, 'a'.repeat(65), '🎬'.repeat(65), 'x\ny', '\t', '\u007f', '\u0085']) {
    assert.ok(normalizeClientProfile({ username }).error);
}
const wrongSize = Buffer.from(jpeg);
const frameOffset = wrongSize.indexOf(Buffer.from([0xff, 0xc0]));
assert.ok(frameOffset > 0);
wrongSize.writeUInt16BE(251, frameOffset + 7);
for (const profilePictureJpeg of [null, false, {}, 'not base64', `${jpegBase64}\n`,
    jpeg.toString('base64url'), wrongSize.toString('base64'), jpeg.subarray(0, -2).toString('base64'),
    Buffer.concat([jpeg, Buffer.from('trailing')]).toString('base64'),
    Buffer.alloc(MAX_PROFILE_PICTURE_BYTES + 1).toString('base64'), 'a'.repeat(200000)]) {
    assert.ok(normalizeClientProfile({ profilePictureJpeg }).error);
}

// Profiles never become identity or durable/offline state; list packets stay
// small and image requests are correlated to an authenticated transport.
{
    const ctx = context();
    const owner = peer(ctx);
    const observer = peer(ctx);
    snapshot(ctx, observer);
    snapshot(ctx, owner, { username: 'Alice', profilePictureJpeg: jpegBase64 });
    const endpointId = owner.endpointId;
    const presence = () => ctx.server.presenceEntries().find(entry => entry.endpointId === endpointId);
    assert.equal(presence().username, 'Alice');
    assert.equal(presence().profilePictureHash, hash);
    assert.equal(Object.hasOwn(presence(), 'profilePictureJpeg'), false);
    assert.equal(latest(owner, 'endpoint_snapshot_applied').snapshot.username, 'Alice');
    assert.equal(Object.hasOwn(latest(owner, 'endpoint_snapshot_applied').snapshot, 'profilePictureJpeg'), false);
    const retained = ctx.server.endpointPresence.get(endpointId);
    for (const field of ['username', 'profilePictureHash', 'profilePictureJpeg']) {
        assert.equal(Object.hasOwn(retained, field), false);
    }
    const revision = ctx.server.presenceRevision;
    const requestId = request(ctx, observer, owner);
    const response = latest(observer, 'profile_picture_response');
    assert.equal(response.requestId, requestId);
    assert.equal(response.endpointId, endpointId);
    assert.equal(response.profilePictureHash, hash);
    assert.equal(response.profilePictureJpeg, jpegBase64);
    assert.equal(response.serverBootId, ctx.server.serverBootId);
    assert.equal(response.connectionGeneration, observer.connectionGeneration);
    assert.equal(ctx.server.presenceRevision, revision);

    snapshot(ctx, owner, { username: 'Bob', profilePictureJpeg: jpegBase64 });
    assert.equal(presence().username, 'Bob');
    assert.equal(owner.endpointId, endpointId);
    assert.ok(ctx.server.presenceRevision > revision);
    snapshot(ctx, owner, { username: 'Rejected', profilePictureJpeg: wrongSize.toString('base64') });
    assert.equal(latest(owner, 'error').code, 'invalid_endpoint_snapshot');
    assert.equal(presence().username, 'Bob');
    assert.equal(owner.profilePictureJpeg, jpegBase64);

    request(ctx, observer, owner, 'f'.repeat(64));
    assert.equal(latest(observer, 'profile_picture_response').profilePictureJpeg, '');
    snapshot(ctx, owner, { username: '', profilePictureJpeg: '' });
    assert.equal(presence().username, '');
    assert.equal(presence().profilePictureHash, '');
    request(ctx, observer, owner);
    assert.equal(latest(observer, 'profile_picture_response').profilePictureJpeg, '');

    snapshot(ctx, owner, { username: 'Alice', profilePictureJpeg: jpegBase64 });
    ctx.server.retireClientTransport(owner, true, 1000, 'test');
    assert.equal(presence().status, 'Disconnected');
    for (const field of ['username', 'profilePictureHash', 'profilePictureJpeg']) {
        assert.equal(Object.hasOwn(presence(), field), false);
        assert.equal(Object.hasOwn(owner, field), false);
    }
    request(ctx, observer, owner);
    assert.equal(latest(observer, 'profile_picture_response').profilePictureJpeg, '');
    const restarted = context();
    assert.equal(restarted.server.endpointPresence.size, 0);
    assert.equal(restarted.server.clients.size, 0);
}

// Unusable live transports also conceal metadata until a fresh heartbeat.
{
    const ctx = context();
    const owner = peer(ctx);
    const observer = peer(ctx);
    snapshot(ctx, owner, { username: 'Alice', profilePictureJpeg: jpegBase64 });
    snapshot(ctx, observer);
    ctx.advance(ctx.server.config.leaseTimeoutMs);
    observer.lastHeartbeatMonotonicAt = ctx.now();
    observer.lastHeartbeatAt = ctx.now();
    let presence = ctx.server.presenceEntries().find(entry => entry.endpointId === owner.endpointId);
    assert.equal(Object.hasOwn(presence, 'username'), false);
    request(ctx, observer, owner);
    assert.equal(latest(observer, 'profile_picture_response').profilePictureJpeg, '');
    owner.lastHeartbeatMonotonicAt = ctx.now();
    owner.lastHeartbeatAt = ctx.now();
    presence = ctx.server.presenceEntries().find(entry => entry.endpointId === owner.endpointId);
    assert.equal(presence.username, 'Alice');
    owner.draining = true;
    presence = ctx.server.presenceEntries().find(entry => entry.endpointId === owner.endpointId);
    assert.equal(Object.hasOwn(presence, 'username'), false);
    request(ctx, observer, owner);
    assert.equal(latest(observer, 'profile_picture_response').profilePictureJpeg, '');
}

// An older client omitting the optional fields clears its previous profile,
// while malformed requests and stale/unauthenticated envelopes cannot read it.
{
    const ctx = context();
    const owner = peer(ctx);
    const observer = peer(ctx);
    snapshot(ctx, owner, { username: 'Alice', profilePictureJpeg: jpegBase64 });
    snapshot(ctx, owner);
    assert.equal(owner.username, '');
    assert.equal(owner.profilePictureHash, '');
    snapshot(ctx, owner, { username: 'Alice', profilePictureJpeg: jpegBase64 });
    request(ctx, observer, owner, 'bad');
    assert.equal(latest(observer, 'error').code, 'invalid_profile_picture_request');
    request(ctx, observer, owner, hash, { connectionGeneration: observer.connectionGeneration + 1 });
    assert.equal(latest(observer, 'error').code, 'stale_connection_generation');
    request(ctx, observer, owner, hash, { serverBootId: crypto.randomUUID() });
    assert.equal(latest(observer, 'error').code, 'server_boot_mismatch');
    observer.authenticated = false;
    request(ctx, observer, owner);
    assert.equal(latest(observer, 'error').code, 'authentication_required');
    assert.equal(latest(observer, 'profile_picture_response'), undefined);
}

console.log('client profile protocol tests passed');
