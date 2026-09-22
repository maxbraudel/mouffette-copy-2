'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const {EventEmitter,once} = require('node:events');
const WebSocket = require('ws');
const {MouffetteServer} = require('./server');
const {loadServerConfig} = require('./config');
const {encodeAudioPacket,parseAudioPacket,encodeAudioAck,parseAudioAck,MAX_PAYLOAD_BYTES} = require('./audio_share_relay');

function socket() {
    return Object.assign(new EventEmitter(),{readyState:1,bufferedAmount:0,messages:[],
        send(data,options,done) { this.messages.push(Buffer.isBuffer(data) ? data : JSON.parse(data)); done?.(); },
        close(code,reason) { if(this.readyState===3) return; this.readyState=3; this.closed={code,reason}; this.emit('close'); }});
}
function context(config = {}) {
    let now=100;
    const server=new MouffetteServer({config:{...loadServerConfig({envFile:'/nonexistent'}),...config},port:0,host:'127.0.0.1',
        monotonicNow:()=>now,protocolLogger:()=>{},metricLogger:()=>{}}),relay=server.audioShare;
    const peers=[];
    const client=name=> {
        const peer={id:name,endpointId:name,runtimeId:`${name}-runtime`,connectionGeneration:1,
            authenticated:true,ws:socket(),screens:[{id:0,width:640,height:360}]};
        server.clients.set(name,peer); server.currentTransportByEndpoint.set(name,peer); peers.push(peer); return peer;
    };
    const control=(peer,type,fields={})=>relay.handleControl(peer.id,{type,connectionGeneration:peer.connectionGeneration,...fields});
    const token=(peer,role)=>{ assert.equal(control(peer,'request_audio_channel',{role,audioVersion:1,requestId:crypto.randomUUID()}),true); return peer.ws.messages.at(-1); };
    const connect=(peer,role)=>{ const issued=token(peer,role),ws=socket(); relay.acceptSocket(ws,issued.token); return ws; };
    const consent=(peer,enabled=true)=>{
        server.screenShare.handleControl(peer.id,{type:'screen_share_consent',enabled});
        return control(peer,'audio_share_consent',{enabled,audioVersion:1});
    };
    const subscribe=(owner,target)=>{
        const session=server.remoteSessions.open({ownerEndpointId:owner.endpointId,targetEndpointId:target.endpointId,
            ownerRuntimeId:owner.runtimeId,targetRuntimeId:target.runtimeId,ownerConnectionGeneration:1,targetConnectionGeneration:1}).session;
        const message={remoteSessionId:session.remoteSessionId,generation:session.generation,enabled:true};
        assert.equal(control(owner,'audio_share_subscribe',message),true);
        return {session,message,entry:relay.subscriptions.get(session.remoteSessionId)};
    };
    const tick=ms=>{ now+=ms; for(const peer of peers) peer.lastHeartbeatMonotonicAt=now;
        for(const session of server.remoteSessions.sessions.values()) for(const peer of peers)
            if(session.lastContact.has(peer.endpointId)) session.lastContact.set(peer.endpointId,now); };
    return {server,relay,client,control,token,connect,consent,subscribe,tick};
}
const frames=ws=>ws.messages.filter(Buffer.isBuffer).map(parseAudioPacket).filter(Boolean);
const receipts=ws=>ws.messages.filter(Buffer.isBuffer).map(parseAudioAck).filter(Boolean);
const frame=(publication,sequence=1,timestamp=20000)=>encodeAudioPacket(publication.id,sequence,timestamp,Buffer.from([0xfc,1,2,3]));

