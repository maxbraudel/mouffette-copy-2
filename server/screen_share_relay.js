'use strict';

const crypto = require('node:crypto');
const WebSocket = require('ws');
const { ScreenPublications, parsePublicationFrame } = require('./screen_publications');

const MAX_FRAME_BYTES = 2 * 1024 * 1024;
const MAX_HEADER_BYTES = 1024;
const MAX_BUFFERED_BYTES = 2048 * 1024;
const MAX_INFLIGHT_FRAMES = 64;
const ACK_TIMEOUT_MS = 3000;
const MAXIMUM_EDGE = 3840;
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
        || !integer(header.sequence, 1) || !integer(header.width, 2, MAXIMUM_EDGE) || header.width % 2 !== 0
        || !integer(header.height, 2, MAXIMUM_EDGE) || header.height % 2 !== 0
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
        this.publishSockets = new Map(); // v2 ingress; independent of viewing
        this.inflight = new Map(); // video socket -> bounded receipt window
        this.subscriptions = new Map(); // RemoteSession -> ephemeral stream grant
        this.subscriptionsByTarget = new Map();
        this.maxBufferedBytes = (server.config?.screenMaxBufferedKiB ?? MAX_BUFFERED_BYTES / 1024) * 1024;
        this.maxInflightFrames = server.config?.screenMaxInflightFrames ?? MAX_INFLIGHT_FRAMES;
        this.ackTimeoutMs = server.config?.screenAckTimeoutMs ?? ACK_TIMEOUT_MS;
        this.feedbackIntervalMs = server.config?.screenFeedbackIntervalMs ?? 500;
        this.keyframeRequestIntervalMs = server.config?.screenKeyframeRequestIntervalMs ?? 1000;
        this.queueTargetMs = server.config?.screenQueueTargetMs ?? 150;
        this.sharedEnabled = server.config?.screenSharedEnabled ?? true;
        this.shared = new ScreenPublications(this);
    }

    current(client) {
        return !!client && client.authenticated && !client.draining && !client.replaced
            && this.server.clients.get(client.id) === client
            && this.server.currentTransportByEndpoint.get(client.endpointId) === client
            && client.ws?.readyState === WebSocket.OPEN
            && !this.server.clientLeaseExpired(client);
    }

    issueToken(clientId, requestId, capabilities = {}) {
        const client = this.server.clients.get(clientId);
        if (!this.current(client) || !opaque(requestId)) return false;
        if ((capabilities.mediaVersion !== undefined || capabilities.role !== undefined)
            && (capabilities.mediaVersion !== 2 || !['view', 'publish'].includes(capabilities.role))) return false;
        if ((capabilities.feedbackVersion !== undefined && capabilities.feedbackVersion !== 1)
            || (capabilities.maximumEdge !== undefined
                && (!integer(capabilities.maximumEdge, 2, MAXIMUM_EDGE) || capabilities.maximumEdge % 2))) return false;
        const now = this.server.monotonicNow();
        const role = this.sharedEnabled && capabilities.mediaVersion === 2 ? capabilities.role : undefined;
        for (const [token, binding] of this.tokens) {
            if ((binding.client === client && binding.role === role) || binding.expiresAt <= now) this.tokens.delete(token);
        }
        const token = crypto.randomBytes(32).toString('base64url');
        this.tokens.set(token, { client, expiresAt: now + 15_000,
            role, mediaVersion: role ? 2 : 1,
            feedbackVersion: capabilities.feedbackVersion === 1 ? 1 : 0,
            maximumEdge: capabilities.maximumEdge ?? 1920 });
        return this.server.sendToEndpoint(client.endpointId, {
            type: 'screen_channel_token', requestId, token,
            ...(role ? { role, mediaVersion: 2 } : {}),
        });
    }

    consumeToken(token, ws = null) {
        if (typeof token !== 'string' || token.length !== 43) return null;
        const binding = this.tokens.get(token);
        this.tokens.delete(token);
        if (!binding || binding.expiresAt <= this.server.monotonicNow() || !this.current(binding.client)) return null;
        if (ws) {
            ws.screenFeedbackVersion = binding.feedbackVersion;
            ws.screenMaximumEdge = binding.maximumEdge;
            ws.screenRole = binding.role;
            ws.screenMediaVersion = binding.mediaVersion;
        }
        return binding.client;
    }

    acceptSocket(ws, token) {
        const client = this.consumeToken(token, ws);
        if (!client) { ws.close(1008, 'Screen channel authentication failed'); return; }
        const sockets = ws.screenRole === 'publish' ? this.publishSockets : this.sockets;
        const previous = sockets.get(client);
        if (previous) {
            sockets.delete(client);
            this.refreshForClient(client);
            previous.close(1008, 'Screen channel replaced');
        }
        ws.mouffetteClient = client;
        sockets.set(client, ws);
        ws.on('message', (data, isBinary) => {
            if (!this.current(client) || sockets.get(client) !== ws) {
                ws.close(1008, 'Stale screen channel'); return;
            }
            if (!isBinary) {
                if (ws.screenRole === 'publish') { ws.close(1008, 'Unexpected publisher message'); return; }
                if (!this.handleAcknowledgement(client, ws, data)) ws.close(1008, 'Invalid screen acknowledgement');
                return;
            }
            if (ws.screenRole === 'view') { ws.close(1008, 'Viewer cannot publish'); return; }
            if (ws.screenRole === 'publish') {
                const parsed = parsePublicationFrame(data);
                if (!parsed) { ws.close(1008, 'Invalid publication frame'); return; }
                if ((Number(ws.bufferedAmount) || 0) > this.maxBufferedBytes) {
                    if (typeof ws.terminate === 'function') ws.terminate();
                    else ws.close(1008, 'Publication receipt congestion');
                    return;
                }
                this.shared.handleFrame(client, ws, parsed);
                return;
            }
            const frame = parseScreenFrame(data);
            if (!frame) {
                ws.close(1008, 'Invalid screen frame');
                return;
            }
            if ((Number(ws.bufferedAmount) || 0) > this.maxBufferedBytes) {
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
            this.shared.receivers.delete(ws);
            if (sockets.get(client) !== ws) return;
            sockets.delete(client);
            this.refreshForClient(client);
        };
        ws.on('close', lost);
        ws.on('error', lost);
        ws.send(JSON.stringify({ type: 'screen_channel_ready', endpointId: client.endpointId,
            protocolVersion: this.server.protocolVersion, serverBootId: this.server.serverBootId,
            messageId: crypto.randomUUID(), connectionGeneration: client.connectionGeneration,
            feedbackVersion: 1, maximumEdge: MAXIMUM_EDGE,
            ...(ws.screenRole ? { role: ws.screenRole, mediaVersion: 2 } : {}) }));
        this.refreshForClient(client);
    }

    window(ws) {
        let window = this.inflight.get(ws);
        if (!window) {
            window = { frames: new Map(), waiting: new Map(), bytes: 0, baselineRttMs: null, baselineAt: 0 };
            this.inflight.set(ws, window);
        }
        return window;
    }

    handleAcknowledgement(client, ws, data) {
        if (!this.current(client) || this.sockets.get(client) !== ws || data.length > 1024) return false;
        let ack;
        try { ack = JSON.parse(data.toString()); } catch (_) { return false; }
        if (ack?.type === 'screen_view_feedback') return this.handleViewerFeedback(client, ack);
        if (!ack || typeof ack !== 'object' || Array.isArray(ack) || Object.keys(ack).length !== 4
            || ack.type !== 'screen_frame_ack' || !UUID.test(ack.streamId || '')
            || !integer(ack.screenId, 0, 1_000_000) || !integer(ack.sequence, 1)) return false;
        const window = this.inflight.get(ws);
        const key = frameKey(ack), pending = window?.frames.get(key);
        // Duplicates and delayed receipts release no other stream's credit.
        if (pending) {
            window.bytes -= pending.bytes;
            window.frames.delete(key);
            const now = this.server.monotonicNow();
            const rtt = Math.min(60000, Math.max(0, Math.round(now - pending.sentAt)));
            const baselineSample = Math.max(0, rtt - (pending.serializationMs || 0));
            if (window.baselineRttMs === null || now - window.baselineAt >= 30000 || baselineSample <= window.baselineRttMs) {
                window.baselineRttMs = baselineSample;
                window.baselineAt = now;
            }
            const entry = this.subscriptions.get(pending.remoteSessionId);
            if (entry?.streamId === ack.streamId && this.feedbackReady(entry)) {
                this.recordFeedback(entry, ack.screenId, {
                    deliveryRttMs: rtt,
                    ...(pending.serializationMs ? { serializationMs: pending.serializationMs } : {}),
                    bufferedBytes: Math.max(window.bytes, Number(ws.bufferedAmount) || 0),
                });
            }
        }
        return true;
    }

    deliveryWindowDelayed(window, now) {
        // Bytes in flight include healthy propagation/serialization time. Use
        // observed RTT growth rather than the frame-credit cap as congestion.
        const limit = window.baselineRttMs === null
            ? Math.min(1000, this.ackTimeoutMs / 2)
            : window.baselineRttMs + this.queueTargetMs;
        return [...window.frames.values()].some(frame => now - frame.sentAt > limit + (frame.serializationMs || 0));
    }

    requested(entry, screenId) {
        return entry.requestedScreens === null || entry.requestedScreens.some(screen => screen.screenId === screenId);
    }

    effectiveSelection(entry) {
        const owner = this.socket(this.endpointClient(entry.session.ownerEndpointId));
        const target = this.publisherSocket(this.endpointClient(entry.session.targetEndpointId));
        const maximum = Math.min(owner?.screenMaximumEdge ?? 1920, target?.screenMaximumEdge ?? 1920);
        return this.topology(entry.session).filter(screen => this.requested(entry, screen.id)).map(screen => ({
            screenId: screen.id,
            maximumEdge: Math.min(maximum,
                entry.requestedScreens?.find(request => request.screenId === screen.id)?.maximumEdge ?? maximum),
        }));
    }

    topology(session) {
        const target = this.endpointClient(session.targetEndpointId);
        return session.latestTargetSnapshot?.generation === session.generation
            ? session.latestTargetSnapshot.snapshot.screens : target?.screens || [];
    }

    feedbackReady(entry) {
        const session = this.server.remoteSessions.get(entry.session.remoteSessionId);
        if (!entry.enabled || !session || session !== entry.session
            || session.generation !== entry.generation || !this.server.remoteSessions.commandReady(session)) return false;
        const owner = this.endpointClient(session.ownerEndpointId), target = this.endpointClient(session.targetEndpointId);
        return !!this.socket(owner) && !!this.publisherSocket(target)
            && this.server.validateSessionMessage(target.id, {
                remoteSessionId: session.remoteSessionId, generation: entry.generation,
                connectionGeneration: target.connectionGeneration,
            }).ok && target.screenSharingEnabled === true
            && owner.connectionGeneration === session.ownerConnectionGeneration
            && target.connectionGeneration === session.targetConnectionGeneration
            && owner.runtimeId === session.ownerRuntimeId && target.runtimeId === session.targetRuntimeId
            && entry.topology === JSON.stringify(this.topology(session))
            && entry.topology === JSON.stringify(target.screens);
    }

    handleViewerFeedback(client, message) {
        const keys = ['type', 'remoteSessionId', 'generation', 'streamId', 'screenId', 'decodeMs', 'droppedFrames'];
        if (!message || Array.isArray(message) || Object.keys(message).length !== keys.length
            || Object.keys(message).some(key => !keys.includes(key))
            || !opaque(message.remoteSessionId) || !integer(message.generation, 1)
            || !UUID.test(message.streamId || '') || !integer(message.screenId, 0, 1_000_000)
            || !integer(message.decodeMs, 0, 60000) || !integer(message.droppedFrames, 0, 10000)) return false;
        const session = this.server.remoteSessions.get(message.remoteSessionId);
        if (session && session.ownerEndpointId !== client.endpointId) return false;
        const valid = this.server.validateSessionMessage(client.id,
            { ...message, connectionGeneration: client.connectionGeneration });
        const entry = this.subscriptions.get(message.remoteSessionId);
        if (!valid.ok || valid.role !== 'owner' || !entry || entry.streamId !== message.streamId
            || !this.feedbackReady(entry) || !this.requested(entry, message.screenId)
            || !this.topology(entry.session).some(screen => screen.id === message.screenId)) {
            // Control and video queues race naturally during hide, revocation,
            // topology changes and recovery. Consume stale telemetry without
            // credit or authority; it must not restart an otherwise healthy pipe.
            return true;
        }
        const feedback = this.feedbackLane(entry, message.screenId);
        const now = this.server.monotonicNow();
        if (now - feedback.lastViewerAt < this.feedbackIntervalMs) return true;
        feedback.lastViewerAt = now;
        this.recordFeedback(entry, message.screenId,
            { congested: message.droppedFrames > 0 || message.decodeMs > 100 });
        return true;
    }

    feedbackLane(entry, screenId) {
        let feedback = entry.feedback.get(screenId);
        if (!feedback) {
            feedback = { lastSentAt: -Infinity, lastViewerAt: -Infinity,
                deliveryRttMs: 0, bufferedBytes: 0, congested: false, pending: false };
            entry.feedback.set(screenId, feedback);
        }
        return feedback;
    }

    recordFeedback(entry, screenId, sample) {
        if (!this.requested(entry, screenId)) return;
        if (this.shared.source(this.endpointClient(entry.session.targetEndpointId))) {
            this.shared.recordFeedback(entry, screenId, sample);
            return;
        }
        const feedback = this.feedbackLane(entry, screenId);
        if (sample.deliveryRttMs !== undefined) feedback.deliveryRttMs = sample.deliveryRttMs;
        if (sample.bufferedBytes !== undefined) feedback.bufferedBytes = Math.min(16 * 1024 * 1024, sample.bufferedBytes);
        feedback.congested ||= sample.congested === true;
        feedback.pending = true;
        this.flushFeedback(entry, screenId, feedback);
    }

    flushFeedback(entry, screenId, feedback) {
        const now = this.server.monotonicNow();
        if (!feedback.pending || now - feedback.lastSentAt < this.feedbackIntervalMs
            || !this.feedbackReady(entry) || !this.requested(entry, screenId)) return;
        const publisher = this.endpointClient(entry.session.targetEndpointId);
        const ws = this.publisherSocket(publisher);
        if (!ws || ws.screenMediaVersion === 2 || ws.screenFeedbackVersion !== 1
            || (Number(ws.bufferedAmount) || 0) > this.maxBufferedBytes) return;
        // Uploads are removed from the live map when terminal. Paused/queued
        // transfers retain their reservation so video does not impede recovery.
        const uploadActive = [...(this.server.uploads?.values() || [])].some(upload =>
            upload.targetEndpointId === entry.session.ownerEndpointId);
        try {
            ws.send(JSON.stringify(this.envelope(entry, 'screen_share_feedback', {
                screenId, deliveryRttMs: feedback.deliveryRttMs, bufferedBytes: feedback.bufferedBytes,
                congested: feedback.congested, uploadActive,
                protocolVersion: this.server.protocolVersion, serverBootId: this.server.serverBootId,
                messageId: crypto.randomUUID(), connectionGeneration: publisher.connectionGeneration,
            })));
            feedback.lastSentAt = now;
            feedback.pending = false;
            feedback.congested = false;
        } catch (_) { /* Feedback is expendable; retain its latest sample. */ }
    }

    sweep() {
        const now = this.server.monotonicNow();
        for (const [ws, window] of this.inflight) {
            if ([...window.frames.values()].some(frame => now >= (frame.deadlineAt ?? frame.sentAt + this.ackTimeoutMs))) {
                this.inflight.delete(ws);
                // close() would put a close frame behind the stale TCP queue.
                // Terminate the disposable pipe immediately to shed that queue.
                if (typeof ws.terminate === 'function') ws.terminate();
                else ws.close(1008, 'Screen receipt timeout');
            } else if (this.deliveryWindowDelayed(window, now)) {
                const reported = new Set();
                for (const pending of window.frames.values()) {
                    const lane = `${pending.streamId}:${pending.screenId}`;
                    const entry = this.subscriptions.get(pending.remoteSessionId);
                    if (reported.has(lane) || entry?.streamId !== pending.streamId) continue;
                    reported.add(lane);
                    this.recordFeedback(entry, pending.screenId, {
                        bufferedBytes: Math.max(window.bytes, Number(ws.bufferedAmount) || 0), congested: true,
                    });
                }
            }
        }
        for (const entry of this.subscriptions.values())
            for (const [screenId, feedback] of entry.feedback) this.flushFeedback(entry, screenId, feedback);
        this.shared.sweep();
    }

    endpointClient(endpointId) {
        return this.server.currentTransportByEndpoint.get(endpointId);
    }

    socket(client) {
        const ws = this.sockets.get(client);
        return this.current(client) && ws?.readyState === WebSocket.OPEN ? ws : null;
    }

    publisherSocket(client) {
        const ws = this.publishSockets.get(client);
        if (this.current(client) && ws?.readyState === WebSocket.OPEN) return ws;
        const legacy = this.socket(client);
        return legacy?.screenMediaVersion === 2 ? null : legacy;
    }

    envelope(entry, type, extra = {}) {
        return { type, remoteSessionId: entry.session.remoteSessionId,
            generation: entry.generation, streamId: entry.streamId || '',
            receiverMaximumEdge: this.socket(this.endpointClient(entry.session.ownerEndpointId))?.screenMaximumEdge ?? 1920,
            ...extra };
    }

    publish(entry, enabled, reason) {
        this.shared.admissionCounts = null;
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
            entry.screenStatuses.clear();
            entry.feedback.clear();
            entry.status = '';
        }
        // Repeated subscriptions act as bounded state queries, allowing an
        // owner that was not ready to apply the first status to recover.
        const selection = { screens: this.effectiveSelection(entry) };
        this.server.sendToEndpoint(entry.session.ownerEndpointId,
            this.envelope(entry, 'screen_share_state', { ...selection, enabled, reason: enabled && entry.status ? entry.status : reason }));
        if (enabled) {
            for (const [screenId, status] of entry.screenStatuses)
                this.server.sendToEndpoint(entry.session.ownerEndpointId,
                    this.envelope(entry, 'screen_share_state', { enabled: true, screenId, reason: status }));
        }
        const target = this.endpointClient(entry.session.targetEndpointId);
        if (this.shared.source(target) || this.shared.byClient.has(target)) this.shared.refresh(target);
        else if (this.socket(target)?.screenMediaVersion !== 2)
            this.server.sendToEndpoint(entry.session.targetEndpointId,
                this.envelope(entry, 'screen_share_request', { ...selection, enabled, reason }));
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
        else if (!this.socket(owner) || !this.publisherSocket(target)) reason = 'channel_unavailable';
        if (entry.enabled && entry.topology !== signature) this.publish(entry, false, 'topology_changed');
        entry.topology = signature;
        this.publish(entry, reason === 'ready', reason);
    }

    refreshForClient(client) {
        for (const entry of this.subscriptions.values()) {
            if (entry.session.ownerEndpointId === client.endpointId
                || entry.session.targetEndpointId === client.endpointId) this.refresh(entry);
        }
        this.shared.refresh(client);
    }

    refreshSession(session) {
        const entry = this.subscriptions.get(session.remoteSessionId);
        if (entry) this.refresh(entry);
    }

    handleControl(clientId, message) {
        const client = this.server.clients.get(clientId);
        if (!this.current(client)) return false;
        if (message.type === 'request_screen_channel') return this.issueToken(clientId, message.requestId, message);
        if (message.type === 'screen_publication_status') return this.shared.status(client, message);
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
            let requestedScreens = null;
            if (message.screens !== undefined) {
                if (!Array.isArray(message.screens) || message.screens.length > 64) return false;
                const topology = this.topology(session), seen = new Set();
                for (const screen of message.screens) {
                    if (!screen || typeof screen !== 'object' || Array.isArray(screen)
                        || Object.keys(screen).length !== 2 || !integer(screen.screenId, 0, 1_000_000)
                        || seen.has(screen.screenId) || !topology.some(item => item.id === screen.screenId)
                        || !integer(screen.maximumEdge, 2, MAXIMUM_EDGE) || screen.maximumEdge % 2) return false;
                    seen.add(screen.screenId);
                }
                requestedScreens = message.screens.map(({ screenId, maximumEdge }) => ({ screenId, maximumEdge }));
            }
            let entry = this.subscriptions.get(session.remoteSessionId);
            if (requestedScreens === null || requestedScreens.length) {
                const viewers = new Set([...this.subscriptions.values()]
                    .filter(other => other.session.targetEndpointId === session.targetEndpointId
                        && other.session.ownerEndpointId !== session.ownerEndpointId
                        && (other.requestedScreens === null || other.requestedScreens.length))
                    .map(other => other.session.ownerEndpointId));
                if (viewers.size >= this.shared.maxViewers) {
                    this.server.sendToEndpoint(session.ownerEndpointId, {
                        type: 'screen_share_state', remoteSessionId: session.remoteSessionId, generation: session.generation,
                        streamId: '', enabled: false, reason: 'viewer_capacity',
                    });
                    return false;
                }
            }
            if (entry && entry.generation !== session.generation) {
                this.removeSession(session, 'session_changed'); entry = null;
            }
            if (!entry) {
                entry = { session, generation: session.generation, enabled: false,
                    streamId: '', screens: new Map(), screenStatuses: new Map(), feedback: new Map(), requestedScreens: null,
                    lastKeyframeRequest: -Infinity };
                this.subscriptions.set(session.remoteSessionId, entry);
                let targetEntries = this.subscriptionsByTarget.get(session.targetEndpointId);
                if (!targetEntries) {
                    targetEntries = new Set(); this.subscriptionsByTarget.set(session.targetEndpointId, targetEntries);
                }
                targetEntries.add(entry);
            }
            entry.requestedScreens = requestedScreens;
            for (const screenId of entry.screens.keys()) {
                if (!this.requested(entry, screenId)) {
                    entry.screens.delete(screenId);
                    const lane = `${entry.streamId}:${screenId}`;
                    for (const window of this.inflight.values()) window.waiting.delete(lane);
                }
            }
            for (const screenId of entry.feedback.keys())
                if (!this.requested(entry, screenId)) entry.feedback.delete(screenId);
            for (const screenId of entry.screenStatuses.keys())
                if (!this.requested(entry, screenId)) entry.screenStatuses.delete(screenId);
            this.refresh(entry);
            return true;
        }
        const entry = this.subscriptions.get(session.remoteSessionId);
        if (!entry?.enabled || entry.generation !== session.generation
            || message.streamId !== entry.streamId) return false;
        if (message.type === 'screen_share_status' && role === 'target') {
            const reasons = new Set(['streaming', 'permission_denied', 'unavailable', 'error', 'starting', 'capture_error']);
            if (!reasons.has(message.reason) || !this.feedbackReady(entry)) return false;
            const scoped = message.screenId !== undefined;
            if (scoped) {
                if (!integer(message.screenId, 0, 1_000_000) || !this.requested(entry, message.screenId)
                    || !this.topology(session).some(screen => screen.id === message.screenId)) return false;
                entry.screenStatuses.set(message.screenId, message.reason);
                if (message.reason !== 'starting' && message.reason !== 'streaming') {
                    const stream = entry.screens.get(message.screenId);
                    if (stream) stream.needsKeyframe = true;
                    for (const window of this.inflight.values())
                        window.waiting.delete(`${entry.streamId}:${message.screenId}`);
                }
            } else entry.status = message.reason;
            this.server.sendToEndpoint(session.ownerEndpointId,
                this.envelope(entry, 'screen_share_state', {
                    enabled: true, reason: message.reason, ...(scoped ? { screenId: message.screenId } : {}),
                }));
            return true;
        }
        if (message.type === 'screen_share_keyframe' && role === 'owner'
            && integer(message.screenId, -1, 1_000_000)
            && (message.screenId === -1 || (this.requested(entry, message.screenId)
                && this.topology(session).some(screen => screen.id === message.screenId)))) {
            return this.requestKeyframe(entry, message.screenId);
        }
        return false;
    }

    requestKeyframe(entry, screenId) {
        const publication = this.shared.byClient.get(this.endpointClient(entry.session.targetEndpointId));
        if (publication) {
            if (screenId === -1) {
                for (const id of publication.screens.keys())
                    if (this.requested(entry, id)) this.shared.requestKeyframe(publication, id, entry.screens.get(id)?.layer || 'main');
                return true;
            }
            return this.shared.requestKeyframe(publication, screenId, entry.screens.get(screenId)?.layer || 'main');
        }
        const now = this.server.monotonicNow();
        if (now - entry.lastKeyframeRequest < this.keyframeRequestIntervalMs) return false;
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
        if (!entry?.enabled || entry.generation !== frame.generation || entry.streamId !== frame.streamId
            || !this.requested(entry, frame.screenId)) return false;
        const screens = session.latestTargetSnapshot?.generation === session.generation
            ? session.latestTargetSnapshot.snapshot.screens : client.screens;
        if (!Array.isArray(screens) || !screens.some(screen => screen.id === frame.screenId)
            || entry.topology !== JSON.stringify(screens) || JSON.stringify(client.screens) !== JSON.stringify(screens)) return false;
        const owner = this.endpointClient(session.ownerEndpointId);
        const destination = this.socket(owner);
        if (!destination || owner.connectionGeneration !== session.ownerConnectionGeneration
            || owner.runtimeId !== session.ownerRuntimeId) return false;
        const maximumEdge = Math.min(ws.screenMaximumEdge ?? 1920, destination.screenMaximumEdge ?? 1920);
        if (frame.width > maximumEdge || frame.height > maximumEdge) return false;
        const status = entry.screenStatuses.get(frame.screenId);
        if (status && status !== 'starting' && status !== 'streaming') return false;
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
        const delayed = this.deliveryWindowDelayed(window, now);
        for (const [waiting, deadline] of window.waiting)
            if (now >= deadline) window.waiting.delete(waiting);
        const turn = window.waiting.keys().next().value;
        if (delayed || (turn !== undefined && turn !== lane) || window.frames.size >= this.maxInflightFrames
            || (window.frames.size > 0 && window.bytes + data.length > this.maxBufferedBytes)
            || buffered > this.maxBufferedBytes || (buffered > 0 && buffered + data.length > this.maxBufferedBytes)
            || (data.length > this.maxBufferedBytes && !frame.keyFrame)) {
            if (window.waiting.has(lane) || window.waiting.size < 256) window.waiting.set(lane, now + this.ackTimeoutMs);
            stream.needsKeyframe = true;
            if (delayed || buffered > this.maxBufferedBytes)
                this.recordFeedback(entry, frame.screenId,
                    { bufferedBytes: Math.max(window.bytes, buffered), congested: true });
            this.requestKeyframe(entry, frame.screenId);
            return false;
        }
        window.waiting.delete(lane);
        if (stream.needsKeyframe && !frame.keyFrame) {
            this.requestKeyframe(entry, frame.screenId);
            return false;
        }
        if (!this.shared.reserveLegacyEgress(destination, data.length)) {
            stream.needsKeyframe = true;
            if (window.waiting.has(lane) || window.waiting.size < 256) window.waiting.set(lane, now + this.ackTimeoutMs);
            this.recordFeedback(entry, frame.screenId, { congested: true, bufferedBytes: window.bytes });
            this.requestKeyframe(entry, frame.screenId);
            return false;
        }
        stream.needsKeyframe = false;
        const key = frameKey(frame);
        window.frames.set(key, { bytes: data.length, sentAt: this.server.monotonicNow(),
            remoteSessionId: session.remoteSessionId, streamId: frame.streamId, screenId: frame.screenId });
        window.bytes += data.length;
        const failed = () => {
            const pending = window.frames.get(key);
            if (pending) { window.bytes -= pending.bytes; window.frames.delete(key); }
            stream.needsKeyframe = true;
            this.recordFeedback(entry, frame.screenId,
                { bufferedBytes: Math.max(window.bytes, Number(destination.bufferedAmount) || 0), congested: true });
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
        const targetEntries = this.subscriptionsByTarget.get(entry.session.targetEndpointId);
        targetEntries?.delete(entry);
        if (targetEntries?.size === 0) this.subscriptionsByTarget.delete(entry.session.targetEndpointId);
    }

    revokeClient(client) {
        for (const [token, binding] of this.tokens) if (binding.client === client) this.tokens.delete(token);
        const ws = this.sockets.get(client);
        const publisher = this.publishSockets.get(client);
        this.sockets.delete(client);
        this.publishSockets.delete(client);
        this.shared.retire(client);
        for (const entry of [...this.subscriptions.values()]) {
            if (entry.session.ownerEndpointId === client.endpointId
                || entry.session.targetEndpointId === client.endpointId) this.removeSession(entry.session, 'channel_unavailable');
        }
        if (ws) { this.inflight.delete(ws); ws.close(1008, 'Control channel disconnected'); }
        if (publisher) publisher.close(1008, 'Control channel disconnected');
    }
}

module.exports = { ScreenShareRelay, parseScreenFrame, MAX_FRAME_BYTES, MAX_HEADER_BYTES, MAX_BUFFERED_BYTES };
