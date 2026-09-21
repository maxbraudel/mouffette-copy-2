#include "backend/screensharing/ScreenSharingService.h"
#include "backend/screensharing/ScreenStreamCodec.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/project/ProjectManager.h"
#include "backend/managers/app/SettingsManager.h"
#include "backend/managers/network/ConnectionManager.h"
#include "backend/managers/system/SystemMonitor.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/WebSocketClient.h"
#include "backend/notifications/NotificationCenter.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "shared/rendering/MediaFrameSource.h"

#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QtTest>

// Exercise the production control/video relay and decoder without capturing
// the developer's desktop or asking the OS for screen recording permission.
class ScreenSharingServiceTest final : public QObject {
    Q_OBJECT
private:
    QProcess m_relay;
    QByteArray m_output;
    QString m_url;

    void configure(WebSocketClient& peer, const QString& name,
                   const QList<ScreenInfo>& screens = {ScreenInfo(0, 640, 360, 0, 0, true)}) {
        const auto advertise = [&peer, name, screens] {
            peer.registerClient(name, QStringLiteral("screen-sharing-test"),
                                screens, 50);
        };
        connect(&peer, &WebSocketClient::connected, &peer, advertise);
        connect(&peer, &WebSocketClient::localDeviceSnapshotRequested, &peer, advertise);
        connect(&peer, &WebSocketClient::remoteSessionTerminating, &peer,
                [&peer](const QJsonObject& event) {
            if (event.value("targetEndpointId").toString() == peer.endpointId())
                peer.acknowledgeRemoteSessionTeardown(event.value("remoteSessionId").toString(),
                    event.value("teardownId").toString(), true, true, true, 0, {}, 0);
        });
    }

    void connectPeers(WebSocketClient& owner, WebSocketClient& target, QString* session,
                      const QList<ScreenInfo>& targetScreens = {ScreenInfo(0, 640, 360, 0, 0, true)}) {
        configure(owner, QStringLiteral("viewer"));
        configure(target, QStringLiteral("publisher"), targetScreens);
        QSignalSpy registrations(&target, &WebSocketClient::registrationConfirmed);
        QSignalSpy opened(&owner, &WebSocketClient::remoteSessionOpened);
        owner.connectToServer(m_url);
        target.connectToServer(m_url);
        QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && !registrations.isEmpty(), 4000);
        QVERIFY(owner.openRemoteSession(target.endpointId()));
        QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 4000);
        *session = opened.first().first().toJsonObject().value("remoteSessionId").toString();
        QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(*session)
                                && target.canIssueSessionCommands(*session), 4000);
    }

    static QJsonObject header(const QJsonObject& grant, const ScreenStreamPacket& packet,
                              quint64 sequence) {
        return {{"remoteSessionId", grant.value("remoteSessionId")},
                {"generation", grant.value("generation")},
                {"streamId", grant.value("streamId")}, {"screenId", 0},
                {"sequence", double(sequence)}, {"width", packet.size.width()},
                {"height", packet.size.height()}, {"keyFrame", packet.keyFrame},
                {"codec", QStringLiteral("h264")}};
    }

    static SystemMonitor::ScreenProvider emptyDesktop() {
        return [](bool* valid) { *valid = true; return QList<LocalScreenTopology::Screen>{}; };
    }

    static QWebSocket* videoSocket(WebSocketClient& peer) {
        for (auto* socket : peer.findChildren<QWebSocket*>())
            if (QUrlQuery(socket->requestUrl()).queryItemValue(QStringLiteral("channel"))
                == QLatin1String("screen")) return socket;
        return nullptr;
    }

    static QJsonObject authenticatedVideoMessage(const WebSocketClient& peer, QJsonObject message) {
        message.insert("protocolVersion", WebSocketClient::ProtocolVersion);
        message.insert("serverBootId", peer.serverBootId());
        message.insert("messageId", QUuid::createUuid().toString(QUuid::WithoutBraces));
        message.insert("connectionGeneration", double(peer.connectionGeneration()));
        return message;
    }

