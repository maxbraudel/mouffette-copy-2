#include "backend/network/AudioTransport.h"
#include "backend/network/AudioWire.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/NetworkProxyPolicy.h"
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <algorithm>

namespace {
bool uuid(const QString& value) { return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces)==value; }
int reservation(int bitrate) { return bitrate==32000 ? 48000 : 112000; }
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
    bool sharing=false,suspended=false,stopped=false;
    quint64 consentGeneration=0;
    bool consentValue=false;
    QString session,stream,publication,grantPublication;
    quint64 generation=0;
    int bitrate=96000,grantBitrate=96000,totalBudget=1200000;
    QString lastState;

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
        if(stream.isEmpty()) return;
        stream.clear(); receivedSequence=0;
        emit q->playbackStreamChanged({});
    }
    void applyPublication() {
        const QString next=wantsPublish() && publish.authenticated ? grantPublication : QString();
        if(publication==next && (next.isEmpty() || bitrate==grantBitrate)) return;
        const bool changed=publication!=next;
        if(changed) { receipts.clear(); pendingBytes=0; sequence=0; lastTimestamp=-1; nextSendAt=0; }
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
    void fail(Pipe& pipe) {
        const bool wasReady=pipe.authenticated;
        close(pipe,true);
        if(&pipe==&view) state(QStringLiteral("channel_unavailable"));
        if(wasReady) emit q->issue(AudioTransport::tr("The remote audio connection was interrupted. Reconnecting…"));
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
            pipe.authenticated=true; pipe.attempts=0; pipe.deadline=0;
            if(&pipe==&publish) applyPublication();
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
                pendingBytes-=pending->bytes; receipts.erase(pending);
                if(baseline<0 || rtt<=baseline || now-baselineAt>=30000) { baseline=rtt; baselineAt=now; }
            } else {
                // Even an obsolete epoch consumes only its own relay credit.
                if(socket->bytesToWrite()>32768) { fail(pipe); return; }
                socket->sendBinaryMessage(AudioWire::ack(frame.epoch,frame.sequence));
                if(!wantsView() || frame.epoch!=stream || frame.sequence<=receivedSequence) return;
                receivedSequence=frame.sequence;
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
        const qint64 now=clock.elapsed(),grace=baseline<0 ? 1000 : baseline+150;
        for(const auto& receipt:receipts) if(now-receipt.sentAt>grace) return true;
        return publish.socket && publish.socket->bytesToWrite()>8192;
    }
    void reconcile() {
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
        for(const auto& receipt:receipts) if(now-receipt.sentAt>=3000) { fail(publish); break; }
        if(wantsPublish() && now-lastBudgetAt>=500) {
            lastBudgetAt=now;
            send({{"type","audio_source_budget"},{"totalBps",totalBudget},{"congested",congested()}});
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
            if(stream!=id) { stream=id; receivedSequence=0; lastState.clear(); emit q->playbackStreamChanged(stream); }
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
bool AudioTransport::sendPacket(const QByteArray& opus,qint64 timestampUs) {
    if(!isPublishing() || opus.isEmpty() || opus.size()>AudioWire::MaximumPayloadBytes || timestampUs<0
        || quint64(timestampUs)>AudioWire::MaximumInteger || timestampUs<d->lastTimestamp
        || d->sequence>=AudioWire::MaximumInteger) return false;
    const auto now=d->clock.elapsed();
    const qint64 size=AudioWire::HeaderBytes+opus.size();
    const qint64 limit=std::min<qint64>(32768,std::max<qint64>(4096,qint64(reservation(d->bitrate))*(std::max<qint64>(50,d->baseline)+150)/8000));
    if(d->receipts.size()>=128 || d->pendingBytes+size>limit || d->publish.socket->bytesToWrite()+size>limit
        || d->congested() || d->nextSendAt>now+40) return false;
    const quint64 sequence=++d->sequence;
    d->lastTimestamp=timestampUs;
    const auto bytes=AudioWire::packet(d->publication,sequence,timestampUs,opus);
    d->receipts.insert(sequence,{bytes.size(),now}); d->pendingBytes+=bytes.size();
    d->nextSendAt=std::max(now,d->nextSendAt)+std::max<qint64>(1,bytes.size()*8000/reservation(d->bitrate));
    if(d->publish.socket->sendBinaryMessage(bytes)<0) { d->fail(d->publish); return false; }
    return true;
}
void AudioTransport::sendStatus(const QString& reason) {
    if(!isPublishing()) return;
    d->send({{"type","audio_publication_status"},{"publicationId",d->publication},{"reason",reason}});
}
void AudioTransport::sendPlaybackFeedback(int droppedPackets,int bufferedMs) {
    if(d->stream.isEmpty() || !d->wantsView() || d->clock.elapsed()-d->lastFeedbackAt<500) return;
    d->lastFeedbackAt=d->clock.elapsed();
    d->send({{"type","audio_view_feedback"},{"remoteSessionId",d->session},{"generation",double(d->generation)},
        {"streamId",d->stream},{"droppedPackets",std::clamp(droppedPackets,0,10000)},{"bufferedMs",std::clamp(bufferedMs,0,10000)}});
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
