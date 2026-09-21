'use strict';

const crypto = require('node:crypto');
const WebSocket = require('ws');

const MAX_FRAME_BYTES = 2 * 1024 * 1024;
const MAX_HEADER_BYTES = 1024;
const MAX_BUFFERED_BYTES = 512 * 1024;
const MAX_INFLIGHT_FRAMES = 3;
const ACK_TIMEOUT_MS = 1500;
const frameKey = frame => `${frame.streamId}:${frame.screenId}:${frame.sequence}`;
const MAGIC = Buffer.from('MSV1');
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const opaque = value => typeof value === 'string' && /^[A-Za-z0-9_-]{1,128}$/.test(value);
const integer = (value, min, max = Number.MAX_SAFE_INTEGER) => Number.isSafeInteger(value) && value >= min && value <= max;
const FRAME_KEYS = new Set(['remoteSessionId', 'generation', 'streamId', 'screenId',
    'sequence', 'width', 'height', 'keyFrame', 'codec', 'timestampUs']);

// One complete Annex-B access unit per message. No compression, transcoding,
// frame retention or base64: the relay validates a small header and forwards
// the original buffer. streamId fences independent control/video TCP queues.
function parseScreenFrame(data) {
    if (!Buffer.isBuffer(data) || data.length < 8
        || data.length > MAX_FRAME_BYTES + MAX_HEADER_BYTES + 6
        || !data.subarray(0, 4).equals(MAGIC)) return null;
    const length = data.readUInt16BE(4);
    if (length < 2 || length > MAX_HEADER_BYTES || data.length <= length + 6
        || data.length - length - 6 > MAX_FRAME_BYTES) return null;
    let header;
    try { header = JSON.parse(data.subarray(6, length + 6).toString('utf8')); }
    catch (_) { return null; }
    if (!header || Array.isArray(header) || typeof header !== 'object'
        || ![9, 10].includes(Object.keys(header).length)
        || Object.keys(header).some(key => !FRAME_KEYS.has(key))
        || !opaque(header.remoteSessionId) || !integer(header.generation, 1)
        || !UUID.test(header.streamId || '') || !integer(header.screenId, 0, 1_000_000)
        || !integer(header.sequence, 1) || !integer(header.width, 2, 1920) || header.width % 2 !== 0
        || !integer(header.height, 2, 1920) || header.height % 2 !== 0
        || (header.timestampUs !== undefined && !integer(header.timestampUs, 0))
        || typeof header.keyFrame !== 'boolean' || header.codec !== 'h264') return null;
    // Annex B starts with a three/four-byte start code; AVCC is not accepted.
    const payload = data.subarray(length + 6);
    if (payload.length < 5 || payload[0] !== 0 || payload[1] !== 0
        || !(payload[2] === 1 || (payload[2] === 0 && payload[3] === 1))) return null;
    return header;
}

class ScreenShareRelay {
    constructor(server) {
        this.server = server;
        this.tokens = new Map();
        this.sockets = new Map(); // authenticated control object -> video socket
        this.inflight = new Map(); // video socket -> bounded receipt window
        this.subscriptions = new Map(); // RemoteSession -> ephemeral stream grant
    }

    current(client) {
        return !!client && client.authenticated && !client.draining && !client.replaced
            && this.server.clients.get(client.id) === client
            && this.server.currentTransportByEndpoint.get(client.endpointId) === client
            && client.ws?.readyState === WebSocket.OPEN
            && !this.server.clientLeaseExpired(client);
    }

    issueToken(clientId, requestId) {
        const client = this.server.clients.get(clientId);
        if (!this.current(client) || !opaque(requestId)) return false;
        const now = this.server.monotonicNow();
        for (const [token, binding] of this.tokens) {
            if (binding.client === client || binding.expiresAt <= now) this.tokens.delete(token);
        }
        const token = crypto.randomBytes(32).toString('base64url');
        this.tokens.set(token, { client, expiresAt: now + 15_000 });
        return this.server.sendToEndpoint(client.endpointId, {
            type: 'screen_channel_token', requestId, token,
        });
    }

    consumeToken(token) {
        if (typeof token !== 'string' || token.length !== 43) return null;
        const binding = this.tokens.get(token);
        this.tokens.delete(token);
        return binding && binding.expiresAt > this.server.monotonicNow()
            && this.current(binding.client) ? binding.client : null;
    }

