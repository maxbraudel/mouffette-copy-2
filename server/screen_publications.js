'use strict';

const crypto = require('node:crypto');
const MAX_FRAME_BYTES = 2 * 1024 * 1024;
const MAX_HEADER_BYTES = 1024;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const integer = (n, min, max = Number.MAX_SAFE_INTEGER) => Number.isSafeInteger(n) && n >= min && n <= max;
const layerName = value => value === 'main' || value === 'low';
const KEYS = new Set(['publicationId', 'screenId', 'layer', 'sequence', 'width', 'height',
    'keyFrame', 'codec', 'timestampUs', 'bitrateBps', 'fps']);

function selfContainedIdr(payload) {
    let mask = 0;
    for (let i = 0; i + 3 < payload.length; ++i) {
        if (payload[i] !== 0 || payload[i + 1] !== 0) continue;
        const offset = payload[i + 2] === 1 ? i + 3
            : payload[i + 2] === 0 && payload[i + 3] === 1 ? i + 4 : -1;
        if (offset < 0 || offset >= payload.length) continue;
        const type = payload[offset] & 31;
        mask |= type === 7 ? 1 : type === 8 ? 2 : type === 5 ? 4 : 0;
        if (mask === 7) return true;
        i = offset;
    }
    return false;
}

function parsePublicationFrame(data) {
    if (!Buffer.isBuffer(data) || data.length < 12 || data.length > MAX_FRAME_BYTES + MAX_HEADER_BYTES + 6
        || data.toString('ascii', 0, 4) !== 'MSV2') return null;
    const size = data.readUInt16BE(4);
    if (size < 2 || size > MAX_HEADER_BYTES || data.length <= size + 6 || data.length - size - 6 > MAX_FRAME_BYTES) return null;
    let header;
    try { header = JSON.parse(data.toString('utf8', 6, size + 6)); } catch (_) { return null; }
    if (!header || Array.isArray(header) || typeof header !== 'object'
        || Object.keys(header).some(key => !KEYS.has(key)) || !UUID.test(header.publicationId || '')
        || !integer(header.screenId, 0, 1_000_000) || !layerName(header.layer) || !integer(header.sequence, 1)
        || !integer(header.width, 2, 3840) || header.width % 2 || !integer(header.height, 2, 3840) || header.height % 2
        || typeof header.keyFrame !== 'boolean' || header.codec !== 'h264'
        || (header.timestampUs !== undefined && !integer(header.timestampUs, 0))
        || (header.bitrateBps !== undefined && !integer(header.bitrateBps, 1, 400_000_000))
        || (header.fps !== undefined && !integer(header.fps, 1, 120))) return null;
    const payload = data.subarray(size + 6);
    if (payload.length < 5 || payload[0] !== 0 || payload[1] !== 0
        || !(payload[2] === 1 || (payload[2] === 0 && payload[3] === 1))) return null;
    if (header.keyFrame && !selfContainedIdr(payload)) return null;
    return { header, payload, encodedBytes: data.length };
}

// One publication per admitted source transport; subscriptions retain their own
// session authority. Only the latest complete IDR is retained, never a GOP queue.
class ScreenPublications {
    constructor(relay) {
        this.relay = relay;
        this.server = relay.server;
        const config = this.server.config || {};
        this.byClient = new Map();
        this.receivers = new Map();
        this.receiverEstimates = new WeakMap(); // authenticated control lifetime, no socket retention
        this.cache = new Map();
        this.cacheBytes = 0;
        this.maxViewers = config.screenMaxViewersPerPublisher ?? 10;
        this.maxPublications = config.screenMaxPublications ?? 256;
        this.cacheLimit = (config.screenKeyframeCacheMiB ?? 16) * 1024 * 1024;
        this.cacheTtlMs = config.screenKeyframeCacheTtlMs ?? 5000;
        this.egressBps = config.screenServerEgressBps ?? 100_000_000;
        this.minimumBps = config.screenViewerMinBps ?? 64_000;
        this.initialBps = config.screenViewerInitialBps ?? 3_000_000;
        this.maximumBps = config.screenViewerMaxBps ?? 20_000_000;
        this.recoveryMs = config.screenViewerRecoveryMs ?? 3000;
        this.snapshotMs = config.screenSnapshotIntervalMs ?? 1000;
        this.snapshotMaxTransferMs = config.screenSnapshotMaxTransferMs ?? 4000;
        this.uploadPercent = config.screenViewerUploadPercent ?? 40;
        this.nextSendAt = 0;
        this.admissionCounts = null;
    }