// Compact framing, integer bounds, strict message kinds and truncated payloads.
{
    const epoch=crypto.randomUUID(),payload=Buffer.from([0xfc,1,2]),packet=encodeAudioPacket(epoch,7,123456,payload);
    assert.deepEqual(parseAudioPacket(packet),{epoch,sequence:7,timestampUs:123456,payload});
    assert.equal(packet.length,36+payload.length);
    assert.deepEqual(parseAudioAck(encodeAudioAck(epoch,7)),{epoch,sequence:7});
    assert.equal(parseAudioPacket(encodeAudioAck(epoch,7)),null);
    assert.equal(parseAudioAck(packet),null);
    assert.equal(parseAudioPacket(packet.subarray(0,36)),null);
    assert.equal(parseAudioPacket(encodeAudioPacket(epoch,1,0,Buffer.alloc(MAX_PAYLOAD_BYTES+1))),null);
    const invalid=Buffer.from(packet); invalid.writeBigUInt64BE(9007199254740992n,28);
    assert.equal(parseAudioPacket(invalid),null);
}
// A single source packet reaches each viewer once, independent of monitor count,
// viewport and video topology. Source receipt does not wait for viewers.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewers=[];
    for(let i=0;i<3;++i) { const owner=c.client(`viewer${i}`),output=c.connect(owner,'view'); viewers.push({owner,output,...c.subscribe(owner,source)}); }
    const publication=c.relay.publications.get(source);
    assert.equal(c.relay.publications.size,1);
    input.emit('message',frame(publication),true);
    assert.equal(receipts(input).length,1);
    for(const v of viewers) { assert.equal(frames(v.output).length,1); assert.equal(frames(v.output)[0].epoch,v.entry.streamId); }
    const id=publication.id,stream=viewers[0].entry.streamId;
    source.screens.push({id:1,width:1920,height:1080},{id:2,width:1920,height:1080});
    c.relay.refreshSession(viewers[0].session);
    assert.equal(c.relay.publications.get(source).id,id); assert.equal(viewers[0].entry.streamId,stream);
    assert.equal(c.relay.reservationFor(viewers[0].owner),112000);
    c.relay.handleAck(viewers[0].owner,viewers[0].output,encodeAudioAck(stream,999));
    assert.equal(c.relay.window(viewers[0].output).frames.size,1);
    c.relay.handleAck(viewers[0].owner,viewers[0].output,encodeAudioAck(stream,1));
    assert.equal(c.relay.window(viewers[0].output).frames.size,0);
    for(const v of viewers) c.control(v.owner,'audio_share_subscribe',{...v.message,enabled:false});
    assert.equal(c.relay.publications.size,0); assert.equal(c.relay.totalReservation(),0);
}
// Consent, session authority, socket role and control transport generations are
// checked independently; old packets cannot restore a revoked grant.
{
    const c=context(),source=c.client('source'),viewer=c.client('viewer'),stranger=c.client('stranger');
    const input=c.connect(source,'publish'),output=c.connect(viewer,'view');
    const {entry,session,message}=c.subscribe(viewer,source);
    assert.equal(entry.reason,'unsupported'); c.consent(source); assert.ok(entry.publication);
    const publication=entry.publication,oldStream=entry.streamId;
    assert.equal(c.control(stranger,'audio_share_subscribe',message),false);
    c.consent(source,false); assert.equal(entry.streamId,''); assert.equal(c.relay.publications.size,0);
    input.emit('message',frame(publication),true); assert.equal(frames(output).length,0); assert.equal(receipts(input).length,0);
    c.consent(source,true); assert.notEqual(entry.publication.id,publication.id); assert.notEqual(entry.streamId,oldStream);
    const old=entry.publication; source.connectionGeneration=2; c.relay.refreshAll();
    assert.equal(entry.publication,null);
    assert.equal(c.relay.handleFrame(source,input,frame(old)),true); assert.equal(frames(output).length,0);
    source.connectionGeneration=1; c.relay.refreshAll();
    output.emit('message',frame(entry.publication),true); assert.equal(output.closed.code,1008);
    c.relay.removeSession(session); assert.equal(c.relay.subscriptions.size,0);
}
// A replacement publish pipe rotates all matching epochs; tokens are single-use
// and invalid after expiry, replacement of the control object or lease loss.
{
    const c=context(),source=c.client('source'),viewer=c.client('viewer');
    c.connect(source,'publish'); c.connect(viewer,'view'); c.consent(source);
    const {entry}=c.subscribe(viewer,source),old=entry.publication.id,oldView=entry.streamId;
    c.connect(source,'publish'); assert.notEqual(entry.publication.id,old); assert.notEqual(entry.streamId,oldView);
    const grant=c.token(source,'publish'),first=socket(); c.relay.acceptSocket(first,grant.token);
    const reused=socket(); c.relay.acceptSocket(reused,grant.token); assert.equal(reused.closed.code,1008);
    const expired=c.token(source,'publish'); c.tick(15000); const stale=socket(); c.relay.acceptSocket(stale,expired.token); assert.equal(stale.closed.code,1008);
    c.relay.revokeClient(source); assert.equal(c.relay.subscriptions.size,0); assert.equal(c.relay.publications.size,0);
}
// Exact receipt windows isolate a slow viewer; a healthy viewer continues. Old
// queues are disposed without touching the command or video connections.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const fast=c.client('fast'),fastWs=c.connect(fast,'view'),a=c.subscribe(fast,source);
    const slow=c.client('slow'),slowWs=c.connect(slow,'view'),b=c.subscribe(slow,source);
    slowWs.bufferedAmount=32768;
    for(let i=1;i<=10;++i) {
        c.tick(20); input.emit('message',frame(a.entry.publication,i,i*20000),true);
        const received=frames(fastWs).at(-1); c.relay.handleAck(fast,fastWs,encodeAudioAck(received.epoch,received.sequence));
    }
    assert.equal(frames(fastWs).length,10); assert.equal(frames(slowWs).length,0);
    assert.equal(c.relay.window(fastWs).bytes,0); assert.equal(c.relay.window(slowWs).bytes,0);
    slowWs.bufferedAmount=0; c.tick(20); input.emit('message',frame(a.entry.publication,11,220000),true);
    c.relay.handleAck(fast,fastWs,encodeAudioAck(a.entry.streamId,11));
    c.tick(3000); c.relay.sweep();
    assert.equal(slowWs.closed.code,1008); assert.equal(fastWs.readyState,1); assert.equal(source.ws.readyState,1);
    assert.equal(b.entry.publication,null);
}
// 96/32 kb/s changes have separate down/up holds. Audio reserves a share of
// existing media budgets, and never resets the video's source publication.
{
    const c=context(),source=c.client('source'); c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'); c.connect(viewer,'view'); const {entry}=c.subscribe(viewer,source);
    const publication=entry.publication;
    c.control(source,'audio_source_budget',{totalBps:128000,congested:false});
    c.tick(999); c.relay.updateProfiles(); assert.equal(publication.bitrate,96000);
    c.tick(1); c.relay.updateProfiles(); assert.equal(publication.bitrate,32000);
    assert.equal(c.relay.reservationFor(viewer),48000);
    c.control(source,'audio_source_budget',{totalBps:1200000,congested:false});
    c.tick(9999); c.relay.updateProfiles(); assert.equal(publication.bitrate,32000);
    c.tick(1); c.relay.updateProfiles(); assert.equal(publication.bitrate,96000);
    entry.targetBps=64000; c.relay.updateProfiles(); c.tick(1000); c.relay.updateProfiles(); assert.equal(publication.bitrate,32000);
    assert.equal(source.audioBudgetBps,1200000);
}