    acceptSocket(ws, token) {
        const client = this.consumeToken(token);
        if (!client) { ws.close(1008, 'Screen channel authentication failed'); return; }
        const previous = this.sockets.get(client);
        if (previous) {
            this.sockets.delete(client);
            this.refreshForClient(client);
            previous.close(1008, 'Screen channel replaced');
        }
        ws.mouffetteClient = client;
        this.sockets.set(client, ws);
        ws.on('message', (data, isBinary) => {
            if (!this.current(client) || this.sockets.get(client) !== ws) {
                ws.close(1008, 'Stale screen channel'); return;
            }
            if (!isBinary) {
                if (!this.handleAcknowledgement(client, ws, data)) ws.close(1008, 'Invalid screen acknowledgement');
                return;
            }
            const frame = parseScreenFrame(data);
            if (!frame) {
                ws.close(1008, 'Invalid screen frame');
                return;
            }
            if ((Number(ws.bufferedAmount) || 0) > MAX_BUFFERED_BYTES) {
                if (typeof ws.terminate === 'function') ws.terminate();
                else ws.close(1008, 'Screen receipt congestion');
                return;
            }
            // Receipt acknowledges transport consumption, including a dropped
            // stale/congested frame. The receiver still needs an IDR after loss.
            ws.send(JSON.stringify({ type: 'screen_frame_ack', streamId: frame.streamId,
                screenId: frame.screenId, sequence: frame.sequence }));
            this.handleFrame(client, ws, data);
        });
        const lost = () => {
            this.inflight.delete(ws);
            if (this.sockets.get(client) !== ws) return;
            this.sockets.delete(client);
            this.refreshForClient(client);
        };
        ws.on('close', lost);
        ws.on('error', lost);
        ws.send(JSON.stringify({ type: 'screen_channel_ready', endpointId: client.endpointId,
            protocolVersion: this.server.protocolVersion, serverBootId: this.server.serverBootId,
            messageId: crypto.randomUUID(), connectionGeneration: client.connectionGeneration }));
        this.refreshForClient(client);
    }

    window(ws) {
        let window = this.inflight.get(ws);
        if (!window) { window = { frames: new Map(), waiting: new Map(), bytes: 0 }; this.inflight.set(ws, window); }
        return window;
    }

    handleAcknowledgement(client, ws, data) {
        if (!this.current(client) || this.sockets.get(client) !== ws || data.length > 512) return false;
        let ack;
        try { ack = JSON.parse(data.toString()); } catch (_) { return false; }
        if (!ack || typeof ack !== 'object' || Array.isArray(ack) || Object.keys(ack).length !== 4
            || ack.type !== 'screen_frame_ack' || !UUID.test(ack.streamId || '')
            || !integer(ack.screenId, 0, 1_000_000) || !integer(ack.sequence, 1)) return false;
        const window = this.inflight.get(ws);
        const key = frameKey(ack), pending = window?.frames.get(key);
        // Duplicates and delayed receipts release no other stream's credit.
        if (pending) { window.bytes -= pending.bytes; window.frames.delete(key); }
        return true;
    }

    sweep() {
        const now = this.server.monotonicNow();
        for (const [ws, window] of this.inflight) {
            if ([...window.frames.values()].some(frame => now - frame.sentAt >= ACK_TIMEOUT_MS)) {
                this.inflight.delete(ws);
                // close() would put a close frame behind the stale TCP queue.
                // Terminate the disposable pipe immediately to shed that queue.
                if (typeof ws.terminate === 'function') ws.terminate();
                else ws.close(1008, 'Screen receipt timeout');
            }
        }
    }

    endpointClient(endpointId) {
        return this.server.currentTransportByEndpoint.get(endpointId);
    }

    socket(client) {
        const ws = this.sockets.get(client);
        return this.current(client) && ws?.readyState === WebSocket.OPEN ? ws : null;
    }

    envelope(entry, type, extra = {}) {
        return { type, remoteSessionId: entry.session.remoteSessionId,
            generation: entry.generation, streamId: entry.streamId || '', ...extra };
    }

    publish(entry, enabled, reason) {
        const changed = entry.enabled !== enabled;
        if (changed) {
            const previousStreamId = entry.streamId;
            for (const window of this.inflight.values()) {
                for (const key of window.waiting.keys())
                    if (previousStreamId && key.startsWith(`${previousStreamId}:`)) window.waiting.delete(key);
                for (const [key, frame] of window.frames) {
                    if (previousStreamId && key.startsWith(`${previousStreamId}:`)) {
                        window.bytes -= frame.bytes;
                        window.frames.delete(key);
                    }
                }
            }
            entry.enabled = enabled;
            entry.streamId = enabled ? crypto.randomUUID() : '';
            entry.screens.clear();
            entry.status = '';
        }
        // Repeated subscriptions act as bounded state queries, allowing an
        // owner that was not ready to apply the first status to recover.
        this.server.sendToEndpoint(entry.session.ownerEndpointId,
            this.envelope(entry, 'screen_share_state', { enabled, reason: enabled && entry.status ? entry.status : reason }));
        this.server.sendToEndpoint(entry.session.targetEndpointId,
            this.envelope(entry, 'screen_share_request', { enabled, reason }));
    }