    source(client) {
        const ws = this.relay.publisherSocket(client);
        return ws?.screenMediaVersion === 2 && ws.screenRole === 'publish' ? ws : null;
    }

    eligible(client) {
        return [...(this.relay.subscriptionsByTarget.get(client.endpointId) || [])]
            .filter(entry => entry.enabled && this.relay.feedbackReady(entry));
    }

    receiver(ws) {
        let state = this.receivers.get(ws);
        if (!state) {
            const previous = ws?.mouffetteClient && this.receiverEstimates.get(ws.mouffetteClient);
            state = { targetBps: Math.min(this.initialBps, previous?.targetBps ?? this.initialBps), nextSendAt: 0,
                lastCongestionAt: previous ? this.server.monotonicNow() : -Infinity,
                lastDecreaseAt: -Infinity, lastIncreaseAt: this.server.monotonicNow() };
            this.receivers.set(ws, state);
        }
        return state;
    }

    refresh(client) {
        if (!client) return;
        const ws = this.source(client), topology = JSON.stringify(client.screens || []);
        const entries = ws && client.screenSharingEnabled === true ? this.eligible(client) : [];
        let publication = this.byClient.get(client);
        if (publication && (publication.socket !== ws || publication.topology !== topology || entries.length === 0)) {
            this.retire(client);
            publication = null;
        }
        if (!entries.length) return;
        const wanted = new Map();
        for (const entry of entries) {
            for (const screen of this.relay.effectiveSelection(entry)) {
                const demand = wanted.get(screen.screenId) || { screenId: screen.screenId, maximumEdge: 2, layers: ['main'] };
                demand.maximumEdge = Math.max(demand.maximumEdge, screen.maximumEdge);
                wanted.set(screen.screenId, demand);
            }
        }
        if (wanted.size === 0) { this.retire(client); return; }
        if (!publication) {
            if (this.byClient.size >= this.maxPublications) {
                for (const entry of entries)
                    this.server.sendToEndpoint(entry.session.ownerEndpointId,
                        this.relay.envelope(entry, 'screen_share_state', { enabled: true, reason: 'capacity_limited' }));
                return;
            }
            publication = { publicationId: crypto.randomUUID(), client, socket: ws, topology,
                screens: new Map(), signature: '', lastKeyframeAt: -Infinity, requestedKeyframes: new Map() };
            this.byClient.set(client, publication);
        }
        for (const entry of entries) {
            const receiverSocket = this.relay.socket(this.relay.endpointClient(entry.session.ownerEndpointId));
            const screenBudget = this.budget(receiverSocket) / this.screenCount(receiverSocket);
            const cap = this.relay.envelope(entry, '').receiverMaximumEdge;
            for (const request of this.relay.effectiveSelection(entry)) {
                const demand = wanted.get(request.screenId), lane = publication.screens.get(request.screenId)?.layers.get('main');
                if (demand.maximumEdge > cap || (request.maximumEdge <= 960 && demand.maximumEdge > 960)
                    || screenBudget < (lane?.bitrateBps || this.initialBps)) demand.layers = ['main', 'low'];
            }
        }
        // A disabled optional layer must not leave a legacy decoder receiving
        // an incompatible main profile. This hard capability constraint is
        // distinct from a slow viewer's bandwidth, which never caps the source.
        for (const [screenId, demand] of wanted) {
            if (publication.screens.get(screenId)?.layers.get('low')?.status !== 'inactive') continue;
            for (const entry of entries) {
                if (this.relay.requested(entry, screenId)) demand.maximumEdge = Math.min(demand.maximumEdge,
                    this.relay.envelope(entry, '').receiverMaximumEdge);
            }
        }
        for (const [screenId, screen] of publication.screens) {
            const demand = wanted.get(screenId);
            if (!demand) {
                for (const layer of screen.layers.values()) this.deleteCache(layer.cacheKey);
                publication.screens.delete(screenId);
                continue;
            }
            screen.demand = demand;
            for (const [name, layer] of screen.layers)
                if (name === 'low' && !demand.layers.includes(name)) this.deleteCache(layer.cacheKey);
        }
        for (const [screenId, demand] of wanted)
            if (!publication.screens.has(screenId)) publication.screens.set(screenId, { demand, layers: new Map() });
        for (const [key, request] of publication.requestedKeyframes)
            if (!wanted.has(request.screenId) || (request.layer === 'low' && !wanted.get(request.screenId).layers.includes('low')))
                publication.requestedKeyframes.delete(key);
        const screens = [...wanted.values()].sort((a, b) => a.screenId - b.screenId);
        const signature = JSON.stringify(screens);
        if (publication.signature !== signature) {
            publication.signature = signature;
            this.server.sendToEndpoint(client.endpointId, {
                type: 'screen_publication_request', publicationId: publication.publicationId, enabled: true, screens,
            });
        }
    }

