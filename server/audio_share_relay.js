'use strict';

const crypto = require('node:crypto');

// Audio has an independent TCP pipe: a large video IDR must not serialize
// ahead of the next 20 ms of sound. Identity lives in the authenticated grant,
// not a JSON envelope repeated fifty times a second.
const HEADER_BYTES = 36;
const MAX_PAYLOAD_BYTES = 1275;
const MAX_SAFE = BigInt(Number.MAX_SAFE_INTEGER);
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const integer = (v, lo, hi = Number.MAX_SAFE_INTEGER) => Number.isSafeInteger(v) && v >= lo && v <= hi;
const opaque = v => typeof v === 'string' && /^[A-Za-z0-9_-]{1,128}$/.test(v);
function uuidBytes(id) { return Buffer.from(id.replaceAll('-', ''), 'hex'); }
function uuidString(bytes) {
    const value = bytes.toString('hex');
    return `${value.slice(0,8)}-${value.slice(8,12)}-${value.slice(12,16)}-${value.slice(16,20)}-${value.slice(20)}`;
}
function encodeAudioPacket(epoch, sequence, timestampUs, payload) {
    const header = Buffer.alloc(HEADER_BYTES);
    header.write('MAU1'); uuidBytes(epoch).copy(header, 4);
    header.writeBigUInt64BE(BigInt(sequence), 20); header.writeBigUInt64BE(BigInt(timestampUs), 28);
    return Buffer.concat([header, payload]);
}
function parseAudioPacket(data) {
    if (!Buffer.isBuffer(data) || data.length <= HEADER_BYTES || data.length > HEADER_BYTES + MAX_PAYLOAD_BYTES
        || data.toString('ascii', 0, 4) !== 'MAU1') return null;
    const epoch = uuidString(data.subarray(4,20)), sequence = data.readBigUInt64BE(20), timestamp = data.readBigUInt64BE(28);
    if (!UUID.test(epoch) || sequence < 1n || sequence > MAX_SAFE || timestamp > MAX_SAFE) return null;
    return { epoch, sequence: Number(sequence), timestampUs: Number(timestamp), payload: data.subarray(HEADER_BYTES) };
}
function encodeAudioAck(epoch, sequence) {
    const packet = Buffer.alloc(28); packet.write('MAA1'); uuidBytes(epoch).copy(packet, 4);
    packet.writeBigUInt64BE(BigInt(sequence), 20); return packet;
}
function parseAudioAck(data) {
    if (!Buffer.isBuffer(data) || data.length !== 28 || data.toString('ascii',0,4) !== 'MAA1') return null;
    const epoch = uuidString(data.subarray(4,20)), sequence = data.readBigUInt64BE(20);
    return UUID.test(epoch) && sequence > 0n && sequence <= MAX_SAFE ? { epoch, sequence: Number(sequence) } : null;
}
const reservation = bitrate => bitrate === 32000 ? 48000 : 112000;

