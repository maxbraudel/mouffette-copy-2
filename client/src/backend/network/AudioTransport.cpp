#include "backend/network/AudioTransport.h"
#include "backend/network/AudioWire.h"
#include "backend/network/AudioPacketFreshness.h"
#include "backend/network/AudioPlayoutPolicy.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/NetworkProxyPolicy.h"
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QUrlQuery>
#include <QWebSocket>
#include <algorithm>

Q_LOGGING_CATEGORY(audioTransportLog, "mouffette.audio.transport")

namespace {
bool uuid(const QString& value) { return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces)==value; }
int reservation(int bitrate) { return bitrate==32000 ? 48000 : 112000; }
qint64 receiptGraceMs(qint64 baseline) { return baseline < 0 ? 1000 : baseline + 150; }
qint64 receiptTimeoutMs(qint64 baseline) { return baseline < 0 ? 2000 : std::clamp<qint64>(baseline + 500, 500, 2000); }
}

struct AudioTransport::Private {
    AudioTransport* q;
    QPointer<WebSocketClient> network;
    QTimer timer;
    QElapsedTimer clock;
    struct Pipe {
        QPointer<QWebSocket> socket;
        QString requestId;
        bool authenticated=false;
        qint64 deadline=0,retryAt=0;
        int attempts=0;
    } publish,view;
    struct Receipt { qint64 bytes=0,sentAt=0; };
    QHash<quint64,Receipt> receipts;
    qint64 pendingBytes=0,baseline=-1,baselineAt=0,nextSendAt=0,lastBudgetAt=-1000,lastFeedbackAt=-1000;
    quint64 sequence=0,receivedSequence=0;
    qint64 lastTimestamp=-1;
    bool sharing=false,suspended=false,stopped=false,sourceCongested=false;
    quint64 consentGeneration=0;
    bool consentValue=false;
    QString session,stream,publication,grantPublication;
    quint64 generation=0;
    int bitrate=96000,grantBitrate=96000,totalBudget=1200000;
    QString lastState;
    AudioPacketFreshness viewerFreshness;
    AudioPlayoutPolicy playoutPolicy;
    qint64 lastVideoArrivalUs=-1,lastVideoSourceUs=-1;
    struct Diagnostics {
        quint64 sent=0,received=0,stale=0,admissionDrops=0,playbackDrops=0;
        qint64 maximumCaptureAgeUs=0,maximumQueueBytes=0;
        int maximumPlaybackBufferedMs=0;
    } diagnostics;
    qint64 lastDiagnosticsAt=0;

    void reportDiagnostics() {
        const qint64 now=clock.elapsed();
        if(now-lastDiagnosticsAt<5000) return;
        const qint64 interval=now-lastDiagnosticsAt;
        lastDiagnosticsAt=now;
        if(diagnostics.sent || diagnostics.received || diagnostics.stale || diagnostics.admissionDrops || diagnostics.playbackDrops)
            qCDebug(audioTransportLog).nospace() << "audio_transport interval_ms=" << interval << " sent=" << diagnostics.sent
                << " received=" << diagnostics.received << " stale=" << diagnostics.stale
                << " admission_drops=" << diagnostics.admissionDrops << " playback_drops=" << diagnostics.playbackDrops
                << " max_capture_age_us=" << diagnostics.maximumCaptureAgeUs << " max_socket_queue_bytes=" << diagnostics.maximumQueueBytes
                << " max_playback_buffered_ms=" << diagnostics.maximumPlaybackBufferedMs << " receipt_rtt_ms=" << baseline
                << " pending_receipts=" << receipts.size() << " pending_bytes=" << pendingBytes << " bitrate_bps=" << bitrate
                << " target_delay_us=" << playoutPolicy.targetDelayUs();
        diagnostics={};
    }

