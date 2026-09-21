#include "backend/screensharing/ScreenSharingService.h"
#include "backend/screensharing/ScreenStreamCodec.h"
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
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

// Exercise the production control/video relay and decoder without capturing
// the developer's desktop or asking the OS for screen recording permission.
class ScreenSharingServiceTest final : public QObject {
    Q_OBJECT
private:
    QProcess m_relay;
    QByteArray m_output;
    QString m_url;

    void configure(WebSocketClient& peer, const QString& name) {
        const auto advertise = [&peer, name] {
            peer.registerClient(name, QStringLiteral("screen-sharing-test"),
                                {ScreenInfo(0, 640, 360, 0, 0, true)}, 50);
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

    void connectPeers(WebSocketClient& owner, WebSocketClient& target, QString* session) {
        configure(owner, QStringLiteral("viewer"));
        configure(target, QStringLiteral("publisher"));
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

private slots:
    void init() {
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
        QVERIFY2(!node.isEmpty(), "Node.js is required for screen sharing integration tests");
        m_output.clear();
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
        viewer.setViewedEndpoint(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("reason") == QLatin1String("disabled"), 4000);
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
            const int oldFrames = frames.count();
            QVERIFY(target.sendScreenFrame(header(grant, packet, ++sequence), packet.annexB));
            QTRY_COMPARE_WITH_TIMEOUT(frames.count(), oldFrames + 1, 4000);
            QVERIFY(viewer.isRemoteScreenAvailable(target.endpointId()));
            const int oldIssues = issues.count();
            const int oldClears = cleared.count();
            QVERIFY(target.sendScreenShareStatus(session,
                quint64(grant.value("generation").toDouble()), reason));
            QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("reason") == reason, 4000);
            QTRY_VERIFY_WITH_TIMEOUT(cleared.count() > oldClears, 4000);
            QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
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
        viewer.setViewedEndpoint(target.endpointId());
        QTRY_VERIFY_WITH_TIMEOUT(!grants.isEmpty()
            && grants.last().first().toJsonObject().value("enabled").toBool(), 4000);
        const auto grant = grants.last().first().toJsonObject();
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty()
            && states.last().first().toJsonObject().value("streamId") == grant.value("streamId"), 4000);
        QVERIFY(issues.isEmpty());
        QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
        QTRY_COMPARE_WITH_TIMEOUT(issues.count(), 1, 12000);
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
        QCOMPARE(issues.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(issues.count(), 2, 6500);
        QVERIFY(!viewer.isRemoteScreenAvailable(target.endpointId()));
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
        // The relay deliberately debounces keyframe requests for 250 ms.
        QTest::qWait(300);
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

        QVERIFY2(settings->setScreenContentVisible(true, &error), qPrintable(error));
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
        QCOMPARE(screenToasts(), 0);
        QVERIFY(target.sendScreenShareStatus(session,
            quint64(grant.value("generation").toDouble()), QStringLiteral("permission_denied")));
        QTRY_COMPARE_WITH_TIMEOUT(screenToasts(), 1, 4000);
        QVERIFY(!source->hasFrame());
        QVERIFY(!runtime.remoteScreenAvailable());
        owner->screenShareStateReceived(states.last().first().toJsonObject());
        QCOMPARE(screenToasts(), 1);
        QVERIFY(target.sendScreenShareStatus(session,
            quint64(grant.value("generation").toDouble()), QStringLiteral("starting")));
        QTRY_VERIFY_WITH_TIMEOUT(states.last().first().toJsonObject().value("reason")
                                == QLatin1String("starting"), 4000);
        QVERIFY(target.sendScreenFrame(header(grant, packet, 2), packet.annexB));
        QTRY_VERIFY_WITH_TIMEOUT(source->hasFrame(), 4000);
        QVERIFY(runtime.remoteScreenAvailable());
        QCOMPARE(screenToasts(), 1); // Recovery never generates a success toast.

        QVERIFY2(settings->setScreenContentVisible(false, &error), qPrintable(error));
        QVERIFY(!source->hasFrame());
        QVERIFY(!runtime.remoteScreenAvailable());
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
};

QTEST_MAIN(ScreenSharingServiceTest)
#include "tst_ScreenSharingService.moc"
