#include "backend/network/AudioTransport.h"
#include "backend/network/AudioWire.h"
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
    void oneAudioPacketForThreeScreensAndImmediateMute() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath("owner"),false),source(identities.filePath("source"),false);
        AudioTransport receiver(&owner),publisher(&source); QString session;
        QSignalSpy packets(&receiver,&AudioTransport::packetReceived),epochs(&receiver,&AudioTransport::playbackStreamChanged);
        activate(owner,source,receiver,publisher,session); QVERIFY(publisher.isSupported());
        QCOMPARE(publisher.reservedSourceBps(),112000);
        const auto payload=QByteArray::fromHex("fc010203");
        QVERIFY(publisher.sendPacket(payload,1000000));
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,3000);
        QCOMPARE(packets[0][0].toByteArray(),payload); QCOMPARE(packets[0][1].toLongLong(),qint64(1000000));
        QCOMPARE(packets[0][2].toULongLong(),quint64(1));
        receiver.setSubscription(session,1,false);
        QVERIFY(receiver.viewerStreamId().isEmpty()); QVERIFY(!epochs.isEmpty()); QCOMPARE(epochs.last().first().toString(),QString());
        QTRY_VERIFY_WITH_TIMEOUT(!publisher.isPublishing(),3000); QCOMPARE(publisher.reservedSourceBps(),0);
        QVERIFY(!publisher.sendPacket(payload,1020000)); QTest::qWait(50); QCOMPARE(packets.size(),1);
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
        QVERIFY(publisher.sendPacket(payload,2000000)); QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,3000);
        QCOMPARE(packets[0][2].toULongLong(),quint64(1));
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
        QVERIFY(publisher.sendPacket(QByteArray::fromHex("fc010203"),1000000));
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
};
QTEST_GUILESS_MAIN(AudioTransportTest)
#include "tst_AudioTransport.moc"