    refresh(entry) {
        const session = this.server.remoteSessions.get(entry.session.remoteSessionId);
        const owner = session && this.endpointClient(session.ownerEndpointId);
        const target = session && this.endpointClient(session.targetEndpointId);
        const signature = JSON.stringify(session?.latestTargetSnapshot?.snapshot.screens || target?.screens || []);
        if (!session?.pendingTargetSnapshot) entry.waitingForTopology = false;
        else if (entry.topology !== signature) entry.waitingForTopology = true;
        let reason = 'ready';
        if (!session || entry.generation !== session.generation
            || !this.server.remoteSessions.commandReady(session)) reason = 'session_unavailable';
        else if (!this.current(owner) || !this.current(target)
            || owner.connectionGeneration !== session.ownerConnectionGeneration
            || target.connectionGeneration !== session.targetConnectionGeneration
            || owner.runtimeId !== session.ownerRuntimeId || target.runtimeId !== session.targetRuntimeId) reason = 'session_unavailable';
        else if (target.screenSharingEnabled !== true) reason = 'disabled';
        else if (entry.waitingForTopology
            || (session.latestTargetSnapshot?.generation === session.generation
                && JSON.stringify(session.latestTargetSnapshot.snapshot.screens) !== JSON.stringify(target.screens))) reason = 'topology_pending';
        else if (!this.socket(owner) || !this.socket(target)) reason = 'channel_unavailable';
        if (entry.enabled && entry.topology !== signature) this.publish(entry, false, 'topology_changed');
        entry.topology = signature;
        this.publish(entry, reason === 'ready', reason);
    }

    refreshForClient(client) {
        for (const entry of this.subscriptions.values()) {
            if (entry.session.ownerEndpointId === client.endpointId
                || entry.session.targetEndpointId === client.endpointId) this.refresh(entry);
        }
    }

    refreshSession(session) {
        const entry = this.subscriptions.get(session.remoteSessionId);
        if (entry) this.refresh(entry);
    }

    handleControl(clientId, message) {
        const client = this.server.clients.get(clientId);
        if (!this.current(client)) return false;
        if (message.type === 'request_screen_channel') return this.issueToken(clientId, message.requestId);
        if (message.type === 'screen_share_consent') {
            if (typeof message.enabled !== 'boolean') return false;
            client.screenSharingEnabled = message.enabled;
            this.refreshForClient(client);
            return true;
        }
        const valid = this.server.validateSessionMessage(clientId, message, { allowUnready: true });
        if (!valid.ok) return false;
        const { session, role } = valid;
        if (message.type === 'screen_share_subscribe') {
            if (role !== 'owner' || typeof message.enabled !== 'boolean') return false;
            if (!message.enabled) { this.removeSession(session, 'unsubscribed'); return true; }
            let entry = this.subscriptions.get(session.remoteSessionId);
            if (entry && entry.generation !== session.generation) {
                this.removeSession(session, 'session_changed'); entry = null;
            }
            if (!entry) {
                entry = { session, generation: session.generation, enabled: false,
                    streamId: '', screens: new Map(), lastKeyframeRequest: -Infinity };
                this.subscriptions.set(session.remoteSessionId, entry);
            }
            this.refresh(entry);
            return true;
        }
        const entry = this.subscriptions.get(session.remoteSessionId);
        if (!entry?.enabled || entry.generation !== session.generation
            || message.streamId !== entry.streamId) return false;
        if (message.type === 'screen_share_status' && role === 'target') {
            const reasons = new Set(['streaming', 'permission_denied', 'unavailable', 'error', 'starting', 'capture_error']);
            if (!reasons.has(message.reason)) return false;
            entry.status = message.reason;
            this.server.sendToEndpoint(session.ownerEndpointId,
                this.envelope(entry, 'screen_share_state', { enabled: true, reason: message.reason }));
            return true;
        }
        if (message.type === 'screen_share_keyframe' && role === 'owner'
            && integer(message.screenId, -1, 1_000_000)) {
            return this.requestKeyframe(entry, message.screenId);
        }
        return false;
    }