// A publishing TCP backlog is acknowledged and discarded, never rebased as
// current audio. The next live packet resumes without replaying that backlog.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'),output=c.connect(viewer,'view'),{entry}=c.subscribe(viewer,source);
    const publication=entry.publication;
    input.emit('message',frame(publication,1,1000000),true);
    c.relay.handleAck(viewer,output,encodeAudioAck(entry.streamId,1));
    c.tick(2000);
    for(let sequence=2;sequence<=11;++sequence) {
        input.emit('message',frame(publication,sequence,1000000+(sequence-1)*20000),true);
        c.tick(20);
    }
    assert.equal(frames(output).length,1,'seconds-old audio must not establish a new arrival baseline');
    assert.equal(receipts(input).length,11,'discarded stale audio still frees its exact source credit');
    input.emit('message',frame(publication,12,3200000),true);
    assert.equal(frames(output).length,2);
    assert.equal(frames(output).at(-1).timestampUs,3200000);
}
// A late first ACK cannot teach the relay that seconds of latency are normal.
// Only its disposable audio socket is retired; other viewers continue live.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const slow=c.client('slow'),slowWs=c.connect(slow,'view'),slowView=c.subscribe(slow,source);
    const fast=c.client('fast'),fastWs=c.connect(fast,'view'),fastView=c.subscribe(fast,source);
    const publication=fastView.entry.publication;
    input.emit('message',frame(publication,1,1000000),true);
    c.relay.handleAck(fast,fastWs,encodeAudioAck(fastView.entry.streamId,1));
    c.tick(2001);
    c.relay.handleAck(slow,slowWs,encodeAudioAck(slowView.entry.streamId,1));
    assert.equal(slowWs.readyState,3); assert.equal(slow.ws.readyState,1);
    input.emit('message',frame(publication,2,3001000),true);
    assert.equal(frames(fastWs).length,2); assert.equal(fastWs.readyState,1);
}