    retire(client) {
        const publication = this.byClient.get(client);
        if (!publication) return;
        this.byClient.delete(client);
        for (const screen of publication.screens.values())
            for (const layer of screen.layers.values()) this.deleteCache(layer.cacheKey);
        this.server.sendToEndpoint(client.endpointId, {
            type: 'screen_publication_request', publicationId: publication.publicationId, enabled: false, screens: [],
        });
    }

    binding(client, publicationId) {
        const publication = this.byClient.get(client);
        if (!publication || publication.publicationId !== publicationId || !this.relay.current(client)
            || publication.socket !== this.source(client) || client.screenSharingEnabled !== true
            || publication.topology !== JSON.stringify(client.screens || [])) return null;
        return publication;
    }

    layer(publication, screenId, name) {
        const screen = publication.screens.get(screenId);
        if (!screen || (name !== 'main' && !screen.demand.layers.includes(name))) return null;
        let layer = screen.layers.get(name);
        if (!layer) {
            layer = { sequence: 0, needsKeyframe: true, status: 'starting', width: 0, height: 0,
                bitrateBps: 0, fps: 0, cacheKey: `${publication.publicationId}:${screenId}:${name}` };
            screen.layers.set(name, layer);
        }
        return layer;
    }

    status(client, message) {
        const reasons = new Set(['starting', 'streaming', 'inactive', 'permission_denied', 'capture_error']);
        const publication = this.binding(client, message.publicationId);
        if (!publication || !integer(message.screenId, 0, 1_000_000) || !layerName(message.layer)
            || !reasons.has(message.reason) || !this.eligible(client).some(entry => this.relay.requested(entry, message.screenId))
            || (message.bitrateBps !== undefined && !integer(message.bitrateBps, 0, 400_000_000))
            || (message.fps !== undefined && !integer(message.fps, 0, 120))) return false;
        const layer = this.layer(publication, message.screenId, message.layer);
        if (!layer) return false;
        layer.status = message.reason;
        if (message.bitrateBps) layer.bitrateBps = message.bitrateBps;
        if (message.fps) layer.fps = message.fps;
        if (!['starting', 'streaming'].includes(message.reason)) {
            layer.needsKeyframe = true;
            this.deleteCache(layer.cacheKey);
        }
        // A failed optional layer does not invalidate a healthy other layer.
        const screen = publication.screens.get(message.screenId);
        const healthy = [...screen.layers.values()].some(other => ['starting', 'streaming'].includes(other.status));
        const reason = healthy ? (message.reason === 'streaming' ? 'streaming' : 'starting')
            : message.reason === 'inactive' ? 'starting' : message.reason;
        for (const entry of this.eligible(client)) {
            if (!this.relay.requested(entry, message.screenId)) continue;
            if (!healthy) {
                const output = this.relay.socket(this.relay.endpointClient(entry.session.ownerEndpointId));
                this.relay.inflight.get(output)?.waiting.delete(`${entry.streamId}:${message.screenId}`);
                const delivery = entry.screens.get(message.screenId);
                if (delivery) delivery.needsKeyframe = true;
            }
            entry.screenStatuses.set(message.screenId, reason);
            this.server.sendToEndpoint(entry.session.ownerEndpointId,
                this.relay.envelope(entry, 'screen_share_state', { enabled: true, screenId: message.screenId, reason }));
        }
        this.refresh(client);
        return true;
    }