    requestKeyframe(entry, screenId) {
        const now = this.server.monotonicNow();
        if (now - entry.lastKeyframeRequest < 250) return false;
        entry.lastKeyframeRequest = now;
        return this.server.sendToEndpoint(entry.session.targetEndpointId,
            this.envelope(entry, 'screen_share_keyframe', { screenId }));
    }

    handleFrame(client, ws, data) {
        if (!this.current(client) || this.sockets.get(client) !== ws) return false;
        const frame = parseScreenFrame(data);
        if (!frame) return false;
        const valid = this.server.validateSessionMessage(client.id,
            { ...frame, connectionGeneration: client.connectionGeneration });
        if (!valid.ok || valid.role !== 'target' || client.screenSharingEnabled !== true) return false;
        const { session } = valid;
        const entry = this.subscriptions.get(session.remoteSessionId);
        if (!entry?.enabled || entry.generation !== frame.generation || entry.streamId !== frame.streamId) return false;
        const screens = session.latestTargetSnapshot?.generation === session.generation
            ? session.latestTargetSnapshot.snapshot.screens : client.screens;
        if (!Array.isArray(screens) || !screens.some(screen => screen.id === frame.screenId)
            || entry.topology !== JSON.stringify(screens) || JSON.stringify(client.screens) !== JSON.stringify(screens)) return false;
        const owner = this.endpointClient(session.ownerEndpointId);
        const destination = this.socket(owner);
        if (!destination || owner.connectionGeneration !== session.ownerConnectionGeneration
            || owner.runtimeId !== session.ownerRuntimeId) return false;
        let stream = entry.screens.get(frame.screenId);
        if (!stream) { stream = { sequence: 0, needsKeyframe: true }; entry.screens.set(frame.screenId, stream); }
        if (frame.sequence <= stream.sequence) return false;
        if (stream.sequence > 0 && frame.sequence !== stream.sequence + 1) stream.needsKeyframe = true;
        stream.sequence = frame.sequence;
        // A large standalone IDR is allowed, but never behind older bytes.
        const buffered = Number(destination.bufferedAmount) || 0;
        const window = this.window(destination);
        const now = this.server.monotonicNow();
        const lane = `${frame.streamId}:${frame.screenId}`;
        for (const [waiting, seen] of window.waiting)
            if (now - seen >= 500) window.waiting.delete(waiting);
        const turn = window.waiting.keys().next().value;
        if ((turn !== undefined && turn !== lane) || window.frames.size >= MAX_INFLIGHT_FRAMES
            || (window.frames.size > 0 && window.bytes + data.length > MAX_BUFFERED_BYTES)
            || buffered > MAX_BUFFERED_BYTES || (buffered > 0 && buffered + data.length > MAX_BUFFERED_BYTES)) {
            if (window.waiting.has(lane) || window.waiting.size < 256) window.waiting.set(lane, now);
            stream.needsKeyframe = true;
            this.requestKeyframe(entry, frame.screenId);
            return false;
        }
        window.waiting.delete(lane);
        if (stream.needsKeyframe && !frame.keyFrame) {
            this.requestKeyframe(entry, frame.screenId);
            return false;
        }
        stream.needsKeyframe = false;
        const key = frameKey(frame);
        window.frames.set(key, { bytes: data.length, sentAt: this.server.monotonicNow() });
        window.bytes += data.length;
        const failed = () => {
            const pending = window.frames.get(key);
            if (pending) { window.bytes -= pending.bytes; window.frames.delete(key); }
            stream.needsKeyframe = true;
            this.requestKeyframe(entry, frame.screenId);
        };
        try {
            destination.send(data, { binary: true, compress: false }, error => { if (error) failed(); });
            return true;
        } catch (_) { failed(); return false; }
    }

    removeSession(session, reason = 'session_closed') {
        const entry = this.subscriptions.get(session.remoteSessionId);
        if (!entry) return;
        this.publish(entry, false, reason);
        this.subscriptions.delete(session.remoteSessionId);
    }

    revokeClient(client) {
        for (const [token, binding] of this.tokens) if (binding.client === client) this.tokens.delete(token);
        const ws = this.sockets.get(client);
        this.sockets.delete(client);
        for (const entry of [...this.subscriptions.values()]) {
            if (entry.session.ownerEndpointId === client.endpointId
                || entry.session.targetEndpointId === client.endpointId) this.removeSession(entry.session, 'channel_unavailable');
        }
        if (ws) { this.inflight.delete(ws); ws.close(1008, 'Control channel disconnected'); }
    }
}

module.exports = { ScreenShareRelay, parseScreenFrame, MAX_FRAME_BYTES, MAX_HEADER_BYTES, MAX_BUFFERED_BYTES };