    bool connected() const { return network && network->isConnected() && network->hasUnexpiredLease(); }
    bool supported() const { return connected() && network->audioSharingSupported(); }
    bool wantsPublish() const { return !stopped && !suspended && sharing && supported(); }
    bool wantsView() const {
        if(stopped || suspended || session.isEmpty() || !supported() || !network->canIssueSessionCommands(session)) return false;
        const auto binding=network->remoteSessionCoordinator()->byId(session);
        return binding.generation==generation && binding.ownerEndpointId==network->endpointId();
    }
    bool send(QJsonObject message) { return connected() && network->sendControlMessage(message); }
    void state(const QString& reason) { if(lastState!=reason) { lastState=reason; emit q->remoteStateChanged(reason); } }
    void clearView() {
        lastState.clear();
        viewerFreshness.reset();
        playoutPolicy.reset(); lastVideoArrivalUs=-1; lastVideoSourceUs=-1;
        if(stream.isEmpty()) return;
        stream.clear(); receivedSequence=0;
        emit q->playbackStreamChanged({});
    }
    void applyPublication() {
        const QString next=wantsPublish() && publish.authenticated ? grantPublication : QString();
        if(publication==next && (next.isEmpty() || bitrate==grantBitrate)) return;
        const bool changed=publication!=next;
        if(changed) { receipts.clear(); pendingBytes=0; sequence=0; lastTimestamp=-1; nextSendAt=0; sourceCongested=false; }
        publication=next; bitrate=grantBitrate;
        emit q->sourceReservationChanged(publication.isEmpty() ? 0 : reservation(bitrate));
        emit q->publicationChanged(!publication.isEmpty(),bitrate);
    }
    void close(Pipe& pipe,bool retry=false) {
        auto* socket=pipe.socket.data();
        pipe.socket=nullptr; pipe.authenticated=false; pipe.requestId.clear(); pipe.deadline=0;
        if(socket) { QObject::disconnect(socket,nullptr,q,nullptr); socket->abort(); socket->deleteLater(); }
        if(&pipe==&publish) { grantPublication.clear(); applyPublication(); receipts.clear(); pendingBytes=0; baseline=-1; }
        else clearView();
        if(retry) { pipe.retryAt=clock.elapsed()+std::min(10000,500*(1<<std::min(pipe.attempts++,5))); }
        else { pipe.retryAt=0; pipe.attempts=0; }
    }
    void fail(Pipe& pipe,bool preserveVideoClock=false,bool timingFailure=false) {
        const bool wasReady=pipe.authenticated;
        const auto freshness=viewerFreshness;
        const auto videoArrival=lastVideoArrivalUs,videoSource=lastVideoSourceUs;
        close(pipe,true);
        if(&pipe==&view) {
            // Retiring stale audio must not erase a recent faster video's
            // clock evidence and make the next late packet look current.
            if(preserveVideoClock) {
                viewerFreshness=freshness; lastVideoArrivalUs=videoArrival; lastVideoSourceUs=videoSource;
            }
            state(timingFailure ? QStringLiteral("timing_unavailable") : QStringLiteral("channel_unavailable"));
        }
        if(&pipe==&view) emit q->issue(timingFailure
            ? AudioTransport::tr("Remote audio exceeded the synchronization delay limit. Reconnecting…")
            : AudioTransport::tr("The remote system audio connection is unavailable. Reconnecting…"));
        else if(wasReady) emit q->publicationIssue(
            AudioTransport::tr("The system audio sharing connection was interrupted. Reconnecting…"));
    }
    bool authenticatedMessage(const QJsonObject& message) const {
        return connected() && message.value("protocolVersion").toInt(-1)==WebSocketClient::ProtocolVersion
            && message.value("serverBootId").toString()==network->serverBootId()
            && message.value("connectionGeneration").toDouble()==double(network->connectionGeneration())
            && uuid(message.value("messageId").toString());
    }
    void request(Pipe& pipe,const QString& role) {
        if(pipe.socket || !pipe.requestId.isEmpty() || clock.elapsed()<pipe.retryAt) return;
        pipe.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces); pipe.deadline=clock.elapsed()+15000;
        if(!send({{"type","request_audio_channel"},{"requestId",pipe.requestId},{"role",role},{"audioVersion",1}})) fail(pipe);
    }
    void open(Pipe& pipe,const QString& role,const QString& token) {
        pipe.requestId.clear();
        auto* socket=new QWebSocket(QString(),QWebSocketProtocol::VersionLatest,q);
        pipe.socket=socket; pipe.deadline=clock.elapsed()+15000;
        socket->setMaxAllowedIncomingFrameSize(AudioWire::HeaderBytes+AudioWire::MaximumPayloadBytes);
        socket->setMaxAllowedIncomingMessageSize(AudioWire::HeaderBytes+AudioWire::MaximumPayloadBytes);
        QObject::connect(socket,&QWebSocket::disconnected,q,[this,&pipe,socket] { if(pipe.socket==socket) fail(pipe); });
        QObject::connect(socket,&QWebSocket::errorOccurred,q,[this,&pipe,socket](QAbstractSocket::SocketError) { if(pipe.socket==socket) fail(pipe); });
        QObject::connect(socket,&QWebSocket::textMessageReceived,q,[this,&pipe,socket,role](const QString& text) {
            if(pipe.socket!=socket) return;
            const auto message=QJsonDocument::fromJson(text.toUtf8()).object();
            if(!authenticatedMessage(message) || pipe.authenticated || message.value("type")!="audio_channel_ready"
                || message.value("audioVersion").toInt()!=1 || message.value("role")!=role) { fail(pipe); return; }
            pipe.authenticated=true; pipe.deadline=0;
            if(&pipe==&publish) { pipe.attempts=0; applyPublication(); }
            else subscribe(true);
        });
        QObject::connect(socket,&QWebSocket::binaryMessageReceived,q,[this,&pipe,socket](const QByteArray& data) {
            if(pipe.socket!=socket || !pipe.authenticated || !connected()) return;
            AudioWire::Frame frame;
            if(!AudioWire::parse(data,frame,&pipe==&publish)) { fail(pipe); return; }
            if(&pipe==&publish) {
                if(frame.epoch!=publication) return;
                auto pending=receipts.find(frame.sequence); if(pending==receipts.end()) return;
                const auto now=clock.elapsed(),rtt=now-pending->sentAt;
                if(rtt>=receiptTimeoutMs(baseline)) { fail(pipe); return; }
                pendingBytes-=pending->bytes; receipts.erase(pending);
                if(baseline<0 || rtt<=baseline || now-baselineAt>=30000) { baseline=rtt; baselineAt=now; }
            } else {
                // Even an obsolete epoch consumes only its own relay credit.
                if(socket->bytesToWrite()+28>4096) { fail(pipe); return; }
                if(socket->sendBinaryMessage(AudioWire::ack(frame.epoch,frame.sequence))<0) { fail(pipe); return; }
                if(!wantsView() || frame.epoch!=stream || frame.sequence<=receivedSequence) return;
                receivedSequence=frame.sequence;
                const qint64 arrivalUs=MediaCaptureClock::nowUs();
                const bool fresh=viewerFreshness.observe(frame.timestampUs,arrivalUs);
                const qint64 mappedUs=viewerFreshness.localTimeUs(frame.timestampUs);
                const qint64 videoMappedUs=viewerFreshness.localTimeUs(lastVideoSourceUs);
                const bool fasterVideo=lastVideoArrivalUs>=0 && arrivalUs-lastVideoArrivalUs<=1000000
                    && videoMappedUs>=0 && lastVideoArrivalUs-videoMappedUs+20000<arrivalUs-mappedUs;
                if(playoutPolicy.observe(frame.timestampUs,arrivalUs,mappedUs)) { fail(pipe,fasterVideo,true); return; }
                if(!fresh) { ++diagnostics.stale; return; }
                // Authentication alone does not prove media has recovered;
                // retain exponential backoff across repeatedly stale pipes.
                if(arrivalUs-mappedUs<playoutPolicy.targetDelayUs()) pipe.attempts=0;
                ++diagnostics.received;
                emit q->packetReceived(frame.payload,frame.timestampUs,frame.sequence);
            }
        });
        QUrl url(network->m_serverUrl); QUrlQuery query(url);
        query.removeAllQueryItems("channel"); query.removeAllQueryItems("token");
        query.addQueryItem("channel","audio"); query.addQueryItem("token",token); url.setQuery(query);
        configureNetworkProxy(socket,url); socket->open(url);
    }
    bool subscribe(bool enabled) {
        if(session.isEmpty()) return true;
        return send({{"type","audio_share_subscribe"},{"remoteSessionId",session},
            {"generation",double(generation)},{"enabled",enabled}});
    }
    bool congested() const {
        const qint64 now=clock.elapsed(),grace=receiptGraceMs(baseline);
        for(const auto& receipt:receipts) if(now-receipt.sentAt>grace) return true;
        return publish.socket && publish.socket->bytesToWrite()>8192;
    }
    void reconcile() {
        reportDiagnostics();
        if(!supported()) {
            if(publish.socket || !publish.requestId.isEmpty()) close(publish);
            if(view.socket || !view.requestId.isEmpty()) close(view);
            consentGeneration=0;
            if(!session.isEmpty()) state(connected() ? QStringLiteral("unsupported") : QStringLiteral("session_unavailable"));
            return;
        }
        const bool consent=sharing && !suspended && !stopped;
        if(consentGeneration!=network->connectionGeneration() || consentValue!=consent) {
            if(!send({{"type","audio_share_consent"},{"enabled",consent},{"audioVersion",1}})) {
                close(publish); return;
            }
            consentValue=consent; consentGeneration=network->connectionGeneration();
        }
        if(wantsPublish()) request(publish,QStringLiteral("publish"));
        else if(publish.socket || !publish.requestId.isEmpty() || !publication.isEmpty()) close(publish);
        if(wantsView()) request(view,QStringLiteral("view"));
        else if(view.socket || !view.requestId.isEmpty() || !stream.isEmpty()) close(view);
        const qint64 now=clock.elapsed();
        for(auto* pipe:{&publish,&view}) if(pipe->deadline && now>=pipe->deadline) fail(*pipe);
        for(const auto& receipt:receipts) if(now-receipt.sentAt>=receiptTimeoutMs(baseline)) { fail(publish); break; }
        if(wantsPublish() && now-lastBudgetAt>=500) {
            lastBudgetAt=now;
            if(send({{"type","audio_source_budget"},{"totalBps",totalBudget},{"congested",congested() || sourceCongested}}))
                sourceCongested=false;
        }
    }
    void control(const QJsonObject& message) {
        if(!supported()) return;
        const auto type=message.value("type").toString();
        if(type==QLatin1String("audio_channel_token")) {
            Pipe* pipe=message.value("requestId")==publish.requestId ? &publish
                : message.value("requestId")==view.requestId ? &view : nullptr;
            if(!pipe || pipe->requestId.isEmpty()) return;
            const QString role=pipe==&publish ? QStringLiteral("publish") : QStringLiteral("view");
            const QString token=message.value("token").toString();
            if(message.value("role")!=role || message.value("audioVersion").toInt()!=1 || token.size()!=43
                || !(pipe==&publish ? wantsPublish() : wantsView())) { close(*pipe); return; }
            open(*pipe,role,token);
        } else if(type==QLatin1String("audio_publication_request")) {
            const bool enabled=message.value("enabled").toBool();
            const QString id=message.value("publicationId").toString();
            const int rate=message.value("bitrateBps").toInt();
            if(!uuid(id) || (rate!=32000 && rate!=96000)) return;
            // A delayed disable cannot retire a newer publication.
            if(!enabled && id!=grantPublication) return;
            grantPublication=enabled && wantsPublish() ? id : QString(); grantBitrate=rate; applyPublication();
        } else if(type==QLatin1String("audio_share_state")) {
            if(message.value("remoteSessionId")!=session || message.value("generation").toDouble()!=double(generation)) return;
            const bool enabled=message.value("enabled").toBool() && wantsView();
            const QString id=enabled ? message.value("streamId").toString() : QString();
            if(enabled && !uuid(id)) return;
            if(stream!=id) {
                // Keep a video observation that arrived before the first audio
                // grant, but do not carry an old audio epoch's baseline forward.
                if(!stream.isEmpty()) { viewerFreshness.reset(); playoutPolicy.reset(); lastVideoArrivalUs=-1; lastVideoSourceUs=-1; }
                stream=id; receivedSequence=0; lastState.clear(); emit q->playbackStreamChanged(stream);
            }
            state(message.value("reason").toString());
        }
    }
};