// A permanent ingress route shift recovers through a new disposable source
// publication. It never makes old buffered audio current in the old epoch.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'),output=c.connect(viewer,'view'),{entry}=c.subscribe(viewer,source);
    const publication=entry.publication,oldStream=entry.streamId;
    input.emit('message',frame(publication,1,1000000),true);
    c.relay.handleAck(viewer,output,encodeAudioAck(oldStream,1));
    c.tick(220);
    for(let sequence=2;sequence<=52;++sequence) {
        input.emit('message',frame(publication,sequence,1000000+(sequence-1)*20000),true);
        c.tick(20);
    }
    assert.equal(input.readyState,3); assert.equal(frames(output).length,1);
    assert.equal(source.ws.readyState,1);
    const resumed=c.connect(source,'publish'),next=entry.publication;
    assert.notEqual(next.id,publication.id); assert.notEqual(entry.streamId,oldStream);
    resumed.emit('message',frame(publication,53,2040000),true);
    assert.equal(frames(output).length,1,'retired source bytes cannot cross the epoch fence');
    resumed.emit('message',frame(next,1,2260000),true);
    assert.equal(frames(output).length,2); assert.equal(frames(output).at(-1).epoch,entry.streamId);
}

// Replaying a compressed backlog in one turn cannot trigger a clock rebase
// or flush a healthy source publication.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'),output=c.connect(viewer,'view'),{entry}=c.subscribe(viewer,source);
    const publication=entry.publication;
    input.emit('message',frame(publication,1,1000000),true);
    c.relay.handleAck(viewer,output,encodeAudioAck(entry.streamId,1));
    c.tick(10000);
    for(let sequence=2;sequence<=102;++sequence)
        input.emit('message',frame(publication,sequence,1000000+(sequence-1)*20000),true);
    assert.equal(input.readyState,1); assert.equal(entry.publication,publication);
    assert.equal(frames(output).length,1);
    input.emit('message',frame(publication,103,11000000),true);
    assert.equal(frames(output).length,2);
}
// Admission itself also retires an expired pipe even before the next sweep.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'),output=c.connect(viewer,'view'),{entry}=c.subscribe(viewer,source);
    const publication=entry.publication;
    input.emit('message',frame(publication,1,1000000),true);
    c.tick(2001); input.emit('message',frame(publication,2,3001000),true);
    assert.equal(output.readyState,3); assert.equal(frames(output).length,1);
}