    acknowledge(publication, header) {
        const client = publication.client;
        publication.socket.send(JSON.stringify({ type: 'screen_publication_ack', publicationId: publication.publicationId,
            screenId: header.screenId, layer: header.layer, sequence: header.sequence,
            protocolVersion: this.server.protocolVersion, serverBootId: this.server.serverBootId,
            messageId: crypto.randomUUID(), connectionGeneration: client.connectionGeneration }));
    }

    handleFrame(client, ws, parsed) {
        const { header, payload, encodedBytes } = parsed;
        const publication = this.binding(client, header.publicationId);
        if (!publication || publication.socket !== ws || header.width > (ws.screenMaximumEdge ?? 3840)
            || header.height > (ws.screenMaximumEdge ?? 3840)
            || !client.screens.some(screen => screen.id === header.screenId)) return false;
        // Ingress receipt is independent of downstream delivery. An in-flight
        // frame for a just-hidden lane is consumed without recreating demand.
        this.acknowledge(publication, header);
        const layer = this.layer(publication, header.screenId, header.layer);
        if (!layer || header.sequence <= layer.sequence || !['starting', 'streaming'].includes(layer.status)) return true;
        if ((layer.sequence && header.sequence !== layer.sequence + 1)
            || (layer.width && (header.width !== layer.width || header.height !== layer.height))) layer.needsKeyframe = true;
        layer.sequence = header.sequence;
        if (layer.needsKeyframe && !header.keyFrame) {
            this.requestKeyframe(publication, header.screenId, header.layer); return true;
        }
        layer.needsKeyframe = false;
        Object.assign(layer, { width: header.width, height: header.height });
        if (header.bitrateBps) layer.bitrateBps = header.bitrateBps;
        if (header.fps) layer.fps = header.fps;
        const now = this.server.monotonicNow();
        const frame = { header, payload, receivedAt: now, bytes: encodedBytes };
        if (header.keyFrame) this.putCache(layer.cacheKey, frame);
        for (const entry of this.eligible(client)) {
            if (!this.relay.requested(entry, header.screenId)) continue;
            this.deliver(entry, publication, header.screenId, frame);
        }
        this.refresh(client);
        return true;
    }

    requestKeyframe(publication, screenId, name) {
        if (!publication || !publication.screens.has(screenId) || !layerName(name)
            || (name === 'low' && !publication.screens.get(screenId).demand.layers.includes('low'))) return false;
        const key = `${screenId}:${name}`;
        publication.requestedKeyframes.set(key, { screenId, layer: name });
        return this.flushKeyframe(publication);
    }

    flushKeyframe(publication) {
        const now = this.server.monotonicNow();
        if (!publication.requestedKeyframes.size || now - publication.lastKeyframeAt < this.relay.keyframeRequestIntervalMs) return false;
        const [key, request] = publication.requestedKeyframes.entries().next().value;
        publication.requestedKeyframes.delete(key);
        publication.lastKeyframeAt = now;
        return this.server.sendToEndpoint(publication.client.endpointId, {
            type: 'screen_publication_keyframe', publicationId: publication.publicationId, ...request,
        });
    }