AudioTransport::AudioTransport(WebSocketClient* network,QObject* parent) : QObject(parent),d(std::make_unique<Private>()) {
    d->q=this; d->network=network; d->clock.start(); d->timer.setInterval(100);
    connect(&d->timer,&QTimer::timeout,this,[this] { d->reconcile(); });
    connect(network,&WebSocketClient::audioControlReceived,this,[this](const QJsonObject& message) { d->control(message); });
    connect(network,&WebSocketClient::connected,this,[this] { d->consentGeneration=0; d->reconcile(); });
    connect(network,&WebSocketClient::disconnected,this,[this] { d->close(d->publish); d->close(d->view); d->consentGeneration=0; });
    connect(network,&WebSocketClient::leaseExpired,this,[this] { d->close(d->publish); d->close(d->view); });
    connect(network->remoteSessionCoordinator(),&RemoteSessionCoordinator::sessionChanged,this,[this] { d->reconcile(); });
    connect(network->remoteSessionCoordinator(),&RemoteSessionCoordinator::sessionRemoved,this,[this] { d->reconcile(); });
    d->timer.start();
}
AudioTransport::~AudioTransport() { blockSignals(true); d->timer.stop(); d->close(d->publish); d->close(d->view); }
void AudioTransport::setSharingEnabled(bool enabled) { d->stopped=false; d->sharing=enabled; d->reconcile(); }
void AudioTransport::setSuspended(bool suspended) { if(d->suspended==suspended) return; d->suspended=suspended; d->reconcile(); }
void AudioTransport::setSubscription(const QString& session,quint64 generation,bool enabled) {
    d->stopped=false;
    if(d->session==session && d->generation==generation && enabled) { d->reconcile(); return; }
    if(!d->session.isEmpty() && !d->subscribe(false)) d->close(d->view);
    d->clearView();
    d->session=enabled ? session : QString(); d->generation=enabled ? generation : 0;
    if(enabled && d->view.authenticated && d->wantsView() && !d->subscribe(true)) d->fail(d->view);
    d->reconcile();
}
void AudioTransport::setSourceBudget(int totalBps) { d->totalBudget=std::clamp(totalBps,128000,100000000); }
void AudioTransport::observeVideoTimestamp(qint64 sourceUs,qint64 receivedAtUs) {
    if(!d->wantsView() || sourceUs<0 || quint64(sourceUs)>AudioWire::MaximumInteger) return;
    d->viewerFreshness.observe(sourceUs,receivedAtUs);
    d->lastVideoSourceUs=sourceUs; d->lastVideoArrivalUs=receivedAtUs;
}
qint64 AudioTransport::playbackTimeUs(qint64 sourceUs) const {
    const qint64 arrival=d->viewerFreshness.localTimeUs(sourceUs);
    return arrival<0 ? -1 : arrival+d->playoutPolicy.targetDelayUs();
}
void AudioTransport::setOutputQuantumUs(qint64 quantumUs) { d->playoutPolicy.setOutputQuantumUs(quantumUs); }
bool AudioTransport::sendPacket(const QByteArray& opus,qint64 timestampUs,quint64 captureSequence) {
    if(!isPublishing() || opus.isEmpty() || opus.size()>AudioWire::MaximumPayloadBytes || timestampUs<0
        || quint64(timestampUs)>AudioWire::MaximumInteger || timestampUs<=d->lastTimestamp
        || d->sequence>=AudioWire::MaximumInteger || captureSequence>AudioWire::MaximumInteger
        || (captureSequence && captureSequence<=d->sequence)) return false;
    const auto captureAge=MediaCaptureClock::nowUs()-timestampUs;
    d->diagnostics.maximumCaptureAgeUs=std::max(d->diagnostics.maximumCaptureAgeUs,captureAge);
    if(captureAge>250000 || captureAge < -250000) { ++d->diagnostics.stale; return false; }
    // Sequence counts captured packets, including locally discarded ones.
    const quint64 sequence=captureSequence ? captureSequence : d->sequence+1;
    d->sequence=sequence; d->lastTimestamp=timestampUs;
    const auto now=d->clock.elapsed();
    const qint64 size=AudioWire::HeaderBytes+opus.size();
    d->diagnostics.maximumQueueBytes=std::max(d->diagnostics.maximumQueueBytes,d->publish.socket->bytesToWrite());
    // Receipt credit includes propagation and reverse-path RTT; it is not
    // queued audio. Bound socket backlog separately instead of dropping
    // healthy audio whenever RTT exceeds a fixed 250 ms byte window.
    const qint64 creditMs=std::clamp<qint64>(receiptGraceMs(d->baseline),250,2000);
    // Existing normal-profile packets remain in flight after a bitrate
    // downgrade. Shrinking their credit creates a fresh artificial dropout.
    const qint64 limit=std::max<qint64>(4096,qint64(reservation(96000))*creditMs/8000);
    if(d->receipts.size()>=std::min<qint64>(128,creditMs/20+2) || d->pendingBytes+size>limit
        || d->publish.socket->bytesToWrite()+size>4096 || d->congested() || d->nextSendAt>now+40) {
        d->sourceCongested=true; ++d->diagnostics.admissionDrops; return false;
    }
    const auto bytes=AudioWire::packet(d->publication,sequence,timestampUs,opus);
    d->receipts.insert(sequence,{bytes.size(),now}); d->pendingBytes+=bytes.size();
    d->nextSendAt=std::max(now,d->nextSendAt)+std::max<qint64>(1,bytes.size()*8000/reservation(d->bitrate));
    if(d->publish.socket->sendBinaryMessage(bytes)<0) { d->fail(d->publish); return false; }
    ++d->diagnostics.sent;
    return true;
}
void AudioTransport::sendStatus(const QString& reason) {
    if(!isPublishing()) return;
    d->send({{"type","audio_publication_status"},{"publicationId",d->publication},{"reason",reason}});
}
void AudioTransport::sendPlaybackFeedback(int droppedPackets,int bufferedMs) {
    d->diagnostics.playbackDrops+=std::clamp(droppedPackets,0,10000);
    d->diagnostics.maximumPlaybackBufferedMs=std::max(d->diagnostics.maximumPlaybackBufferedMs,std::clamp(bufferedMs,0,10000));
    if(d->stream.isEmpty() || !d->wantsView() || d->clock.elapsed()-d->lastFeedbackAt<500) return;
    d->lastFeedbackAt=d->clock.elapsed();
    // The relay adapts network quality from queue pressure. A device's known
    // extra PCM reservoir is not congestion; keep actual depth in diagnostics.
    const auto hardwareMs=AudioOutputTiming::extraPlayoutDelayUs(d->playoutPolicy.outputQuantumUs())/1000;
    const int networkBufferedMs=int(std::clamp<qint64>(qint64(bufferedMs)-hardwareMs,0,10000));
    d->send({{"type","audio_view_feedback"},{"remoteSessionId",d->session},{"generation",double(d->generation)},
        {"streamId",d->stream},{"droppedPackets",std::clamp(droppedPackets,0,10000)},{"bufferedMs",networkBufferedMs}});
}
void AudioTransport::restartPublication() {
    if(!isPublishing()) return;
    d->close(d->publish,true); d->reconcile();
}
void AudioTransport::stop() {
    if(!d->session.isEmpty()) d->subscribe(false);
    d->session.clear(); d->generation=0; d->stopped=true; d->reconcile();
    d->close(d->publish); d->close(d->view);
}
bool AudioTransport::isSupported() const { return d->supported(); }
bool AudioTransport::isPublishing() const { return !d->publication.isEmpty() && d->wantsPublish() && d->publish.authenticated; }
int AudioTransport::audioBitrateBps() const { return d->bitrate; }
int AudioTransport::reservedSourceBps() const { return isPublishing() ? reservation(d->bitrate) : 0; }
QString AudioTransport::publicationId() const { return d->publication; }
QString AudioTransport::viewerStreamId() const { return d->stream; }