// Propagation RTT is not a media queue: a stable 600 ms acknowledgement
// round-trip must not force packet loss through a fixed 250 ms credit window.
// The application socket backlog remains bounded independently.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'),output=c.connect(viewer,'view'),{entry}=c.subscribe(viewer,source);
    const publication=entry.publication;
    for(let sequence=1;sequence<=100;++sequence) {
        c.tick(20);
        if(sequence>30) c.relay.handleAck(viewer,output,encodeAudioAck(entry.streamId,sequence-30));
        if(sequence===51) publication.bitrate=32000;
        input.emit('message',encodeAudioPacket(publication.id,sequence,sequence*20000,
            Buffer.alloc(publication.bitrate===32000 ? 80 : 240)),true);
    }
    assert.equal(frames(output).length,100,'healthy long RTT and profile changes must not create artificial loss');
    assert.equal(c.relay.window(output).baseline,600);
    assert.ok(c.relay.window(output).bytes<=31*276);
    output.bufferedAmount=4096; c.tick(20); input.emit('message',frame(publication,101,2020000),true);
    assert.equal(frames(output).length,100,'RTT allowance must not permit a growing socket queue');
    assert.equal(output.readyState,1);
}

// Once RTT is known, an actual stall is still retired promptly. The initial
// discovery allowance must not become a two-second steady-state audio queue.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'),output=c.connect(viewer,'view'),{entry}=c.subscribe(viewer,source);
    const publication=entry.publication;
    input.emit('message',frame(publication,1,1000000),true);
    c.relay.handleAck(viewer,output,encodeAudioAck(entry.streamId,1));
    c.tick(20); input.emit('message',frame(publication,2,1020000),true);
    c.tick(501); c.relay.sweep();
    assert.equal(output.readyState,3); assert.equal(source.ws.readyState,1);
}

// Sender/IPC loss and per-viewer admission loss remain visible as sequence
// gaps. Rewriting only the epoch keeps receipt credit isolated per viewer.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const fast=c.client('fast'),fastWs=c.connect(fast,'view'),fastView=c.subscribe(fast,source);
    const slow=c.client('slow'),slowWs=c.connect(slow,'view'),slowView=c.subscribe(slow,source);
    const publication=fastView.entry.publication;
    input.emit('message',frame(publication,5,1000000),true);
    slowWs.bufferedAmount=4096; c.tick(20); input.emit('message',frame(publication,6,1020000),true);
    slowWs.bufferedAmount=0; c.tick(40); input.emit('message',frame(publication,8,1060000),true);
    assert.deepEqual(frames(fastWs).map(p=>p.sequence),[5,6,8]);
    assert.deepEqual(frames(slowWs).map(p=>p.sequence),[5,8]);
    c.relay.handleAck(slow,slowWs,encodeAudioAck(fastView.entry.streamId,5));
    assert.equal(c.relay.window(slowWs).frames.size,2,'another viewer epoch cannot release receipt credit');
    c.relay.handleAck(slow,slowWs,encodeAudioAck(slowView.entry.streamId,5));
    assert.equal(c.relay.window(slowWs).frames.size,1);
    // Discarding a current-epoch duplicate timestamp still releases its ACK.
    input.emit('message',frame(publication,9,1060000),true);
    assert.equal(receipts(input).at(-1).sequence,9); assert.equal(frames(fastWs).length,3);
}

// A throwing websocket write cannot escape the relay or leave stranded
// receipt state on a broken channel.
{
    const c=context(),source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'),output=c.connect(viewer,'view'),{entry}=c.subscribe(viewer,source);
    output.send=()=>{throw new Error('simulated closed socket');};
    assert.doesNotThrow(()=>input.emit('message',frame(entry.publication),true));
    assert.equal(output.readyState,3); assert.equal(c.relay.windows.has(output),false);
    assert.equal(source.ws.readyState,1);
}