    recordFeedback(entry, screenId, sample) {
        const ws = this.relay.socket(this.relay.endpointClient(entry.session.ownerEndpointId));
        if (!ws || !this.relay.feedbackReady(entry)) return;
        const state = this.receiver(ws), now = this.server.monotonicNow(), window = this.relay.window(ws);
        const queueGrowth = sample.deliveryRttMs !== undefined && window.baselineRttMs !== null
            && sample.deliveryRttMs - (sample.serializationMs || 0) > window.baselineRttMs + this.relay.queueTargetMs;
        if (sample.congested || queueGrowth) {
            state.lastCongestionAt = now;
            if (now - state.lastDecreaseAt >= this.relay.feedbackIntervalMs) {
                state.targetBps = Math.max(this.minimumBps, Math.floor(state.targetBps * .7));
                state.lastDecreaseAt = now;
            }
        } else if (sample.deliveryRttMs !== undefined && now - state.lastCongestionAt >= this.recoveryMs
            && now - state.lastIncreaseAt >= 1000) {
            state.targetBps = Math.min(this.maximumBps, Math.ceil(state.targetBps * 1.05));
            state.lastIncreaseAt = now;
        }
        if (ws.mouffetteClient) this.receiverEstimates.set(ws.mouffetteClient, { targetBps: state.targetBps });
        this.refresh(this.relay.endpointClient(entry.session.targetEndpointId));
    }

    budget(ws) {
        let budget = this.receiver(ws).targetBps;
        if ([...(this.server.uploads?.values() || [])].some(upload => upload.targetEndpointId === ws.mouffetteClient?.endpointId))
            budget = Math.max(this.minimumBps, Math.floor(budget * this.uploadPercent / 100));
        return Math.max(1, Math.min(budget - (this.server.audioShare?.reservationFor(ws.mouffetteClient) || 0),
            this.fairEgressBps()));
    }

    fairEgressBps() {
        return Math.max(1, Math.floor(Math.max(1, this.egressBps - (this.server.audioShare?.totalReservation() || 0)) / Math.max(1, this.counts().size)));
    }

    reserveLegacyEgress(ws, bytes) {
        const now = this.server.monotonicNow(), state = this.receiver(ws);
        if (state.nextSendAt > now + this.relay.queueTargetMs || this.nextSendAt > now + this.relay.queueTargetMs) return false;
        state.nextSendAt = Math.max(now, state.nextSendAt) + bytes * 8000 / this.fairEgressBps();
        this.nextSendAt = Math.max(now, this.nextSendAt) + bytes * 8000 / this.egressBps;
        return true;
    }

    screenCount(ws) {
        return Math.max(1, this.counts().get(ws?.mouffetteClient?.endpointId) || 0);
    }

    counts() {
        if (!this.admissionCounts) {
            this.admissionCounts = new Map();
            for (const entry of this.relay.subscriptions.values()) {
                if (!entry.enabled) continue;
                const count = this.relay.effectiveSelection(entry).length;
                if (count) this.admissionCounts.set(entry.session.ownerEndpointId,
                    (this.admissionCounts.get(entry.session.ownerEndpointId) || 0) + count);
            }
        }
        return this.admissionCounts;
    }

    choose(entry, publication, screenId, budget) {
        const screen = publication.screens.get(screenId);
        if (!screen) return null;
        const cap = this.relay.envelope(entry, '').receiverMaximumEdge;
        const maximum = entry.requestedScreens?.find(item => item.screenId === screenId)?.maximumEdge ?? cap;
        const available = [...screen.layers.entries()].filter(([name, layer]) => (name === 'main' || screen.demand.layers.includes(name)) && layer.width > 0
            && Math.max(layer.width, layer.height) <= cap && ['starting', 'streaming'].includes(layer.status));
        if (!available.length) return null;
        const main = available.find(([name]) => name === 'main'), low = available.find(([name]) => name === 'low');
        const fits = candidate => candidate && (candidate[1].bitrateBps || this.initialBps) <= budget;
        let selected = fits(main) && (!low || maximum > Math.max(low[1].width, low[1].height)) ? main
            : fits(low) ? low : fits(main) ? main : low || main;
        return { layer: selected[0], snapshot: !fits(selected) };
    }