private slots:
    void init() {
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        QVERIFY2(!node.isEmpty(), "Node.js is required for screen sharing integration tests");
        m_output.clear();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("MOUFFETTE_SCREEN_SHARED_ENABLED"),
            QByteArray(QTest::currentTestFunction()).startsWith("shared") ? QStringLiteral("true") : QStringLiteral("false"));
        m_relay.setProcessEnvironment(environment);
        m_relay.setProcessChannelMode(QProcess::MergedChannels);
        connect(&m_relay, &QProcess::readyReadStandardOutput, this, [this] {
            m_output += m_relay.readAllStandardOutput();
        });
        m_relay.start(node, {QStringLiteral(MOUFFETTE_RELAY_FIXTURE), QStringLiteral("0")});
        QVERIFY(m_relay.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(m_output.contains("TEST_READY "), 5000);
        const auto match = QRegularExpression(QStringLiteral("TEST_READY (\\d+)"))
                               .match(QString::fromUtf8(m_output));
        QVERIFY2(match.hasMatch(), m_output.constData());
        m_url = QStringLiteral("ws://127.0.0.1:%1").arg(match.captured(1));
    }

    void cleanup() {
        if (m_relay.state() != QProcess::NotRunning) {
            m_relay.write("{\"action\":\"shutdown\"}\n");
            if (!m_relay.waitForFinished(2000)) {
                m_relay.kill();
                m_relay.waitForFinished(2000);
            }
        }
        disconnect(&m_relay, nullptr, this, nullptr);
    }

    void sharedPublicationFansOutOneUploadAndRetainsOtherViewer() {
        QTemporaryDir identities;
        WebSocketClient first(identities.filePath("first"), false);
        WebSocketClient second(identities.filePath("second"), false);
        WebSocketClient source(identities.filePath("source"), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService firstView(&first, &monitor), secondView(&second, &monitor);
        QSignalSpy firstFrames(&firstView, &ScreenSharingService::frameReady);
        QSignalSpy secondFrames(&secondView, &ScreenSharingService::frameReady);
        QSignalSpy publication(&source, &WebSocketClient::screenPublicationRequested);
        QSignalSpy sourceReceipts(&source, &WebSocketClient::screenSourceFeedback);
        QSignalSpy forwardedViewerFeedback(&source, &WebSocketClient::screenShareFeedbackReceived);
        QString firstSession;
        connectPeers(first, source, &firstSession);
        configure(second, "second-viewer");
        QSignalSpy secondOpened(&second, &WebSocketClient::remoteSessionOpened);
        second.connectToServer(m_url);
        QTRY_VERIFY_WITH_TIMEOUT(second.isConnected(), 4000);
        QVERIFY(second.openRemoteSession(source.endpointId()));
        QTRY_VERIFY_WITH_TIMEOUT(!secondOpened.isEmpty(), 4000);
        const auto secondSession = secondOpened.last().first().toJsonObject().value("remoteSessionId").toString();
        QTRY_VERIFY_WITH_TIMEOUT(second.canIssueSessionCommands(secondSession)
            && source.canIssueSessionCommands(secondSession), 4000);
        source.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(source.isScreenPublicationChannelConnected(), 4000);
        QVERIFY(source.sharedScreenPublicationSupported());
        firstView.setViewedEndpoint(source.endpointId());
        secondView.setViewedEndpoint(source.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!publication.isEmpty()
            && publication.last().first().toJsonObject().value("enabled").toBool(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(first.isScreenChannelConnected() && second.isScreenChannelConnected(), 4000);
        // Wait for both subscriptions' control states before the one upstream
        // packet. This avoids depending on cross-socket handshake ordering.
        QTest::qWait(150);
        const auto grant = publication.last().first().toJsonObject();
        const auto id = grant.value("publicationId").toString();
        QVERIFY(!id.isEmpty());
        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        ScreenStreamEncoder encoder(false);
        QString error;
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        auto metadata = QJsonObject{{"publicationId", id}, {"screenId", 0}, {"layer", "main"},
            {"sequence", 1}, {"width", 640}, {"height", 360}, {"keyFrame", true},
            {"codec", "h264"}, {"bitrateBps", 200000}, {"fps", 5}};
        QVERIFY(source.sendScreenPublicationFrame(metadata, packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(firstFrames.count(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(secondFrames.count(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(sourceReceipts.count(), 1, 4000);
        bool feedbackSent = false;
        QTRY_VERIFY_WITH_TIMEOUT(feedbackSent || (feedbackSent = first.sendScreenViewFeedback(firstSession,
            first.remoteSessionCoordinator()->byId(firstSession).generation, 0, 600, 10)), 2000);
        QTest::qWait(100);
        QCOMPARE(sourceReceipts.count(), 1);
        QVERIFY(forwardedViewerFeedback.isEmpty());
        firstView.setViewedEndpoint({});
        QTest::qWait(150);
        QCOMPARE(publication.last().first().toJsonObject().value("publicationId").toString(), id);
        QVERIFY(publication.last().first().toJsonObject().value("enabled").toBool());
        metadata.insert("sequence", 2);
        QTRY_VERIFY_WITH_TIMEOUT(source.screenSendWindowOpen(), 4000);
        QVERIFY(source.sendScreenPublicationFrame(metadata, packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(secondFrames.count(), 2, 4000);
        QCOMPARE(firstFrames.count(), 1);
        QVERIFY(secondView.isRemoteScreenAvailable(source.endpointId()));
        QVERIFY(first.canIssueSessionCommands(firstSession));
        QVERIFY(second.canIssueSessionCommands(secondSession));
        secondView.setViewedEndpoint({});
        QTRY_VERIFY_WITH_TIMEOUT(!publication.last().first().toJsonObject().value("enabled").toBool(), 4000);
        metadata.insert("sequence", 3);
        QVERIFY(!source.sendScreenPublicationFrame(metadata, packet.annexB));
        first.disconnect(); second.disconnect(); source.disconnect();
    }

    void sharedPublicationSelectsIndependentDecodableLayers() {
        QTemporaryDir identities;
        WebSocketClient first(identities.filePath("first"), false), second(identities.filePath("second"), false),
            source(identities.filePath("source"), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService firstView(&first, &monitor), secondView(&second, &monitor);
        QSignalSpy firstFrames(&firstView, &ScreenSharingService::frameReady);
        QSignalSpy secondFrames(&secondView, &ScreenSharingService::frameReady);
        QSignalSpy publications(&source, &WebSocketClient::screenPublicationRequested);
        QString firstSession;
        connectPeers(first, source, &firstSession, {ScreenInfo(0, 1920, 1080, 0, 0, true)});
        configure(second, "small-viewer");
        QSignalSpy opened(&second, &WebSocketClient::remoteSessionOpened);
        second.connectToServer(m_url);
        QTRY_VERIFY_WITH_TIMEOUT(second.isConnected(), 4000);
        QVERIFY(second.openRemoteSession(source.endpointId()));
        QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 4000);
        const auto secondSession = opened.last().first().toJsonObject().value("remoteSessionId").toString();
        QTRY_VERIFY_WITH_TIMEOUT(second.canIssueSessionCommands(secondSession)
            && source.canIssueSessionCommands(secondSession), 4000);
        source.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(source.isScreenPublicationChannelConnected(), 4000);
        firstView.setViewedEndpoint(source.endpointId());
        firstView.setViewedScreens({QJsonObject{{"screenId", 0}, {"maximumEdge", 1920}}});
        secondView.setViewedEndpoint(source.endpointId());
        secondView.setViewedScreens({QJsonObject{{"screenId", 0}, {"maximumEdge", 320}}});
        const auto lowRequested = [&] {
            if (publications.isEmpty()) return false;
            const auto screens = publications.last().first().toJsonObject().value("screens").toArray();
            return !screens.isEmpty() && screens.first().toObject().value("layers").toArray().contains("low");
        };
        QTRY_VERIFY_WITH_TIMEOUT(lowRequested(), 4000);
        const auto id = publications.last().first().toJsonObject().value("publicationId");
        source.setScreenVideoBudget(2000000);
        ScreenStreamEncoder mainEncoder(false), lowEncoder(false);
        ScreenStreamProfile mainProfile; mainProfile.maximumEdge = 1280; mainProfile.bitrateBps = 800000;
        ScreenStreamProfile lowProfile; lowProfile.maximumEdge = 320; lowProfile.bitrateBps = 100000;
        mainEncoder.setProfile(mainProfile); lowEncoder.setProfile(lowProfile);
        QImage big(1280, 720, QImage::Format_RGBA8888), small(320, 180, QImage::Format_RGBA8888);
        big.fill(Qt::blue); small.fill(Qt::green);
        QString error;
        const auto mainPackets = mainEncoder.encode(QVideoFrame(big), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto lowPackets = lowEncoder.encode(QVideoFrame(small), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(mainPackets.size(), 1); QCOMPARE(lowPackets.size(), 1);
        const auto publish = [&](const QString& layer, const ScreenStreamPacket& packet, int sequence, int bitrate) {
            return source.sendScreenPublicationFrame({{"publicationId", id}, {"screenId", 0}, {"layer", layer},
                {"sequence", sequence}, {"width", packet.size.width()}, {"height", packet.size.height()},
                {"keyFrame", packet.keyFrame}, {"codec", "h264"}, {"bitrateBps", bitrate}, {"fps", 10}}, packet.annexB);
        };
        QVERIFY(publish("low", lowPackets.first(), 1, 100000));
        QTRY_COMPARE_WITH_TIMEOUT(firstFrames.count(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(secondFrames.count(), 1, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(source.screenSendWindowOpen(), 4000);
        QVERIFY(publish("main", mainPackets.first(), 1, 800000));
        QTRY_COMPARE_WITH_TIMEOUT(firstFrames.count(), 2, 4000);
        QCOMPARE(qvariant_cast<QVideoFrame>(firstFrames.last().at(2)).size(), big.size());
        QCOMPARE(qvariant_cast<QVideoFrame>(secondFrames.last().at(2)).size(), small.size());
        QCOMPARE(secondFrames.count(), 1);
        const auto nextMain = mainEncoder.encode(QVideoFrame(big), false, error);
        const auto nextLow = lowEncoder.encode(QVideoFrame(small), false, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(nextMain.size(), 1); QCOMPARE(nextLow.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(source.screenSendWindowOpen(), 4000);
        QVERIFY(publish("low", nextLow.first(), 2, 100000));
        QTRY_COMPARE_WITH_TIMEOUT(secondFrames.count(), 2, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(source.screenSendWindowOpen(), 4000);
        QVERIFY(publish("main", nextMain.first(), 2, 800000));
        QTRY_COMPARE_WITH_TIMEOUT(firstFrames.count(), 3, 4000);
        QCOMPARE(qvariant_cast<QVideoFrame>(firstFrames.last().at(2)).size(), big.size());
        QCOMPARE(qvariant_cast<QVideoFrame>(secondFrames.last().at(2)).size(), small.size());
        firstView.stop(); secondView.stop();
        first.disconnect(); second.disconnect(); source.disconnect();
    }

    void sharedPublicationKeepsPublishingAcrossOwnViewerSocketFailure() {
        QTemporaryDir identities;
        WebSocketClient viewer(identities.filePath("viewer"), false);
        WebSocketClient source(identities.filePath("source"), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService display(&viewer, &monitor);
        QSignalSpy frames(&display, &ScreenSharingService::frameReady);
        QSignalSpy publications(&source, &WebSocketClient::screenPublicationRequested);
        QSignalSpy publishLost(&source, &WebSocketClient::screenPublicationChannelUnavailable);
        QString session;
        connectPeers(viewer, source, &session);
        source.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(source.isScreenPublicationChannelConnected(), 4000);
        display.setViewedEndpoint(source.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!publications.isEmpty()
            && publications.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = publications.last().first().toJsonObject();
        QImage image(640, 360, QImage::Format_RGBA8888); image.fill(Qt::blue);
        ScreenStreamEncoder encoder(false); QString error;
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto metadata = QJsonObject{{"publicationId", grant.value("publicationId")},
            {"screenId", 0}, {"layer", "main"}, {"sequence", 1}, {"width", 640}, {"height", 360},
            {"keyFrame", true}, {"codec", "h264"}, {"bitrateBps", 200000}, {"fps", 5}};
        auto* ownViewSocket = videoSocket(source);
        QVERIFY(ownViewSocket);
        // The first-created media socket is the role=view connection. Losing
        // it must not retire the independent role=publish token/publication.
        ownViewSocket->abort();
        QVERIFY(source.isScreenPublicationChannelConnected());
        QVERIFY(source.sendScreenPublicationFrame(metadata, packets.first().annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 4000);
        QVERIFY(publishLost.isEmpty());
        QCOMPARE(publications.last().first().toJsonObject().value("publicationId"), grant.value("publicationId"));
        QVERIFY(source.canIssueSessionCommands(session));
        source.setScreenSharingEnabled(false);
        QTRY_VERIFY_WITH_TIMEOUT(!display.isRemoteScreenAvailable(source.endpointId()), 4000);
        auto stale = metadata; stale.insert("sequence", 2);
        QVERIFY(!source.sendScreenPublicationFrame(stale, packets.first().annexB));
        display.stop(); viewer.disconnect(); source.disconnect();
    }

    void consentDecodeDetachAndRevocation() {
        QTemporaryDir identities;
        QVERIFY(identities.isValid());
        WebSocketClient owner(identities.filePath(QStringLiteral("owner")), false);
        WebSocketClient target(identities.filePath(QStringLiteral("target")), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService viewer(&owner, &monitor);
        QSignalSpy frames(&viewer, &ScreenSharingService::frameReady);
        QSignalSpy cleared(&viewer, &ScreenSharingService::framesCleared);
        QSignalSpy issues(&viewer, &ScreenSharingService::remoteIssue);
        QSignalSpy states(&owner, &WebSocketClient::screenShareStateReceived);
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        QString session;
        connectPeers(owner, target, &session);
        QVERIFY(!session.isEmpty());
        states.clear(); // Await the subscription response, not the session's initial snapshot.
        viewer.setViewedEndpoint(target.endpointId());
        QVERIFY(viewer.isRemoteScreenLoading(target.endpointId()));
        QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("reason") == QLatin1String("disabled"), 4000);
        QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
        QVERIFY(frames.isEmpty());
        QCOMPARE(issues.count(), 1);
        QVERIFY(issues.last().at(1).toString().contains(QStringLiteral("disabled")));

        target.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(owner.isScreenChannelConnected() && target.isScreenChannelConnected(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const QJsonObject firstGrant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == firstGrant.value("streamId"), 4000);
        QVERIFY(viewer.isRemoteScreenLoading(target.endpointId()));
        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(QColor(30, 190, 70));
        ScreenStreamEncoder encoder(false);
        QString error;
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        QVERIFY(packet.keyFrame);
        QVERIFY(target.sendScreenFrame(header(firstGrant, packet, 1), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 4000);
        QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
        QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));
        QCOMPARE(issues.count(), 1); // First successful frame is silent.
        QCOMPARE(frames.first().at(0).toString(), target.endpointId());
        QCOMPARE(frames.first().at(1).toInt(), 0);
        auto decoded = qvariant_cast<QVideoFrame>(frames.first().at(2));
        QCOMPARE(decoded.size(), image.size());
        QCOMPARE(decoded.surfaceFormat().colorSpace(), QVideoFrameFormat::ColorSpace_BT709);
        QCOMPARE(decoded.surfaceFormat().colorRange(), QVideoFrameFormat::ColorRange_Video);
        QCOMPARE(decoded.pixelFormat(), QVideoFrameFormat::Format_YUV420P);
        QVERIFY(decoded.map(QVideoFrame::ReadOnly));
        // Verify the BT.709 limited-range planes the GPU consumes. Qt's
        // offscreen RGB conversion can use a different YUV color matrix.
        const int luma = decoded.bits(0)[100 * decoded.bytesPerLine(0) + 100];
        const int blueChroma = decoded.bits(1)[50 * decoded.bytesPerLine(1) + 50];
        const int redChroma = decoded.bits(2)[50 * decoded.bytesPerLine(2) + 50];
        decoded.unmap();
        QVERIFY(qAbs(luma - 142) < 8);
        QVERIFY(qAbs(blueChroma - 91) < 8);
        QVERIFY(qAbs(redChroma - 63) < 8);

        // Inject at the transport signal boundary to place a decode in flight,
        // then leave the canvas before the completion callback can run.
        owner.screenFrameReceived(header(firstGrant, packet, 2), packet.annexB);
        viewer.setViewedEndpoint({});
        QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
        QTRY_VERIFY_WITH_TIMEOUT(viewer.findChildren<QFutureWatcherBase*>().isEmpty(), 4000);
        QCOMPARE(frames.count(), 1);
        QVERIFY(!cleared.isEmpty());
        QCOMPARE(issues.count(), 1); // Intentional detachment is silent.

        viewer.setViewedEndpoint(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool()
            && grants.last().first().toJsonObject().value("streamId") != firstGrant.value("streamId"), 4000);
        const auto nextGrant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("streamId")
                                == nextGrant.value("streamId"), 4000);
        owner.screenFrameReceived(header(firstGrant, packet, 3), packet.annexB);
        QVERIFY(target.sendScreenFrame(header(nextGrant, packet, 1), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 2, 4000);

        const int oldClears = cleared.count();
        target.setScreenSharingEnabled(false);
        QTRY_VERIFY_WITH_TIMEOUT(!states.last().first().toJsonObject().value("enabled").toBool(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(cleared.count() > oldClears, 4000);
        QCOMPARE(issues.count(), 2); // Consent was revoked after recovery.
        QVERIFY(!target.sendScreenFrame(header(nextGrant, packet, 2), packet.annexB));
        owner.screenFrameReceived(header(nextGrant, packet, 2), packet.annexB);
        QTest::qWait(100);
        QCOMPARE(frames.count(), 2);
        viewer.stop();
        QCOMPARE(issues.count(), 2);
        owner.disconnect();
        target.disconnect();
    }

    void captureFailuresReachTheViewerAndClearItsFrames() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath(QStringLiteral("owner")), false);
        WebSocketClient target(identities.filePath(QStringLiteral("target")), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService viewer(&owner, &monitor);
        QSignalSpy frames(&viewer, &ScreenSharingService::frameReady);
        QSignalSpy cleared(&viewer, &ScreenSharingService::framesCleared);
        QSignalSpy issues(&viewer, &ScreenSharingService::remoteIssue);
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        QSignalSpy states(&owner, &WebSocketClient::screenShareStateReceived);
        QString session;
        connectPeers(owner, target, &session);
        QVERIFY(!session.isEmpty());
        target.setScreenSharingEnabled(true);
        // Consent and subscribe use different clients' control sockets. Wait
        // for the publisher handshake (issued after its consent on the same
        // ordered socket) before asserting a warning-free viewer handshake.
        QTRY_VERIFY_WITH_TIMEOUT(target.isScreenChannelConnected(), 4000);
        viewer.setViewedEndpoint(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);
        QVERIFY(issues.isEmpty()); // Normal connection progress is silent.

        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        ScreenStreamEncoder encoder(false);
        QString error;
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        quint64 sequence = 0;
        for (const auto& reason : {QStringLiteral("permission_denied"), QStringLiteral("capture_error"),
                                   QStringLiteral("unavailable"), QStringLiteral("error")}) {
            QVERIFY(target.sendScreenShareStatus(session,
                quint64(grant.value("generation").toDouble()), QStringLiteral("starting")));
            QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("reason")
                                    == QLatin1String("starting"), 4000);
            QVERIFY(viewer.isRemoteScreenLoading(target.endpointId()));
            const int oldFrames = frames.count();
            QVERIFY(target.sendScreenFrame(header(grant, packet, ++sequence), packet.annexB));
            QTRY_COMPARE_WITH_TIMEOUT(frames.count(), oldFrames + 1, 4000);
            QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));
            QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
            const int oldIssues = issues.count();
            const int oldClears = cleared.count();
            QVERIFY(target.sendScreenShareStatus(session,
                quint64(grant.value("generation").toDouble()), reason));
            QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("reason") == reason, 4000);
            QTRY_VERIFY_WITH_TIMEOUT(cleared.count() > oldClears, 4000);
            QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
            QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
            QCOMPARE(issues.count(), oldIssues + 1);
            QCOMPARE(issues.last().first().toString(), target.endpointId());
            const auto message = issues.last().at(1).toString();
            if (reason == QLatin1String("permission_denied")) {
                QVERIFY2(message.contains(QStringLiteral("Allow screen recording")), qPrintable(message));
                QVERIFY2(message.contains(QStringLiteral("system settings")), qPrintable(message));
            } else {
                QVERIFY2(message.contains(QStringLiteral("could not be captured")), qPrintable(message));
            }
            QVERIFY2(!message.contains(QStringLiteral("Waiting")), qPrintable(message));
            // Simulate an old keyframe overtaken by the error on the separate
            // control channel. It must not replace the failure message/image.
            QVERIFY(target.sendScreenFrame(header(grant, packet, ++sequence), packet.annexB));
            QTest::qWait(100);
            QCOMPARE(frames.count(), oldFrames + 1);
            QCOMPARE(issues.last().at(1).toString(), message);
            // Repeated state broadcasts must not repeat the same toast.
            owner.screenShareStateReceived(states.last().first().toJsonObject());
            QCOMPARE(issues.count(), oldIssues + 1);
        }
        viewer.stop();
        owner.disconnect();
        target.disconnect();
    }

    void missingAndStalledFramesReportOneIssueUntilRecovery() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath(QStringLiteral("owner")), false);
        WebSocketClient target(identities.filePath(QStringLiteral("target")), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService viewer(&owner, &monitor);
        QSignalSpy issues(&viewer, &ScreenSharingService::remoteIssue);
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        QSignalSpy states(&owner, &WebSocketClient::screenShareStateReceived);
        QString session;
        connectPeers(owner, target, &session);
        target.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(target.isScreenChannelConnected(), 4000);
        viewer.setViewedEndpoint(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);
        QVERIFY(issues.isEmpty());
        QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
        QVERIFY(viewer.isRemoteScreenLoading(target.endpointId()));
        QTRY_COMPARE_WITH_TIMEOUT(issues.count(), 1, AppConfig::instance().screenFirstFrameTimeoutMs() + 2000);
        QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
        QVERIFY(issues.last().at(1).toString().contains(QStringLiteral("did not respond")));
        viewer.refresh();
        QCOMPARE(issues.count(), 1);

        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        ScreenStreamEncoder encoder(false);
        QString error;
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        QVERIFY(target.sendScreenFrame(header(grant, packet, 1), packet.annexB));
        QTRY_VERIFY_WITH_TIMEOUT(viewer.isRemoteScreenAvailable(target.endpointId()), 4000);
        QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
        QCOMPARE(issues.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(issues.count(), 2, AppConfig::instance().screenStaleTimeoutMs() + 2000);
        QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
        QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
        QVERIFY(issues.last().at(1).toString().contains(QStringLiteral("stopped updating")));
        viewer.refresh();
        QCOMPARE(issues.count(), 2);
        QVERIFY(target.sendScreenFrame(header(grant, packet, 2), packet.annexB));
        QTRY_VERIFY_WITH_TIMEOUT(viewer.isRemoteScreenAvailable(target.endpointId()), 4000);
        QCOMPARE(issues.count(), 2);
        auto lostChannel = states.last().first().toJsonObject();
        lostChannel.insert(QStringLiteral("enabled"), false);
        lostChannel.insert(QStringLiteral("reason"), QStringLiteral("channel_unavailable"));
        lostChannel.insert(QStringLiteral("streamId"), QString());
        owner.screenShareStateReceived(lostChannel);
        QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
        QCOMPARE(issues.count(), 3);
        QVERIFY(issues.last().at(1).toString().contains(QStringLiteral("connection is unavailable")));
        owner.screenShareStateReceived(lostChannel);
        QCOMPARE(issues.count(), 3);
        viewer.setViewedEndpoint({});
        QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
        QCOMPARE(issues.count(), 3); // Hiding the canvas is not a connection error.
        viewer.stop();
        owner.disconnect();
        target.disconnect();
    }

    void missingPredecessorRecoversOnlyWithAKeyframe() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath(QStringLiteral("owner")), false);
        WebSocketClient target(identities.filePath(QStringLiteral("target")), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService viewer(&owner, &monitor);
        QSignalSpy frames(&viewer, &ScreenSharingService::frameReady);
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        QSignalSpy states(&owner, &WebSocketClient::screenShareStateReceived);
        QString session;
        connectPeers(owner, target, &session);
        QVERIFY(!session.isEmpty());
        target.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(target.isScreenChannelConnected(), 4000);
        viewer.setViewedEndpoint(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);
        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        ScreenStreamEncoder encoder(false);
        QString error;
        const auto first = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(first.size(), 1);
        QVERIFY(target.sendScreenFrame(header(grant, first.first(), 1), first.first().annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 4000);

        const auto delta = encoder.encode(QVideoFrame(image), false, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(delta.size(), 1);
        QVERIFY(!delta.first().keyFrame);
        // The relay deliberately debounces recovery requests for one second.
        QTest::qWait(1100);
        QSignalSpy keyRequests(&target, &WebSocketClient::screenShareKeyFrameRequested);
        // Exercise the service queue's own discontinuity fence, after the wire
        // validation covered above. P-frames must not use a broken reference.
        owner.screenFrameReceived(header(grant, delta.first(), 3), delta.first().annexB);
        QTRY_VERIFY_WITH_TIMEOUT(!keyRequests.isEmpty(), 2000);
        QCOMPARE(frames.count(), 1);
        const auto recovery = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(recovery.size(), 1);
        QVERIFY(recovery.first().keyFrame);
        owner.screenFrameReceived(header(grant, recovery.first(), 4), recovery.first().annexB);
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 2, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.findChildren<QFutureWatcherBase*>().isEmpty(), 4000);

        // A stalled GUI can deliver a burst before the decoder completion
        // callback. Backpressure must discard the obsolete reference chain,
        // including the in-flight result, and recover with one fresh IDR.
        for (quint64 sequence = 5; sequence <= 40; ++sequence)
            owner.screenFrameReceived(header(grant, delta.first(), sequence), delta.first().annexB);
        QVERIFY(viewer.findChildren<QFutureWatcherBase*>().size() <= 1);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.findChildren<QFutureWatcherBase*>().isEmpty(), 4000);
        QCOMPARE(frames.count(), 2);
        owner.screenFrameReceived(header(grant, recovery.first(), 41), recovery.first().annexB);
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 3, 4000);
        viewer.stop();
        owner.disconnect();
        target.disconnect();
    }

    void viewportRemovalAndReadditionPreserveSessionAndFenceDecoding() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath(QStringLiteral("owner")), false);
        WebSocketClient target(identities.filePath(QStringLiteral("target")), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService viewer(&owner, &monitor);
        QSignalSpy frames(&viewer, &ScreenSharingService::frameReady);
        QSignalSpy cleared(&viewer, &ScreenSharingService::frameCleared);
        QSignalSpy issues(&viewer, &ScreenSharingService::remoteIssue);
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        QSignalSpy states(&owner, &WebSocketClient::screenShareStateReceived);
        QSignalSpy sourceSamples(&target, &WebSocketClient::screenSourceFeedback);
        QString session;
        connectPeers(owner, target, &session);
        target.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(target.isScreenChannelConnected(), 4000);
        viewer.setViewedEndpoint(target.endpointId());
        const QJsonArray demand{QJsonObject{{"screenId", 0}, {"maximumEdge", 640}}};
        viewer.setViewedScreens(demand);
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool()
            && grants.last().first().toJsonObject().value("screens").toArray() == demand, 4000);
        const auto grant = grants.last().first().toJsonObject();
        const auto generation = quint64(grant.value("generation").toDouble());
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);
        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::cyan);
        ScreenStreamEncoder encoder(false);
        QString error;
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        QVERIFY(target.sendScreenFrame(header(grant, packet, 1), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 4000);

        // Remove the display while its second decode is in flight. The old
        // completion cannot put its pixels back into the now-hidden canvas.
        owner.screenFrameReceived(header(grant, packet, 2), packet.annexB);
        viewer.setViewedScreens({});
        QCOMPARE(cleared.count(), 1);
        QCOMPARE(cleared.first().at(1).toInt(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.findChildren<QFutureWatcherBase*>().isEmpty(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(grants.last().first().toJsonObject().value("screens").toArray().isEmpty()
            && states.last().first().toJsonObject().value("screens").toArray().isEmpty(), 4000);
        QCOMPARE(grants.last().first().toJsonObject().value("streamId"), grant.value("streamId"));
        QVERIFY(grants.last().first().toJsonObject().value("enabled").toBool());
        QCOMPARE(frames.count(), 1);
        QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
        QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));
        QVERIFY(issues.isEmpty());

        // A packet already in the publisher's video queue is consumed/ACKed
        // by the relay, but it must not be forwarded after viewport removal.
        const int previousSamples = sourceSamples.count();
        QVERIFY(target.sendScreenFrame(header(grant, packet, 2), packet.annexB));
        QTRY_VERIFY_WITH_TIMEOUT(sourceSamples.count() > previousSamples, 4000);
        QCOMPARE(frames.count(), 1);
        QVERIFY(owner.canIssueSessionCommands(session));
        QVERIFY(target.canIssueSessionCommands(session));

        const QJsonArray smallerDemand{QJsonObject{{"screenId", 0}, {"maximumEdge", 320}}};
        viewer.setViewedScreens(smallerDemand);
        QTRY_VERIFY_WITH_TIMEOUT(grants.last().first().toJsonObject().value("screens").toArray() == smallerDemand
            && states.last().first().toJsonObject().value("screens").toArray() == smallerDemand, 4000);
        QCOMPARE(grants.last().first().toJsonObject().value("streamId"), grant.value("streamId"));
        QVERIFY(viewer.isRemoteScreenLoading(target.endpointId()));
        // The previous encode remains valid during a demand resize; the
        // negotiated decoder capability is a separate, larger bound.
        QVERIFY(target.sendScreenFrame(header(grant, packet, 3), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 2, 4000);
        QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));
        QVERIFY(issues.isEmpty());
        QCOMPARE(owner.remoteSessionCoordinator()->byId(session).generation, generation);
        viewer.stop();
        owner.disconnect();
        target.disconnect();
    }

    void feedbackUsesExactReceiptsAndLeavesCommandsResponsive() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath(QStringLiteral("owner")), false);
        WebSocketClient target(identities.filePath(QStringLiteral("target")), false);
        QString session;
        connectPeers(owner, target, &session);
        const auto generation = owner.remoteSessionCoordinator()->byId(session).generation;
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        QSignalSpy states(&owner, &WebSocketClient::screenShareStateReceived);
        QSignalSpy frames(&owner, &WebSocketClient::screenFrameReceived);
        QSignalSpy sourceSamples(&target, &WebSocketClient::screenSourceFeedback);
        QSignalSpy downstreamSamples(&target, &WebSocketClient::screenShareFeedbackReceived);
        QSignalSpy cursor(&owner, &WebSocketClient::remoteCursorReceived);
        QSignalSpy channelLost(&target, &WebSocketClient::screenChannelUnavailable);
        QSignalSpy disconnected(&target, &WebSocketClient::disconnected);
        target.setScreenSharingEnabled(true);
        QVERIFY(owner.setScreenShareSubscription(session, generation, true,
            QJsonArray{QJsonObject{{"screenId", 0}, {"maximumEdge", 640}}}));
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);
        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::yellow);
        ScreenStreamEncoder encoder(false);
        QString error;
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        QVERIFY(target.sendScreenFrame(header(grant, packet, 1), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(sourceSamples.count(), 1, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(!downstreamSamples.isEmpty(), 4000);
        QVERIFY(sourceSamples.first().at(0).toInt() >= 0);
        QVERIFY(!sourceSamples.first().at(1).toBool());
        QCOMPARE(downstreamSamples.first().first().toJsonObject().value("streamId"), grant.value("streamId"));

        // Exercise wire parsing at the QWebSocket delivery boundary: forged
        // and duplicate ACK tuples do not create capacity/RTT samples.
        auto* socket = videoSocket(target);
        QVERIFY(socket);
        for (const int sequence : {999, 1}) {
            const auto ack = authenticatedVideoMessage(target, {{"type", "screen_frame_ack"},
                {"streamId", grant.value("streamId")}, {"screenId", 0}, {"sequence", sequence}});
            socket->textMessageReceived(QString::fromUtf8(QJsonDocument(ack).toJson(QJsonDocument::Compact)));
        }
        QCOMPARE(sourceSamples.count(), 1);
        const int reports = downstreamSamples.count();
        auto stale = downstreamSamples.first().first().toJsonObject();
        stale.insert("streamId", QUuid::createUuid().toString(QUuid::WithoutBraces));
        socket->textMessageReceived(QString::fromUtf8(QJsonDocument(stale).toJson(QJsonDocument::Compact)));
        QCOMPARE(downstreamSamples.count(), reports);

        QVERIFY(owner.sendScreenViewFeedback(session, generation, 0, 250, 2));
        for (int i = 0; i < 100; ++i)
            QVERIFY(!owner.sendScreenViewFeedback(session, generation, 0, 250, 2));
        QVERIFY(target.sendRemoteCursor(session, generation, 1, true, 0, QPointF(12, 34)));
        QTRY_COMPARE_WITH_TIMEOUT(cursor.count(), 1, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(downstreamSamples.last().first().toJsonObject().value("congested").toBool(), 4000);
        QVERIFY(owner.canIssueSessionCommands(session));
        QVERIFY(target.canIssueSessionCommands(session));
        QVERIFY(owner.isScreenChannelConnected());
        QVERIFY(target.isScreenChannelConnected());
        QVERIFY(channelLost.isEmpty());
        QVERIFY(disconnected.isEmpty());
        QVERIFY(owner.setScreenShareSubscription(session, generation, false));
        owner.disconnect();
        target.disconnect();
    }

    void viewerPreferenceControlsRuntimeSubscriptionAndCanvas() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto previousProfile = RuntimeProfile::context();
        const auto restoreProfile = qScopeGuard([previousProfile] {
            RuntimeProfile::configure(previousProfile);
        });
        RuntimeProfileContext profile;
        profile.rootPath = directory.filePath(QStringLiteral("viewer"));
        profile.persistent = false;
        QVERIFY(QDir().mkpath(profile.rootPath));
        QVERIFY(QFile::setPermissions(profile.rootPath,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        RuntimeProfile::configure(profile);
        SettingsManager savedSettings;
        QString error;
        QVERIFY2(savedSettings.setScreenContentVisible(false, &error), qPrintable(error));

        ApplicationRuntime runtime(profile);
        QSignalSpy toasts(runtime.getNotificationCenter(), &NotificationCenter::toastRequested);
        const auto screenToasts = [&] {
            int count = 0;
            for (const auto& toast : toasts) {
                const auto entry = qvariant_cast<NotificationEntry>(toast.first());
                if (entry.category == QLatin1String("Screen sharing")) {
                    if (entry.severity != NotificationSeverity::Warning) return -1;
                    ++count;
                }
            }
            return count;
        };
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        auto* settings = runtime.getSettingsManager();
        auto* owner = runtime.getWebSocketClient();
        auto* connection = runtime.findChild<ConnectionManager*>();
        QVERIFY(connection);
        QVERIFY(!settings->getScreenContentVisible());
        QVERIFY(!settings->getScreenSharingEnabled());
        QSignalSpy publishingChanges(settings, &SettingsManager::screenSharingEnabledChanged);
        QSignalSpy states(owner, &WebSocketClient::screenShareStateReceived);
        WebSocketClient target(directory.filePath(QStringLiteral("publisher")), false);
        configure(target, QStringLiteral("publisher"));
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        target.setScreenSharingEnabled(true);
        connection->connectToServer(m_url);
        target.connectToServer(m_url);
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 4000);
        runtime.activateClient(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(!owner->remoteSessionCoordinator()
            ->outgoingForPeer(target.endpointId()).remoteSessionId.isEmpty(), 4000);
        const QString session = owner->remoteSessionCoordinator()
                                    ->outgoingForPeer(target.endpointId()).remoteSessionId;
        QTRY_VERIFY_WITH_TIMEOUT(owner->canIssueSessionCommands(session)
                                && target.canIssueSessionCommands(session)
                                && target.isScreenChannelConnected(), 4000);
        auto* canvas = qobject_cast<QuickCanvasHost*>(runtime.getActiveCanvas());
        QVERIFY(canvas);
        auto* controller = canvas->controller();
        QVERIFY(controller);
        QTRY_COMPARE_WITH_TIMEOUT(controller->screensModel().size(), 1, 4000);
        auto* source = qobject_cast<RemoteVideoFrameSource*>(controller->screensModel()
            .first().toMap().value(QStringLiteral("frameSource")).value<QObject*>());
        QVERIFY(source);
        QTest::qWait(100);
        QVERIFY(grants.isEmpty());
        QVERIFY(!source->hasFrame());
        QVERIFY(!runtime.remoteScreenAvailable());
        QVERIFY(!runtime.remoteScreenLoading());

        QVERIFY2(settings->setScreenContentVisible(true, &error), qPrintable(error));
        QVERIFY(runtime.remoteScreenLoading());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);
        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        ScreenStreamEncoder encoder(false);
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        QVERIFY(target.sendScreenFrame(header(grant, packet, 1), packet.annexB));
        QTRY_VERIFY_WITH_TIMEOUT(source->hasFrame(), 4000);
        QCOMPARE(source->videoFrame().size(), image.size());
        QVERIFY(runtime.remoteScreenAvailable());
        QVERIFY(!runtime.remoteScreenLoading());
        QCOMPARE(screenToasts(), 0);
        QVERIFY(target.sendScreenShareStatus(session,
            quint64(grant.value("generation").toDouble()), QStringLiteral("permission_denied")));
        QTRY_COMPARE_WITH_TIMEOUT(screenToasts(), 1, 4000);
        QVERIFY(!source->hasFrame());
        QVERIFY(!runtime.remoteScreenAvailable());
        QVERIFY(!runtime.remoteScreenLoading());
        owner->screenShareStateReceived(states.last().first().toJsonObject());
        QCOMPARE(screenToasts(), 1);
        QVERIFY(target.sendScreenShareStatus(session,
            quint64(grant.value("generation").toDouble()), QStringLiteral("starting")));
        QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("reason")
                                == QLatin1String("starting"), 4000);
        QVERIFY(runtime.remoteScreenLoading());
        QCOMPARE(screenToasts(), 1); // Loading never generates a toast.
        QVERIFY(target.sendScreenFrame(header(grant, packet, 2), packet.annexB));
        QTRY_VERIFY_WITH_TIMEOUT(source->hasFrame(), 4000);
        QVERIFY(runtime.remoteScreenAvailable());
        QCOMPARE(screenToasts(), 1); // Recovery never generates a success toast.

        QVERIFY2(settings->setScreenContentVisible(false, &error), qPrintable(error));
        QVERIFY(!source->hasFrame());
        QVERIFY(!runtime.remoteScreenAvailable());
        QVERIFY(!runtime.remoteScreenLoading());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        QVERIFY(!target.sendScreenFrame(header(grant, packet, 2), packet.annexB));
        QVERIFY(owner->canIssueSessionCommands(session));
        QVERIFY(canvas->hasActiveScreens());

        const int grantsWhileHidden = grants.count();
        runtime.navigateToClients();
        runtime.activateClient(target.endpointId());
        QTRY_COMPARE_WITH_TIMEOUT(runtime.getActiveCanvas(), canvas, 4000);
        QTest::qWait(100);
        QVERIFY(!settings->getScreenContentVisible());
        QCOMPARE(grants.count(), grantsWhileHidden);
        QVERIFY(!source->hasFrame());
        QVERIFY(!runtime.remoteScreenAvailable());

        QVERIFY2(settings->setScreenContentVisible(true, &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(grants.count() > grantsWhileHidden
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto nextGrant = grants.last().first().toJsonObject();
        QVERIFY(nextGrant.value("streamId") != grant.value("streamId"));
        QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("streamId")
                                == nextGrant.value("streamId"), 4000);
        QVERIFY(target.sendScreenFrame(header(nextGrant, packet, 1), packet.annexB));
        QTRY_VERIFY_WITH_TIMEOUT(source->hasFrame(), 4000);
        QVERIFY(!settings->getScreenSharingEnabled());
        QVERIFY(publishingChanges.isEmpty());
        QVERIFY(runtime.remoteScreenAvailable());
        QCOMPARE(screenToasts(), 1);
        runtime.handleApplicationAboutToQuit();
        target.disconnect();
    }
    void captureFailureIsIsolatedToOneScreenAndRecoversIndependently() {
        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath(QStringLiteral("owner")), false);
        WebSocketClient target(identities.filePath(QStringLiteral("target")), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService viewer(&owner, &monitor);
        QSignalSpy frames(&viewer, &ScreenSharingService::frameReady);
        QSignalSpy screenCleared(&viewer, &ScreenSharingService::frameCleared);
        QSignalSpy allCleared(&viewer, &ScreenSharingService::framesCleared);
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        QSignalSpy states(&owner, &WebSocketClient::screenShareStateReceived);
        QString session;
        connectPeers(owner, target, &session,
            {ScreenInfo(0, 640, 360, 0, 0, true), ScreenInfo(1, 640, 360, 640, 0, false)});
        QVERIFY(!session.isEmpty());
        target.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(target.isScreenChannelConnected(), 4000);
        viewer.setViewedEndpoint(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = grants.last().first().toJsonObject();
        const auto generation = quint64(grant.value("generation").toDouble());
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);

        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::green);
        ScreenStreamEncoder encoder(false);
        QString error;
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        const auto metadata = [&](int screen, quint64 sequence) {
            auto value = header(grant, packet, sequence);
            value.insert(QStringLiteral("screenId"), screen);
            return value;
        };
        const auto frameCount = [&](int screen) {
            int count = 0;
            for (const auto& arguments : frames)
                if (arguments.at(1).toInt() == screen) ++count;
            return count;
        };
        QVERIFY(target.sendScreenFrame(metadata(0, 1), packet.annexB));
        QVERIFY(target.sendScreenFrame(metadata(1, 1), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 2, 4000);
        QCOMPARE(frameCount(0), 1);
        QCOMPARE(frameCount(1), 1);
        QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));
        const int wholeCanvasClears = allCleared.count();

        QVERIFY(target.sendScreenShareStatus(session, generation, QStringLiteral("capture_error"), 0));
        QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("reason") == QLatin1String("capture_error")
            && states.last().first().toJsonObject().value("screenId").toInt(-1) == 0, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(screenCleared.count(), 1, 4000);
        QCOMPARE(screenCleared.first().at(1).toInt(), 0);
        QCOMPARE(allCleared.count(), wholeCanvasClears);
        QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));
        QVERIFY(!viewer.isRemoteScreenLoading(target.endpointId()));

        // An obsolete frame can still be in either the video connection or
        // the decoder queue when the independent control failure arrives.
        QVERIFY(target.sendScreenFrame(metadata(0, 2), packet.annexB));
        owner.screenFrameReceived(metadata(0, 3), packet.annexB);
        QVERIFY(target.sendScreenFrame(metadata(1, 2), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frameCount(1), 2, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.findChildren<QFutureWatcherBase*>().isEmpty(), 4000);
        QCOMPARE(frameCount(0), 1);
        QCOMPARE(allCleared.count(), wholeCanvasClears);
        QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));

        QVERIFY(target.sendScreenShareStatus(session, generation, QStringLiteral("starting"), 0));
        QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("reason") == QLatin1String("starting")
            && states.last().first().toJsonObject().value("screenId").toInt(-1) == 0, 4000);
        QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));
        QVERIFY(target.sendScreenFrame(metadata(0, 3), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frameCount(0), 2, 4000);
        QCOMPARE(frameCount(1), 2);
        QCOMPARE(allCleared.count(), wholeCanvasClears);
        QVERIFY(owner.canIssueSessionCommands(session));
        QVERIFY(target.canIssueSessionCommands(session));
        viewer.stop();
        owner.disconnect();
        target.disconnect();
    }

    void staleScreenDoesNotClearAnotherScreenThatKeepsUpdating() {
        const AppConfig savedConfig = AppConfig::instance();
        const auto restoreConfig = qScopeGuard([savedConfig] { AppConfig::instance() = savedConfig; });
        AppConfig::LoadOptions options;
        options.arguments = {QStringLiteral("tst_ScreenSharingService")};
        options.defaultEnvFilePath = QString();
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_SCREEN_STALE_TIMEOUT_MS"), QStringLiteral("2000"));
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_SCREEN_IDLE_INTERVAL_MS"), QStringLiteral("500"));
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_SCREEN_FEEDBACK_INTERVAL_MS"), QStringLiteral("100"));
        QString error;
        QVERIFY2(AppConfig::instance().load(options, &error), qPrintable(error));

        QTemporaryDir identities;
        WebSocketClient owner(identities.filePath(QStringLiteral("owner")), false);
        WebSocketClient target(identities.filePath(QStringLiteral("target")), false);
        SystemMonitor monitor(nullptr, emptyDesktop());
        ScreenSharingService viewer(&owner, &monitor);
        QSignalSpy frames(&viewer, &ScreenSharingService::frameReady);
        QSignalSpy screenCleared(&viewer, &ScreenSharingService::frameCleared);
        QSignalSpy allCleared(&viewer, &ScreenSharingService::framesCleared);
        QSignalSpy grants(&target, &WebSocketClient::screenShareRequestReceived);
        QSignalSpy states(&owner, &WebSocketClient::screenShareStateReceived);
        QString session;
        connectPeers(owner, target, &session,
            {ScreenInfo(0, 640, 360, 0, 0, true), ScreenInfo(1, 640, 360, 640, 0, false)});
        QVERIFY(!session.isEmpty());
        target.setScreenSharingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(target.isScreenChannelConnected(), 4000);
        viewer.setViewedEndpoint(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);

        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        ScreenStreamEncoder encoder(false);
        const auto packets = encoder.encode(QVideoFrame(image), true, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(packets.size(), 1);
        const auto packet = packets.first();
        const auto metadata = [&](int screen, quint64 sequence) {
            auto value = header(grant, packet, sequence);
            value.insert(QStringLiteral("screenId"), screen);
            return value;
        };
        const auto frameCount = [&](int screen) {
            int count = 0;
            for (const auto& arguments : frames)
                if (arguments.at(1).toInt() == screen) ++count;
            return count;
        };
        QVERIFY(target.sendScreenFrame(metadata(0, 1), packet.annexB));
        QVERIFY(target.sendScreenFrame(metadata(1, 1), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frames.count(), 2, 4000);
        const int wholeCanvasClears = allCleared.count();
        quint64 sequence = 1;
        bool sendFailed = false;
        QTimer refreshHealthyScreen;
        connect(&refreshHealthyScreen, &QTimer::timeout, &target, [&] {
            sendFailed |= !target.sendScreenFrame(metadata(1, ++sequence), packet.annexB);
        });
        refreshHealthyScreen.start(200);
        QTRY_VERIFY_WITH_TIMEOUT(!screenCleared.isEmpty(), 4000);
        refreshHealthyScreen.stop();
        QVERIFY(!sendFailed);
        QCOMPARE(screenCleared.count(), 1);
        QCOMPARE(screenCleared.first().at(1).toInt(), 0);
        QCOMPARE(allCleared.count(), wholeCanvasClears);
        QCOMPARE(frameCount(0), 1);
        QVERIFY(frameCount(1) > 1);
        QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));

        QVERIFY(target.sendScreenFrame(metadata(0, 2), packet.annexB));
        QTRY_COMPARE_WITH_TIMEOUT(frameCount(0), 2, 4000);
        QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));
        QCOMPARE(allCleared.count(), wholeCanvasClears);
        viewer.stop();
        owner.disconnect();
        target.disconnect();
    }
};

QTEST_MAIN(ScreenSharingServiceTest)
#include "tst_ScreenSharingService.moc"