// The aggregate audio pacer accounts for every recipient, even when their
// requested reservations exceed the relay's complete media egress ceiling.
{
    const c=context({screenServerEgressBps:128000}),source=c.client('source');
    const input=c.connect(source,'publish'); c.consent(source); const viewers=[];
    for(let i=0;i<3;++i) { const owner=c.client(`viewer${i}`),output=c.connect(owner,'view'); viewers.push({owner,output,...c.subscribe(owner,source)}); }
    const publication=c.relay.publications.get(source);
    assert.equal(c.relay.totalReservation(),112000);
    for(let sequence=1;sequence<=100;++sequence) {
        c.tick(20); input.emit('message',encodeAudioPacket(publication.id,sequence,sequence*20000,Buffer.alloc(240)),true);
        for(const viewer of viewers) for(const packet of frames(viewer.output))
            c.relay.handleAck(viewer.owner,viewer.output,encodeAudioAck(packet.epoch,packet.sequence));
    }
    const bytes=viewers.reduce((sum,v)=>sum+v.output.messages.filter(Buffer.isBuffer).reduce((n,p)=>n+p.length,0),0);
    assert.ok(bytes<=112000*2.1/8+276,`bounded aggregate output: ${bytes}`);
    for(const viewer of viewers) assert.ok(frames(viewer.output).length>10,'rotation prevents a stable first-viewer monopoly');
}

// Diagnostics aggregate counts and buffer/RTT maxima, with no packet content
// and at most one transport summary every five seconds.
{
    const c=context(),events=[]; c.server.protocolLogger=record=>events.push(JSON.parse(record));
    const source=c.client('source'),input=c.connect(source,'publish'); c.consent(source);
    const viewer=c.client('viewer'),output=c.connect(viewer,'view'),{entry}=c.subscribe(viewer,source);
    input.emit('message',frame(entry.publication),true);
    c.relay.handleAck(viewer,output,encodeAudioAck(entry.streamId,1));
    c.tick(4999); c.relay.sweep(); assert.equal(events.length,0);
    c.tick(1); c.relay.sweep(); assert.equal(events.length,1);
    assert.equal(events[0].event,'audio_transport_summary');
    assert.equal(events[0].receivedPackets,1); assert.equal(events[0].forwardedPackets,1);
    assert.equal(events[0].intervalMs,5000); assert.equal(events[0].payload,undefined);
    c.tick(5000); c.relay.sweep(); assert.equal(events.length,1,'idle intervals do not spam logs');
}

async function integration() {
    const c=context(); c.server.start(); await once(c.server.wss,'listening');
    const port=c.server.wss.address().port,source=c.client('source'),viewer=c.client('viewer');
    const open=async(peer,role)=>{
        const {token}=c.token(peer,role),ws=new WebSocket(`ws://127.0.0.1:${port}/?channel=audio&token=${token}`);
        const [raw,binary]=await once(ws,'message'); assert.equal(binary,false);
        assert.equal(JSON.parse(raw).type,'audio_channel_ready'); return ws;
    };
    const input=await open(source,'publish'),output=await open(viewer,'view'); c.consent(source);
    const {entry}=c.subscribe(viewer,source),publication=entry.publication;
    const incoming=once(input,'message'),outgoing=once(output,'message'); input.send(frame(publication));
    const [ack,isAckBinary]=await incoming,[encoded,isFrameBinary]=await outgoing;
    assert.equal(isAckBinary,true); assert.equal(isFrameBinary,true);
    assert.equal(parseAudioAck(ack).epoch,publication.id); assert.equal(parseAudioPacket(encoded).epoch,entry.streamId);
    output.send(encodeAudioAck(entry.streamId,1));
    const closed1=once(input,'close'),closed2=once(output,'close'); input.close(); output.close(); await Promise.all([closed1,closed2]);
    clearInterval(c.server.uploadCleanupInterval); clearInterval(c.server.leaseSweepInterval);
    await new Promise(resolve=>c.server.wss.close(resolve));
}
integration().then(()=>console.log('audio_share_protocol: all tests passed')).catch(error=>{console.error(error);process.exitCode=1;});