    deliver(entry, publication, screenId, candidate = null) {
        if (!this.relay.feedbackReady(entry) || !this.relay.requested(entry, screenId)) return false;
        const ws = this.relay.socket(this.relay.endpointClient(entry.session.ownerEndpointId));
        if (!ws) return false;
        const budget = this.budget(ws), choice = this.choose(entry, publication, screenId, budget / this.screenCount(ws));
        if (!choice) return false;
        let lane = entry.screens.get(screenId);
        if (!lane) {
            lane = { sequence: 0, needsKeyframe: true, layer: null, sentSources: new Map(), lastSentAt: -Infinity,
                lastReceivedAt: -Infinity };
            entry.screens.set(screenId, lane);
        }
        // A mode/profile change never admits a dependent frame from another GOP.
        const switched = lane.layer !== choice.layer;
        const screen = publication.screens.get(screenId), source = screen.layers.get(choice.layer);
        const now = this.server.monotonicNow();
        const window = this.relay.window(ws), state = this.receiver(ws);
        if (choice.snapshot && (now - lane.lastSentAt < this.snapshotMs
            || [...window.frames.values()].some(pending => pending.streamId === entry.streamId && pending.screenId === screenId))) return false;
        let frame = candidate?.header.layer === choice.layer ? candidate : null;
        if (choice.snapshot || switched || !frame) frame = this.cache.get(source.cacheKey) || frame;
        if (!frame || now - frame.receivedAt >= this.cacheTtlMs
            || frame.header.sequence <= (lane.sentSources?.get(choice.layer) || 0)
            || frame.receivedAt < lane.lastReceivedAt) {
            if ((!choice.snapshot || state.nextSendAt <= now) && (switched || lane.needsKeyframe || choice.snapshot))
                this.requestKeyframe(publication, screenId, choice.layer);
            return false;
        }
        if ((switched || lane.needsKeyframe || choice.snapshot) && !frame.header.keyFrame) {
            this.requestKeyframe(publication, screenId, choice.layer); return false;
        }
        if (!frame.header.keyFrame && frame.header.sequence !== (lane.sentSources.get(choice.layer) || 0) + 1) {
            // Delivery numbering is local to this viewer. It must never conceal
            // a gap in the encoded source chain after a cached IDR or snapshot.
            lane.needsKeyframe = true;
            this.requestKeyframe(publication, screenId, choice.layer);
            return false;
        }
        const header = { remoteSessionId: entry.session.remoteSessionId, generation: entry.generation,
            streamId: entry.streamId, screenId, sequence: lane.sequence + 1,
            width: frame.header.width, height: frame.header.height, keyFrame: frame.header.keyFrame, codec: 'h264',
            ...(frame.header.timestampUs === undefined ? {} : { timestampUs: frame.header.timestampUs }) };
        const metadata = Buffer.from(JSON.stringify(header));
        const size = metadata.length + 6 + frame.payload.length, buffered = Number(ws.bufferedAmount) || 0;
        const fairKey = `${entry.streamId}:${screenId}`;
        for (const [key, deadline] of window.waiting)
            if (now >= deadline) window.waiting.delete(key);
        const turn = window.waiting.keys().next().value;
        const delayed = this.relay.deliveryWindowDelayed(window, now);
        const cost = size * 8000 / budget, globalCost = size * 8000 / this.egressBps;
        const longSnapshot = choice.snapshot && frame.header.keyFrame && cost > this.relay.ackTimeoutMs / 2;
        if ((!choice.snapshot && cost > this.relay.ackTimeoutMs / 2)
            || (choice.snapshot && (cost > this.snapshotMaxTransferMs || now - frame.receivedAt + cost >= this.cacheTtlMs))) {
            // An intrinsically inadmissible image cannot monopolize the next
            // turn, nor trigger repeated requests for an equally large IDR.
            window.waiting.delete(fairKey);
            if (candidate) lane.needsKeyframe = true;
            return false;
        }
        const paced = state.nextSendAt > now + (choice.snapshot ? 0 : this.relay.queueTargetMs)
            || this.nextSendAt > now + this.relay.queueTargetMs;
        const unavailable = delayed || (turn !== undefined && turn !== fairKey) || paced
            || (longSnapshot && window.frames.size > 0)
            || [...window.frames.values()].some(pending => pending.serializationMs > 0)
            || window.frames.size >= this.relay.maxInflightFrames
            || (window.frames.size > 0 && window.bytes + size > this.relay.maxBufferedBytes)
            || buffered > this.relay.maxBufferedBytes || (buffered && buffered + size > this.relay.maxBufferedBytes)
            || (size > this.relay.maxBufferedBytes && !frame.header.keyFrame);
        if (unavailable) {
            if (window.waiting.has(fairKey) || window.waiting.size < 256) {
                const receiptDeadline = Math.max(now, ...[...window.frames.values()].map(pending => pending.deadlineAt || now));
                window.waiting.set(fairKey, Math.max(receiptDeadline, state.nextSendAt, now) + this.relay.ackTimeoutMs);
            }
            if (candidate && !choice.snapshot) lane.needsKeyframe = true;
            if (delayed || buffered > this.relay.maxBufferedBytes)
                this.recordFeedback(entry, screenId, { congested: true });
            if (lane.needsKeyframe) this.requestKeyframe(publication, screenId, choice.layer);
            return false;
        }
        window.waiting.delete(fairKey);
        const prefix = Buffer.alloc(6); prefix.write('MSV1'); prefix.writeUInt16BE(metadata.length, 4);
        const packet = Buffer.concat([prefix, metadata, frame.payload], size);
        const key = `${entry.streamId}:${screenId}:${header.sequence}`;
        const pending = { bytes: size, sentAt: now, remoteSessionId: entry.session.remoteSessionId,
            streamId: entry.streamId, screenId, shared: true,
            ...(longSnapshot ? { serializationMs: cost,
                deadlineAt: now + Math.max(this.relay.ackTimeoutMs,
                    cost + (window.baselineRttMs ?? this.relay.ackTimeoutMs) + this.relay.queueTargetMs) } : {}),
        };
        window.frames.set(key, pending); window.bytes += size;
        lane.sequence = header.sequence; lane.layer = choice.layer; lane.needsKeyframe = false;
        lane.sentSources.set(choice.layer, frame.header.sequence);
        lane.lastSentAt = now; lane.lastReceivedAt = frame.receivedAt;
        state.nextSendAt = Math.max(now, state.nextSendAt) + cost;
        this.nextSendAt = Math.max(now, this.nextSendAt) + globalCost;
        const failed = () => {
            if (window.frames.get(key) === pending) { window.frames.delete(key); window.bytes -= size; }
            lane.needsKeyframe = true;
            this.recordFeedback(entry, screenId, { congested: true });
        };
        try { ws.send(packet, { binary: true, compress: false }, error => { if (error) failed(); }); }
        catch (_) { failed(); return false; }
        return true;
    }

