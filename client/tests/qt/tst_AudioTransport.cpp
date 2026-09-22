#include "backend/network/AudioTransport.h"
#include "backend/network/AudioWire.h"
#include "backend/network/AudioPacketFreshness.h"
#include "backend/network/AudioPlayoutPolicy.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include "backend/audiosharing/AudioSharingService.h"
#include "backend/audiosharing/AudioEngine.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

// Exercise the production authentication/session coordinator and both audio
// sockets. Synthetic compressed bytes never touch a capture or output device.
class AudioTransportTest final : public QObject {
    Q_OBJECT
    QProcess relay;
    QByteArray log;
    QString url;
    void configure(WebSocketClient& peer,const QString& name) {
        const auto advertise=[&peer,name] { peer.registerClient(name,QStringLiteral("audio-test"),
            {ScreenInfo(0,640,360,0,0,true),ScreenInfo(1,640,360,640,0,false),ScreenInfo(2,640,360,1280,0,false)},50); };
        connect(&peer,&WebSocketClient::connected,&peer,advertise);
        connect(&peer,&WebSocketClient::localDeviceSnapshotRequested,&peer,advertise);
        connect(&peer,&WebSocketClient::remoteSessionTerminating,&peer,[&peer](const QJsonObject& event) {
            if(event.value("targetEndpointId")==peer.endpointId()) peer.acknowledgeRemoteSessionTeardown(
                event.value("remoteSessionId").toString(),event.value("teardownId").toString(),true,true,true,0,{},0);
        });
    }
    void connectPeers(WebSocketClient& owner,WebSocketClient& target,QString& session) {
        configure(owner,QStringLiteral("viewer")); configure(target,QStringLiteral("publisher"));
        QSignalSpy registered(&target,&WebSocketClient::registrationConfirmed),opened(&owner,&WebSocketClient::remoteSessionOpened);
        owner.connectToServer(url); target.connectToServer(url);
        QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && !registered.isEmpty(),5000);
        QVERIFY(owner.openRemoteSession(target.endpointId()));
        QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(),5000);
        session=opened.first().first().toJsonObject().value("remoteSessionId").toString();
        QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(session) && target.canIssueSessionCommands(session),5000);
    }
    void activate(WebSocketClient& owner,WebSocketClient& source,AudioTransport& receiver,AudioTransport& publisher,QString& session) {
        connectPeers(owner,source,session);
        source.setScreenSharingEnabled(true); publisher.setSharingEnabled(true);
        receiver.setSubscription(session,owner.remoteSessionCoordinator()->byId(session).generation,true);
        QTRY_VERIFY_WITH_TIMEOUT(publisher.isPublishing(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(!receiver.viewerStreamId().isEmpty(),5000);
    }
private slots:
    void init() {
        const auto node=QStandardPaths::findExecutable(QStringLiteral("node")); QVERIFY(!node.isEmpty()); log.clear();
        auto environment=QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("MOUFFETTE_TEST_AUDIO_LEGACY"),
            QByteArray(QTest::currentTestFunction()).startsWith("legacy") ? QStringLiteral("1") : QStringLiteral("0"));
        relay.setProcessEnvironment(environment);
        relay.setProcessChannelMode(QProcess::MergedChannels);
        connect(&relay,&QProcess::readyReadStandardOutput,this,[this] { log+=relay.readAllStandardOutput(); });
        relay.start(node,{QStringLiteral(MOUFFETTE_RELAY_FIXTURE),QStringLiteral("0")}); QVERIFY(relay.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(log.contains("TEST_READY "),5000);
        const auto match=QRegularExpression(QStringLiteral("TEST_READY (\\d+)")).match(QString::fromUtf8(log));
        QVERIFY2(match.hasMatch(),log.constData()); url=QStringLiteral("ws://127.0.0.1:%1").arg(match.captured(1));
    }
    void cleanup() {
        if(relay.state()!=QProcess::NotRunning) { relay.write("{\"action\":\"shutdown\"}\n");
            if(!relay.waitForFinished(2000)) { relay.kill(); relay.waitForFinished(2000); } }
        disconnect(&relay,nullptr,this,nullptr);
    }
    void listeningStatesAndFailuresAreIndependentOfScreenSharing() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioSharingService listener(&owner);
        AudioTransport publisher(&source);
        auto* receiver=listener.findChild<AudioTransport*>(); QVERIFY(receiver);
        auto* worker=AudioEngine::instance();
        QSignalSpy issues(&listener,&AudioSharingService::remoteIssue);
        QString session; connectPeers(owner,source,session);
        source.setScreenSharingEnabled(false);
        listener.setViewedEndpoint(source.endpointId());
        QCOMPARE(listener.state(),QStringLiteral("disabled"));
        QVERIFY(receiver->viewerStreamId().isEmpty());
        listener.setListeningEnabled(true);
        QTRY_COMPARE_WITH_TIMEOUT(listener.state(),QStringLiteral("sharing_disabled"),5000);
        QVERIFY(listener.remoteStatus().contains(QStringLiteral("disabled system audio sharing")));
        publisher.setSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(publisher.isPublishing() && !receiver->viewerStreamId().isEmpty(),5000);
        QTRY_COMPARE_WITH_TIMEOUT(listener.state(),QStringLiteral("loading"),3000);
        const auto stream=receiver->viewerStreamId();
        // A successfully started but silent system has no packets or output
        // clock yet. Its capture readiness must not remain loading forever.
        publisher.sendStatus(QStringLiteral("streaming"));
        QTRY_COMPARE_WITH_TIMEOUT(listener.state(),QStringLiteral("available"),3000);
        QVERIFY(listener.remoteStatus().contains(QStringLiteral("sharing is active")));
        worker->playbackClock(source.endpointId(),stream,100,200);
        QCOMPARE(listener.state(),QStringLiteral("available"));

        // Source capture/codec failure is visible even with screen consent off.
        issues.clear(); publisher.sendStatus(QStringLiteral("capture_error"));
        QTRY_COMPARE_WITH_TIMEOUT(listener.state(),QStringLiteral("error"),3000);
        QVERIFY(listener.remoteStatus().contains(QStringLiteral("captured or encoded")));
        QCOMPARE(issues.size(),1);
        publisher.sendStatus(QStringLiteral("capture_error"));
        QTest::qWait(150);
        QCOMPARE(issues.size(),1);
        QCOMPARE(receiver->viewerStreamId(),stream);
        publisher.sendStatus(QStringLiteral("starting"));
        QTRY_COMPARE_WITH_TIMEOUT(listener.state(),QStringLiteral("loading"),3000);
        publisher.sendStatus(QStringLiteral("streaming"));
        QTRY_COMPARE_WITH_TIMEOUT(listener.state(),QStringLiteral("available"),3000);
        worker->playbackClock(source.endpointId(),stream,300,400);
        QCOMPARE(listener.state(),QStringLiteral("available"));

        // Late decoder/device errors cannot belong to a replacement stream.
        worker->playbackFailed(source.endpointId(),QStringLiteral("old-epoch"),QStringLiteral("Old codec error"));
        QCOMPARE(listener.state(),QStringLiteral("available"));
        worker->playbackFailed(QStringLiteral("other-client"),stream,QStringLiteral("Other device error"));
        QCOMPARE(listener.state(),QStringLiteral("available"));
        issues.clear();
        worker->playbackFailed(source.endpointId(),stream,QStringLiteral("Audio decoder failed"));
        QCOMPARE(listener.state(),QStringLiteral("error"));
        QCOMPARE(listener.remoteStatus(),QStringLiteral("Audio decoder failed"));
        QCOMPARE(issues.size(),1);
        worker->playbackFailed(source.endpointId(),stream,QStringLiteral("Audio decoder failed"));
        QCOMPARE(issues.size(),1);
        publisher.sendStatus(QStringLiteral("streaming"));
        QTest::qWait(150);
        QCOMPARE(listener.state(),QStringLiteral("error"));
        worker->playbackClock(source.endpointId(),stream,500,600);
        QCOMPARE(listener.state(),QStringLiteral("available"));

        source.setScreenSharingEnabled(true);
        source.setScreenSharingEnabled(false);
        QTest::qWait(250);
        QCOMPARE(receiver->viewerStreamId(),stream);
        QCOMPARE(listener.state(),QStringLiteral("available"));
        listener.setListeningEnabled(false);
        QCOMPARE(listener.state(),QStringLiteral("disabled"));
        QVERIFY(receiver->viewerStreamId().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!publisher.isPublishing(),3000);
        worker->playbackFailed(source.endpointId(),stream,QStringLiteral("Late failure"));
        QCOMPARE(listener.state(),QStringLiteral("disabled"));
        listener.setListeningEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(publisher.isPublishing(),3000);
        publisher.setSharingEnabled(false);
        QTRY_COMPARE_WITH_TIMEOUT(listener.state(),QStringLiteral("sharing_disabled"),3000);
    }
    void captureFailureRemainsVisibleDuringRetryBackoff() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner);
        AudioEngine audio(nullptr, [] { return std::unique_ptr<SystemAudioCapture>{}; });
        AudioSharingService publisher(&source, nullptr, &audio);
        auto* transport=publisher.findChild<AudioTransport*>(); QVERIFY(transport);
        QSignalSpy remoteStates(&receiver,&AudioTransport::remoteStateChanged);
        QString session; connectPeers(owner,source,session);
        source.setScreenSharingEnabled(false);
        // Native capture is deliberately unavailable; do not request OS
        // permissions in this relay test. Its error must remain independent of video.
        publisher.setSharingEnabled(true);
        receiver.setSubscription(session,owner.remoteSessionCoordinator()->byId(session).generation,true);
        QTRY_VERIFY_WITH_TIMEOUT(!remoteStates.isEmpty()
            && remoteStates.last().first().toString()==QLatin1String("capture_error"),5000);
        QVERIFY(publisher.status().contains(QStringLiteral("capture is unavailable")));
        const auto epoch=transport->publicationId(); QVERIFY(!epoch.isEmpty());
        QTest::qWait(1000);
        QCOMPARE(transport->publicationId(),epoch);
        QCOMPARE(remoteStates.last().first().toString(),QStringLiteral("capture_error"));
        QVERIFY(publisher.status().contains(QStringLiteral("capture is unavailable")));
        QTRY_VERIFY_WITH_TIMEOUT(!transport->publicationId().isEmpty() && transport->publicationId()!=epoch
            && remoteStates.last().first().toString()==QLatin1String("capture_error"),5000);
        publisher.setSharingEnabled(false);
        QVERIFY(publisher.status().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(remoteStates.last().first().toString(),QStringLiteral("disabled"),3000);
    }
    void wirePreservesEpochSequenceAndCapturePts() {
        const auto epoch=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto payload=QByteArray::fromHex("fc010203"); AudioWire::Frame decoded;
        QVERIFY(AudioWire::parse(AudioWire::packet(epoch,3,123456789,payload),decoded));
        QCOMPARE(decoded.epoch,epoch); QCOMPARE(decoded.sequence,quint64(3)); QCOMPARE(decoded.timestampUs,qint64(123456789)); QCOMPARE(decoded.payload,payload);
        QVERIFY(AudioWire::parse(AudioWire::ack(epoch,3),decoded,true));
        QVERIFY(!AudioWire::parse(AudioWire::packet(epoch,0,1,payload),decoded));
        QVERIFY(!AudioWire::parse(AudioWire::packet(epoch,1,1,{}),decoded));
        QVERIFY(!AudioWire::parse(AudioWire::packet(epoch,1,1,QByteArray(1276,'x')),decoded));
    }
    void staleAudioCannotResetTheArrivalBaseline() {
        AudioPacketFreshness freshness;
        QCOMPARE(freshness.localTimeUs(5000000),qint64(-1));
        // A current video frame seeds the source clock before delayed audio.
        QVERIFY(freshness.observe(5000000,10000000));
        QCOMPARE(freshness.localTimeUs(5040000),qint64(10040000));
        QVERIFY(!freshness.observe(3000000,10010000));
        QVERIFY(!freshness.observe(3020000,10030000));
        QCOMPARE(freshness.localTimeUs(5040000),qint64(10040000));
        QVERIFY(freshness.observe(5040000,10040000));
        // A later TCP stall is dropped, then live delivery recovers directly.
        QVERIFY(!freshness.observe(5060000,12060000));
        QVERIFY(!freshness.observe(5080000,12060000));
        QCOMPARE(freshness.localTimeUs(5060000),qint64(10060000));
        QVERIFY(freshness.observe(7060000,12060000));
        freshness.reset();
        QCOMPARE(freshness.localTimeUs(5000000),qint64(-1));
        QVERIFY(freshness.observe(1000,100000000));
        // Ordinary relative clock drift stays below the 200 ppm allowance.
        QVERIFY(freshness.observe(3600001000LL,3700360000LL));
    }
    void frequentObservationsPreserveFractionalClockDrift() {
        AudioPacketFreshness freshness;
        QVERIFY(freshness.observe(10000000,20000000));
        // Several screens and audio can interleave observations more often
        // than the 5 ms needed to accumulate one microsecond at 200 ppm.
        for(qint64 i=1;i<=10000;++i)
            QVERIFY(freshness.observe(10000000+i*1000,20000000+i*1000+i/5));
        QVERIFY(qAbs(freshness.localTimeUs(20000000)-30002000)<=1);
        const auto before=freshness.localTimeUs(20000000);
        for(qint64 i=1;i<=1000;++i)
            QVERIFY(!freshness.observe(10000000+i*1000,40000000+i*1000));
        QCOMPARE(freshness.localTimeUs(20000000),before);
        freshness.reset();
        QVERIFY(freshness.observe(10000000,20000000));
        QCOMPARE(freshness.localTimeUs(20000000),qint64(30000000));
    }
    void startupReservesCaptureAndDeviceHeadroomImmediately() {
        AudioPlayoutPolicy policy;
        // A faster video observation maps source time without hiding the
        // source audio's60ms collection+20ms packet+codec lookahead latency.
        QVERIFY(!policy.observe(1000000,1086500,1000000));
        QVERIFY(policy.targetDelayUs()-86500 >= 60000);
        QVERIFY(policy.targetDelayUs()<=AudioPlayoutPolicy::MaximumDelayUs);
    }
    void jitterTargetAndRouteRecoveryStayBounded() {
        AudioPlayoutPolicy policy;
        QCOMPARE(policy.targetDelayUs(),qint64(120000));
        for(qint64 i=0;i<20;++i) {
            const qint64 source=1000000+i*20000;
            QVERIFY(!policy.observe(source,source+100000,source));
        }
        QCOMPARE(policy.targetDelayUs(),qint64(150000));
        // Very late TCP bursts cannot create a new epoch or exceed150ms.
        policy.reset();
        for(qint64 i=0;i<100;++i)
            QVERIFY(!policy.observe(1000000+i*20000,10000000,1000000+i*20000));
        QVERIFY(policy.targetDelayUs()<=150000);
        // A sustained path shift requires one second of source and arrival
        // progress, even if the video path remains fast.
        policy.reset();
        for(qint64 i=0;i<50;++i) {
            const qint64 source=1000000+i*20000;
            QVERIFY(!policy.observe(source,source+200000,source));
        }
        QCOMPARE(policy.targetDelayUs(),qint64(150000));
        QVERIFY(policy.observe(2000000,2200000,2000000));
        policy.reset();
        QCOMPARE(policy.targetDelayUs(),qint64(120000));
    }
    void captureSequenceGapsSurviveTransportAndRelay() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        QSignalSpy packets(&receiver,&AudioTransport::packetReceived);
        activate(owner,source,receiver,publisher,session);
        const auto payload=QByteArray::fromHex("fc010203");
        const auto capturedAt=MediaCaptureClock::nowUs();
        QVERIFY(publisher.sendPacket(payload,capturedAt,5));
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,3000);
        QVERIFY(!publisher.sendPacket(payload,capturedAt,6));
        QVERIFY(!publisher.sendPacket(payload,MediaCaptureClock::nowUs(),5));
        QTest::qWait(20);
        QVERIFY(publisher.sendPacket(payload,MediaCaptureClock::nowUs(),8));
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),2,3000);
        QCOMPARE(packets[0][2].toULongLong(),quint64(5));
        QCOMPARE(packets[1][2].toULongLong(),quint64(8));
    }
    void oneAudioPacketForThreeScreensAndImmediateMute() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        QSignalSpy packets(&receiver,&AudioTransport::packetReceived),epochs(&receiver,&AudioTransport::playbackStreamChanged);
        activate(owner,source,receiver,publisher,session); QVERIFY(publisher.isSupported());
        QCOMPARE(publisher.reservedSourceBps(),112000);
        const auto payload=QByteArray::fromHex("fc010203");
        const auto capturedAt=MediaCaptureClock::nowUs();
        QVERIFY(!publisher.sendPacket(payload,capturedAt-500000));
        QVERIFY(publisher.sendPacket(payload,capturedAt));
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,3000);
        QCOMPARE(packets[0][0].toByteArray(),payload); QCOMPARE(packets[0][1].toLongLong(),capturedAt);
        QCOMPARE(packets[0][2].toULongLong(),quint64(1));
        receiver.setSubscription(session,1,false);
        QVERIFY(receiver.viewerStreamId().isEmpty()); QVERIFY(!epochs.isEmpty()); QCOMPARE(epochs.last().first().toString(),QString());
        QTRY_VERIFY_WITH_TIMEOUT(!publisher.isPublishing(),3000); QCOMPARE(publisher.reservedSourceBps(),0);
        QVERIFY(!publisher.sendPacket(payload,MediaCaptureClock::nowUs())); QTest::qWait(50); QCOMPARE(packets.size(),1);
    }
    void sourcePipeReplacementChangesEpochAndKeepsCommandSession() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        QSignalSpy packets(&receiver,&AudioTransport::packetReceived);
        activate(owner,source,receiver,publisher,session);
        const auto oldPublication=publisher.publicationId(),oldStream=receiver.viewerStreamId();
        const auto sockets=publisher.findChildren<QWebSocket*>(); QCOMPARE(sockets.size(),1); sockets.first()->abort();
        QTRY_VERIFY_WITH_TIMEOUT(!publisher.publicationId().isEmpty() && publisher.publicationId()!=oldPublication,5000);
        QTRY_VERIFY_WITH_TIMEOUT(!receiver.viewerStreamId().isEmpty() && receiver.viewerStreamId()!=oldStream,5000);
        QVERIFY(owner.canIssueSessionCommands(session)); QVERIFY(source.canIssueSessionCommands(session));
        const auto payload=QByteArray::fromHex("fc010203");
        QVERIFY(publisher.sendPacket(payload,MediaCaptureClock::nowUs())); QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,3000);
        QCOMPARE(packets[0][2].toULongLong(),quint64(1));
    }
    void restartedCaptureProcessUsesAFreshPublicationSequence() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        QSignalSpy packets(&receiver,&AudioTransport::packetReceived);
        activate(owner,source,receiver,publisher,session);
        const auto oldPublication=publisher.publicationId(),oldStream=receiver.viewerStreamId();
        const auto payload=QByteArray::fromHex("fc010203");
        QVERIFY(publisher.sendPacket(payload,MediaCaptureClock::nowUs(),5));
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,3000);
        publisher.restartPublication();
        QVERIFY(!publisher.isPublishing());
        QTRY_VERIFY_WITH_TIMEOUT(publisher.isPublishing() && publisher.publicationId()!=oldPublication,5000);
        QTRY_VERIFY_WITH_TIMEOUT(!receiver.viewerStreamId().isEmpty() && receiver.viewerStreamId()!=oldStream,5000);
        QVERIFY(publisher.sendPacket(payload,MediaCaptureClock::nowUs(),1));
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),2,3000);
        QCOMPARE(packets.last()[2].toULongLong(),quint64(1));
        QVERIFY(owner.canIssueSessionCommands(session)); QVERIFY(source.canIssueSessionCommands(session));
    }
    void staleAudioPipeRetiresWithoutForgettingFasterVideo() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        QSignalSpy packets(&receiver,&AudioTransport::packetReceived),states(&receiver,&AudioTransport::remoteStateChanged);
        activate(owner,source,receiver,publisher,session);
        const auto oldStream=receiver.viewerStreamId();
        const auto payload=QByteArray::fromHex("fc010203");
        qint64 videoAt=0;
        bool unavailable=false;
        for(int i=0;i<75 && !unavailable;++i) {
            videoAt=MediaCaptureClock::nowUs();
            receiver.observeVideoTimestamp(videoAt,videoAt);
            if(publisher.isPublishing()) publisher.sendPacket(payload,videoAt-200000);
            QTest::qWait(20);
            for(const auto& state:states) if(state.first().toString()==QStringLiteral("timing_unavailable")) unavailable=true;
        }
        QVERIFY(unavailable); QVERIFY(packets.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(publisher.isPublishing() && !receiver.viewerStreamId().isEmpty()
            && receiver.viewerStreamId()!=oldStream,5000);
        // Reauthentication must not turn an old audio-only delay into a new
        // source clock when recent video already proved that clock's origin.
        QVERIFY(qAbs(receiver.playbackTimeUs(videoAt)-(videoAt+AudioPlayoutPolicy::InitialDelayUs))<1000);
        QVERIFY(publisher.sendPacket(payload,MediaCaptureClock::nowUs()-200000));
        QTest::qWait(30); QVERIFY(packets.isEmpty());
        QVERIFY(publisher.sendPacket(payload,MediaCaptureClock::nowUs()));
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,3000);
        QVERIFY(owner.canIssueSessionCommands(session)); QVERIFY(source.canIssueSessionCommands(session));
    }
    void consentAndSuspensionFenceQueuedPlayback() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        activate(owner,source,receiver,publisher,session);
        publisher.setSharingEnabled(false);
        QVERIFY(!publisher.isPublishing());
        QTRY_VERIFY_WITH_TIMEOUT(receiver.viewerStreamId().isEmpty(),3000);
        publisher.setSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(publisher.isPublishing() && !receiver.viewerStreamId().isEmpty(),5000);
        receiver.setSuspended(true); QVERIFY(receiver.viewerStreamId().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!publisher.isPublishing(),3000);
        receiver.setSuspended(false);
        QTRY_VERIFY_WITH_TIMEOUT(publisher.isPublishing() && !receiver.viewerStreamId().isEmpty(),5000);
        QVERIFY(owner.canIssueSessionCommands(session));
    }
    void automaticFallbackReservesTheCombinedSourceBudget() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        activate(owner,source,receiver,publisher,session);
        const auto epoch=publisher.publicationId();
        publisher.setSourceBudget(128000);
        QTRY_COMPARE_WITH_TIMEOUT(publisher.audioBitrateBps(),32000,4000);
        QCOMPARE(publisher.publicationId(),epoch); QCOMPARE(publisher.reservedSourceBps(),48000);
        QSignalSpy packets(&receiver,&AudioTransport::packetReceived);
        QVERIFY(publisher.sendPacket(QByteArray::fromHex("fc010203"),MediaCaptureClock::nowUs()));
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,3000);
    }
    void legacyWelcomeDoesNotTriggerAudioControlsOrRetries() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        connectPeers(owner,source,session);
        source.setScreenSharingEnabled(true); publisher.setSharingEnabled(true);
        receiver.setSubscription(session,owner.remoteSessionCoordinator()->byId(session).generation,true);
        QVERIFY(!publisher.isSupported()); QVERIFY(!receiver.isSupported());
        QTest::qWait(1200);
        QVERIFY(!publisher.isPublishing()); QVERIFY(receiver.viewerStreamId().isEmpty());
        QVERIFY2(!log.contains("TEST_UNEXPECTED_AUDIO"),log.constData());
        QVERIFY(owner.canIssueSessionCommands(session)); QVERIFY(source.canIssueSessionCommands(session));
    }
    void hardwareReservoirDoesNotTriggerNetworkQualityDowngrade() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        activate(owner,source,receiver,publisher,session);
        receiver.setOutputQuantumUs(100000);
        for(int i=0;i<4;++i) {
            receiver.sendPlaybackFeedback(0,240);
            QTest::qWait(550);
            QCOMPARE(publisher.audioBitrateBps(),96000);
        }
        // The same depth on an ordinary output really is excess buffering.
        receiver.setOutputQuantumUs(20000);
        for(int i=0;i<4;++i) {
            receiver.sendPlaybackFeedback(0,240);
            QTest::qWait(550);
        }
        QTRY_COMPARE_WITH_TIMEOUT(publisher.audioBitrateBps(),32000,3000);
    }
};
QTEST_GUILESS_MAIN(AudioTransportTest)
#include "tst_AudioTransport.moc"