class AudioShareRelay {
    constructor(server) {
        this.server = server;
        this.tokens = new Map(); this.publishSockets = new Map(); this.viewSockets = new Map();
        this.publications = new Map(); this.subscriptions = new Map(); this.windows = new Map();
        this.maxViewers = server.config?.screenMaxViewersPerPublisher ?? 10;
        this.maxPublications = server.config?.screenMaxPublications ?? 256;
        this.ackTimeoutMs = 3000; this.maxBufferedBytes = 32768; this.maxInflight = 128;
        this.queueTargetMs = 150; this.lastSweep = 0; this.nextEgressAt = 0; this.deliveryTurn = 0;
    }
    current(client) { return this.server.screenShare.current(client); }
    client(endpoint) { return this.server.currentTransportByEndpoint.get(endpoint); }
    socket(client, role) {
        const ws = (role === 'publish' ? this.publishSockets : this.viewSockets).get(client);
        return this.current(client) && ws?.readyState === 1 ? ws : null;
    }
    send(client, message) { return this.server.sendToEndpoint(client.endpointId, message); }
    issueToken(client, message) {
        if (!opaque(message.requestId) || !['publish','view'].includes(message.role) || message.audioVersion !== 1) return false;
        const now = this.server.monotonicNow();
        for (const [token,binding] of this.tokens)
            if (binding.expiresAt <= now || (binding.client === client && binding.role === message.role)) this.tokens.delete(token);
        const token = crypto.randomBytes(32).toString('base64url');
        this.tokens.set(token, { client, role: message.role, generation: client.connectionGeneration, expiresAt: now + 15000 });
        return this.send(client, { type:'audio_channel_token', requestId:message.requestId, token, role:message.role, audioVersion:1 });
    }
    acceptSocket(ws, token) {
        const binding = this.tokens.get(token); this.tokens.delete(token);
        if (!binding || binding.expiresAt <= this.server.monotonicNow() || !this.current(binding.client)
            || binding.generation !== binding.client.connectionGeneration) {
            ws.close(1008,'Audio authentication failed'); return;
        }
        const { client, role } = binding, sockets = role === 'publish' ? this.publishSockets : this.viewSockets;
        const previous = sockets.get(client);
        if (previous) { sockets.delete(client); this.refreshAll(); this.windows.delete(previous); previous.close(1008,'Audio channel replaced'); }
        ws.mouffetteClient = client; ws.audioRole = role; sockets.set(client,ws);
        ws.on('message',(data,binary) => {
            if (!this.current(client) || sockets.get(client) !== ws) { ws.close(1008,'Stale audio channel'); return; }
            if (!binary) { ws.close(1008,'Audio requires binary packets'); return; }
            const valid = role === 'publish' ? this.handleFrame(client,ws,data) : this.handleAck(client,ws,data);
            if (!valid) ws.close(1008,'Invalid audio packet');
        });
        const closed = () => {
            this.windows.delete(ws);
            if (sockets.get(client) !== ws) return;
            sockets.delete(client); this.refreshAll();
        };
        ws.on('close',closed); ws.on('error',closed);
        ws.send(JSON.stringify({type:'audio_channel_ready',audioVersion:1,role,
            protocolVersion:this.server.protocolVersion,serverBootId:this.server.serverBootId,
            messageId:crypto.randomUUID(),connectionGeneration:client.connectionGeneration}));
        this.refreshAll();
    }
    handleControl(clientId,message) {
        const client = this.server.clients.get(clientId);
        if (!this.current(client)) return false;
        if (message.type === 'request_audio_channel') return this.issueToken(client,message);
        if (message.type === 'audio_share_consent') {
            if (typeof message.enabled !== 'boolean' || message.audioVersion !== 1) return false;
            client.audioVersion = 1; client.audioSharingEnabled = message.enabled;
            this.refreshAll(); return true;
        }
        if (message.type === 'audio_source_budget') {
            if (!integer(message.totalBps,128000,100000000) || typeof message.congested !== 'boolean') return false;
            client.audioBudgetBps = message.totalBps;
            client.audioCongestedUntil = message.congested ? this.server.monotonicNow() + 1000 : 0;
            this.updateProfiles(); return true;
        }
        if (message.type === 'audio_publication_status') {
            const publication = this.publications.get(client);
            if (!publication || publication.id !== message.publicationId
                || !['starting','streaming','permission_denied','unavailable','capture_error'].includes(message.reason)) return false;
            publication.reason = message.reason;
            for (const entry of this.subscriptions.values()) if (entry.publication === publication) this.sendState(entry,true,message.reason);
            return true;
        }
        const valid = this.server.validateSessionMessage(clientId,message,{allowUnready:true});
        if (!valid.ok || valid.role !== 'owner') return false;
        const { session } = valid;
        if (message.type === 'audio_share_subscribe') {
            if (typeof message.enabled !== 'boolean') return false;
            if (!message.enabled) { this.removeSession(session,'unsubscribed'); return true; }
            let entry = this.subscriptions.get(session.remoteSessionId);
            if (entry && entry.generation !== session.generation) { this.removeSession(session,'session_changed'); entry = null; }
            if (!entry) {
                const count = [...this.subscriptions.values()].filter(v => v.session.targetEndpointId === session.targetEndpointId).length;
                if (count >= this.maxViewers || this.subscriptions.size >= this.maxPublications * this.maxViewers) return false;
                entry = {session,generation:session.generation,streamId:'',publication:null,reason:'',sequence:0,
                    congestedUntil:0,lastFeedback:-Infinity,targetBps:this.server.config?.screenViewerInitialBps ?? 3000000};
                this.subscriptions.set(session.remoteSessionId,entry);
            }
            this.refreshAll(); this.sendState(entry,!!entry.publication,entry.reason); return true;
        }
        if (message.type === 'audio_view_feedback') {
            const entry = this.subscriptions.get(session.remoteSessionId), now = this.server.monotonicNow();
            if (!integer(message.droppedPackets,0,10000) || !integer(message.bufferedMs,0,10000)) return false;
            if (!entry || entry.streamId !== message.streamId || !entry.publication) return true;
            if (now-entry.lastFeedback < 500) return true;
            entry.lastFeedback = now;
            if (message.droppedPackets || message.bufferedMs > this.queueTargetMs) this.congested(entry,now);
            this.updateProfiles(); return true;
        }
        return false;
    }
    allowed(entry) {
        const s = this.server.remoteSessions.get(entry.session.remoteSessionId);
        if (!s || s.generation !== entry.generation || !this.server.remoteSessions.commandReady(s)) return 'session_unavailable';
        const owner = this.client(s.ownerEndpointId), target = this.client(s.targetEndpointId);
        if (!this.current(owner) || !this.current(target) || owner.runtimeId !== s.ownerRuntimeId || target.runtimeId !== s.targetRuntimeId
            || owner.connectionGeneration !== s.ownerConnectionGeneration || target.connectionGeneration !== s.targetConnectionGeneration) return 'session_unavailable';
        if (target.audioVersion !== 1) return 'unsupported';
        if (!target.screenSharingEnabled || !target.audioSharingEnabled) return 'disabled';
        if (!this.socket(owner,'view') || !this.socket(target,'publish')) return 'channel_unavailable';
        return 'ready';
    }
    sendState(entry,enabled,reason) {
        entry.reason = reason;
        this.send(this.client(entry.session.ownerEndpointId) || {endpointId:entry.session.ownerEndpointId}, {
            type:'audio_share_state',remoteSessionId:entry.session.remoteSessionId,generation:entry.generation,
            enabled,reason,streamId:entry.streamId,publicationId:entry.publication?.id || '',bitrateBps:entry.publication?.bitrate || 96000});
    }
    clearEpoch(epoch) {
        if (!epoch) return;
        for (const window of this.windows.values()) for (const [key,pending] of window.frames)
            if (pending.epoch === epoch) { window.frames.delete(key); window.bytes -= pending.bytes; }
    }
    refreshAll() {
        // Epochs depend on audio transport/session/consent, never screen topology.
        for (const [client,p] of this.publications) {
            const wanted = [...this.subscriptions.values()].some(e => e.session.targetEndpointId === client.endpointId && this.allowed(e) === 'ready');
            if (!wanted || this.socket(client,'publish') !== p.socket) {
                this.publications.delete(client);
                this.send(client,{type:'audio_publication_request',enabled:false,publicationId:p.id,bitrateBps:p.bitrate});
            }
        }
        for (const entry of this.subscriptions.values()) {
            let reason = this.allowed(entry), publication = null;
            if (reason === 'ready') {
                const target = this.client(entry.session.targetEndpointId);
                publication = this.publications.get(target);
                if (!publication && this.publications.size < this.maxPublications) {
                    publication = {client:target,socket:this.socket(target,'publish'),id:crypto.randomUUID(),bitrate:96000,
                        sequence:0,timestampUs:-1,nextIngressAt:0,badSince:null,healthySince:null,reason:'starting'};
                    this.publications.set(target,publication);
                    this.send(target,{type:'audio_publication_request',enabled:true,publicationId:publication.id,bitrateBps:publication.bitrate});
                }
                if (!publication) reason = 'capacity_limited';
            }
            if (entry.publication !== publication) {
                this.clearEpoch(entry.streamId); entry.publication = publication;
                entry.streamId = publication ? crypto.randomUUID() : ''; entry.sequence = 0;
                this.sendState(entry,!!publication,publication ? publication.reason : reason);
            } else if (!publication && entry.reason !== reason) this.sendState(entry,false,reason);
        }
        this.updateProfiles();
    }
    refreshSession() { this.refreshAll(); }
    viewerBudget(entry) {
        const client = this.client(entry.session.ownerEndpointId), ws = this.server.screenShare.socket(client);
        let budget = entry.targetBps;
        if (ws) budget = Math.min(budget,this.server.screenShare.shared.receiver(ws).targetBps);
        if ([...this.server.uploads.values()].some(u => u.targetEndpointId === client?.endpointId)) budget *= .4;
        return Math.max(1,Math.min(Math.max(64000,Math.floor(budget)),this.audioFairEgressBps()));
    }
    updateProfiles() {
        const now = this.server.monotonicNow();
        for (const publication of this.publications.values()) {
            const entries = [...this.subscriptions.values()].filter(e => e.publication === publication);
            const budget = Math.min(publication.client.audioBudgetBps ?? 1200000,...entries.map(e => this.viewerBudget(e)));
            const bad = budget < 256000 || (publication.client.audioCongestedUntil || 0) > now || entries.some(e => e.congestedUntil > now);
            publication.badSince = bad ? publication.badSince ?? now : null;
            const healthy = !bad && budget >= 384000;
            publication.healthySince = healthy ? publication.healthySince ?? now : null;
            const next = publication.bitrate === 96000 && publication.badSince !== null && now-publication.badSince >= 1000 ? 32000
                : publication.bitrate === 32000 && publication.healthySince !== null && now-publication.healthySince >= 10000 ? 96000 : publication.bitrate;
            if (next !== publication.bitrate) {
                publication.bitrate = next;
                this.send(publication.client,{type:'audio_publication_request',enabled:true,publicationId:publication.id,bitrateBps:next});
                for (const entry of entries) this.sendState(entry,true,publication.reason);
            }
        }
    }
    reservationFor(client) {
        return [...this.subscriptions.values()].filter(e => e.publication && e.session.ownerEndpointId === client?.endpointId)
            .reduce((sum,e) => sum + reservation(e.publication.bitrate),0);
    }
    totalReservation() {
        const requested = [...this.subscriptions.values()].reduce((sum,e) => sum + (e.publication ? reservation(e.publication.bitrate) : 0),0);
        return Math.min(requested,Math.max(1,(this.server.config?.screenServerEgressBps ?? 100000000)-16000));
    }
    audioFairEgressBps() {
        const count = [...this.subscriptions.values()].filter(e => e.publication).length;
        return Math.max(1,Math.floor((this.server.config?.screenServerEgressBps ?? 100000000) / Math.max(1,count)));
    }
    window(ws) {
        let window = this.windows.get(ws);
        if (!window) { window = {frames:new Map(),bytes:0,baseline:null,baselineAt:0,nextSendAt:0}; this.windows.set(ws,window); }
        return window;
    }
    congested(entry,now) {
        entry.congestedUntil = now+1500;
        if (now-(entry.lastDecrease ?? -Infinity) >= 500) {
            entry.targetBps = Math.max(64000,Math.floor(entry.targetBps*.7)); entry.lastDecrease = now;
        }
    }
    handleFrame(client,ws,data) {
        const frame = parseAudioPacket(data);
        if (!frame) return false;
        const publication = this.publications.get(client);
        // Delayed valid packets cannot revive retired epochs; consuming them is
        // harmless, but only current authority earns publisher receipt credit.
        if (!this.current(client) || this.socket(client,'publish') !== ws || !publication || publication.id !== frame.epoch) return true;
        if (!client.screenSharingEnabled || !client.audioSharingEnabled) return true;
        if (frame.sequence <= publication.sequence || frame.timestampUs < publication.timestampUs) return true;
        publication.sequence = frame.sequence; publication.timestampUs = frame.timestampUs;
        if ((ws.bufferedAmount || 0) > this.maxBufferedBytes) { this.abort(ws); return true; }
        ws.send(encodeAudioAck(frame.epoch,frame.sequence),{binary:true,compress:false});
        if (!['starting','streaming'].includes(publication.reason)) return true;
        const now = this.server.monotonicNow();
        // Bound malicious ingress bursts without retaining compressed sound.
        if (publication.nextIngressAt > now + 100) return true;
        publication.nextIngressAt = Math.max(now,publication.nextIngressAt)+20;
        const entries = [...this.subscriptions.values()].filter(e => e.publication === publication);
        const aggregateBudget = Math.max(1,this.totalReservation());
        const offset = entries.length ? this.deliveryTurn++ % entries.length : 0;
        for (let index = 0; index < entries.length; ++index) {
            const entry = entries[(index+offset)%entries.length];
            if (entry.publication !== publication || this.allowed(entry) !== 'ready') continue;
            const output = this.socket(this.client(entry.session.ownerEndpointId),'view'), window = this.window(output);
            const oldest = window.frames.values().next().value;
            const grace = window.baseline === null ? 1000 : window.baseline + this.queueTargetMs;
            if (window.frames.size >= this.maxInflight || window.bytes+data.length > this.maxBufferedBytes
                || (output.bufferedAmount || 0)+data.length > this.maxBufferedBytes
                || (oldest && now-oldest.sentAt > grace) || window.nextSendAt > now+100 || this.nextEgressAt > now+100) {
                this.congested(entry,now); continue;
            }
            const sequence = ++entry.sequence, packet = encodeAudioPacket(entry.streamId,sequence,frame.timestampUs,frame.payload);
            const key = `${entry.streamId}:${sequence}`, pending = {epoch:entry.streamId,sequence,bytes:packet.length,sentAt:now,entry};
            window.frames.set(key,pending); window.bytes += packet.length;
            window.nextSendAt = Math.max(now,window.nextSendAt)+packet.length*8000/Math.min(reservation(publication.bitrate),this.viewerBudget(entry));
            this.nextEgressAt = Math.max(now,this.nextEgressAt)+packet.length*8000/aggregateBudget;
            const failed = () => { if (window.frames.get(key) === pending) { window.frames.delete(key); window.bytes -= pending.bytes; } this.congested(entry,now); };
            try { output.send(packet,{binary:true,compress:false},error => { if(error) failed(); }); } catch (_) { failed(); }
        }
        return true;
    }
    handleAck(client,ws,data) {
        const ack = parseAudioAck(data); if (!ack) return false;
        if (!this.current(client) || this.socket(client,'view') !== ws) return false;
        const window = this.window(ws), key = `${ack.epoch}:${ack.sequence}`, pending = window.frames.get(key);
        if (!pending) return true;
        window.frames.delete(key); window.bytes -= pending.bytes;
        const now = this.server.monotonicNow(), rtt = now-pending.sentAt;
        if (window.baseline === null || rtt <= window.baseline || now-window.baselineAt > 30000) { window.baseline=rtt; window.baselineAt=now; }
        if (rtt > window.baseline+this.queueTargetMs) this.congested(pending.entry,now);
        else if (now-pending.entry.congestedUntil > 3000 && now-(pending.entry.lastIncrease ?? 0) > 1000) {
            pending.entry.targetBps = Math.min(this.server.config?.screenViewerMaxBps ?? 20000000,Math.ceil(pending.entry.targetBps*1.05));
            pending.entry.lastIncrease = now;
        }
        return true;
    }
    abort(ws) { if (typeof ws.terminate === 'function') ws.terminate(); else ws.close(1008,'Audio receipt timeout'); }
    sweep() {
        const now = this.server.monotonicNow();
        for (const [token,binding] of this.tokens) if (binding.expiresAt <= now || !this.current(binding.client)) this.tokens.delete(token);
        for (const [ws,window] of this.windows) {
            const oldest = window.frames.values().next().value;
            if (oldest && now-oldest.sentAt >= this.ackTimeoutMs) { this.windows.delete(ws); this.abort(ws); }
            else if (oldest && window.baseline !== null && now-oldest.sentAt > window.baseline+this.queueTargetMs)
                this.congested(oldest.entry,now);
        }
        this.refreshAll();
    }
    removeSession(session,reason='session_unavailable') {
        const entry = this.subscriptions.get(session.remoteSessionId); if (!entry) return;
        this.clearEpoch(entry.streamId); entry.streamId=''; entry.publication=null; this.sendState(entry,false,reason);
        this.subscriptions.delete(session.remoteSessionId); this.refreshAll();
    }
    revokeClient(client) {
        for (const [token,binding] of this.tokens) if (binding.client === client) this.tokens.delete(token);
        for (const sockets of [this.publishSockets,this.viewSockets]) {
            const ws=sockets.get(client); sockets.delete(client);
            if(ws) { this.windows.delete(ws); ws.close(1008,'Audio control disconnected'); }
        }
        for(const entry of [...this.subscriptions.values()])
            if ([entry.session.ownerEndpointId,entry.session.targetEndpointId].includes(client.endpointId)) this.removeSession(entry.session);
        this.refreshAll();
    }
}
module.exports = {AudioShareRelay,parseAudioPacket,encodeAudioPacket,parseAudioAck,encodeAudioAck,HEADER_BYTES,MAX_PAYLOAD_BYTES};