    putCache(key, frame) {
        this.deleteCache(key);
        if (frame.bytes > this.cacheLimit) return;
        while (this.cache.size && this.cacheBytes + frame.bytes > this.cacheLimit)
            this.deleteCache(this.cache.keys().next().value);
        // Incoming ws buffers may be slices of a larger pooled allocation.
        // One exact, unpooled copy per cached IDR keeps retained pixel bytes
        // bounded independently of those slabs; every viewer shares this copy.
        const payload = Buffer.allocUnsafeSlow(frame.payload.length);
        frame.payload.copy(payload);
        this.cache.set(key, { ...frame, payload }); this.cacheBytes += frame.bytes;
    }

    deleteCache(key) {
        const previous = this.cache.get(key);
        if (!previous) return;
        this.cacheBytes -= previous.bytes; this.cache.delete(key);
    }

    sweep() {
        const now = this.server.monotonicNow();
        for (const [key, frame] of this.cache)
            if (now - frame.receivedAt >= this.cacheTtlMs) this.deleteCache(key);
        for (const ws of this.receivers.keys())
            if (ws.readyState !== 1) this.receivers.delete(ws);
        for (const publication of this.byClient.values()) {
            this.refresh(publication.client);
            if (this.byClient.get(publication.client) !== publication) continue;
            for (const entry of this.eligible(publication.client))
                for (const screenId of publication.screens.keys()) this.deliver(entry, publication, screenId);
            this.flushKeyframe(publication);
        }
    }
}

module.exports = { ScreenPublications, parsePublicationFrame };
