#include "backend/network/UploadManager.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/media/ResidentVideoPlayer.h"
#include <QApplication>
#include <QAudioOutput>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QQuickView>
#include <QQuickItem>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QUuid>
#include <QWebSocketServer>
#include <QtTest>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/files/FileManager.h"
#include "backend/network/SceneRunCoordinator.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/WebSocketClient.h"
#include "backend/managers/network/ConnectionManager.h"
#include "backend/security/DeviceIdentityStore.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
#include "frontend/qml/TimelineController.h"
#include "frontend/rendering/remote/RemoteSceneController.h"

class RemoteSceneLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        MediaResidencyManager::instance().setMemorySnapshotForTesting(
            {8ULL << 30, 6ULL << 30, 128ULL << 20, false, 0});
    }
    void cleanupTestCase()
    {
        MediaResidencyManager::instance().clearMemorySnapshotForTesting();
    }
    void remoteTimelineLocksManualNavigation_data()
    {
        QTest::addColumn<QString>("phase");
        for (const char* phase : {"launching", "playing", "stopping"})
            QTest::newRow(phase) << QString::fromLatin1(phase);
    }
    void remoteTimelineLocksManualNavigation()
    {
        QFETCH(QString, phase);
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create()); QVERIFY(host);
        host->setProjectEditingEnabled(true);
        TimelineController timeline;
        timeline.setHost(host.get());
        timeline.seek(7000);
        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(1100, 240);
        view.setInitialProperties({{"session", QVariantMap{{"timeline", QVariant::fromValue<QObject*>(&timeline)}}}});
        view.setSource(QUrl("qrc:/qt/qml/Mouffette/App/resources/qml/app/canvas/TimelinePanel.qml"));
        QCOMPARE(view.status(), QQuickView::Ready);
        view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto* root = view.rootObject();
        auto* tracks = root->findChild<QQuickItem*>("timelineTracks"); QVERIFY(tracks);
        auto* clips = root->findChild<QQuickItem*>("timelineClipViewport"); QVERIFY(clips);
        auto* headers = root->findChild<QQuickItem*>("timelineTrackHeaders"); QVERIFY(headers);
        tracks->setProperty("contentX", 200.0);
        clips->setProperty("contentY", 0.0);
        const auto wheel = [&](QPoint point, QPoint pixels, QPoint angles, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QWheelEvent event(point, view.mapToGlobal(point), pixels, angles, Qt::NoButton,
                modifiers, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(&view, &event);
        };
        host->m_sceneLaunching = phase == "launching";
        host->m_sceneLaunched = phase == "playing";
        host->m_sceneStopping = phase == "stopping";
        host->publishActionState();
        QVERIFY(timeline.remoteActive());
        const auto duration = root->property("viewDurationMs");
        const auto position = timeline.positionMs();
        for (const auto point : {tracks->mapToScene({100, 10}).toPoint(),
                                tracks->mapToScene({100, 45}).toPoint(),
                                tracks->mapToScene({100, 90}).toPoint(),
                                headers->mapToScene({10, 90}).toPoint()}) {
            wheel(point, {}, {0, -120});
            wheel(point, {-40, -40}, {});
            wheel(point, {}, {0, -120}, Qt::ShiftModifier);
            wheel(point, {}, {0, -120}, Qt::ControlModifier);
        }
        for (const auto* name : {"timelineHorizontalScrollBar", "timelineVerticalScrollBar",
                                 "timelineZoomIn", "timelineZoomOut", "timelineFitDuration"}) {
            auto* control = root->findChild<QQuickItem*>(name); QVERIFY(control);
            QVERIFY(!control->isEnabled());
            const auto from = control->mapToScene({control->width()/2, control->height()/2}).toPoint();
            auto to = from;
            if (control->objectName() == "timelineHorizontalScrollBar") to.rx() -= 30;
            if (control->objectName() == "timelineVerticalScrollBar") to.ry() -= 15;
            QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, from);
            QTest::mouseMove(&view, to);
            QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, to);
        }
        QCOMPARE(tracks->property("contentX").toReal(), 200.0);
        QCOMPARE(clips->property("contentY").toReal(), 0.0);
        QCOMPARE(root->property("viewDurationMs"), duration);
        QCOMPARE(timeline.positionMs(), position);
        // Remote clock updates must still bring the playhead into view.
        host->document()->setTimelinePosition(90000);
        host->publishActionState();
        QVERIFY(tracks->property("contentX").toReal() > 200);
        host->m_sceneLaunching = host->m_sceneLaunched = host->m_sceneStopping = false;
        host->publishActionState();
        const auto previousX = tracks->property("contentX").toReal();
        wheel(tracks->mapToScene({100, 90}).toPoint(), {-40, -40}, {});
        QCOMPARE(tracks->property("contentX").toReal(), previousX + 40);
        QCOMPARE(clips->property("contentY").toReal(), 40.0);
        QVERIFY(root->findChild<QQuickItem*>("timelineHorizontalScrollBar")->isEnabled());
        QVERIFY(root->findChild<QQuickItem*>("timelineVerticalScrollBar")->isEnabled());
        QVERIFY(root->findChild<QQuickItem*>("timelineZoomIn")->isEnabled());
    }
    void transportTimeoutPreservesSessionUntilItsFixedRecoveryProofDeadline()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QWebSocketServer server(QStringLiteral("v6-recovery-boundary"), QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        qint64 continuousNow = 0;
        WebSocketClient client(directory.path(), false, nullptr, [&] { return continuousNow; });
        ConnectionManager connection(&client, nullptr, [&] { return continuousNow; });
        const QString boot = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString target(43, QLatin1Char('B'));
        QPointer<QWebSocket> peer;
        QJsonObject opened;
        int closeCommands = 0;
        auto send = [&](QJsonObject message) {
            message.insert("protocolVersion", WebSocketClient::ProtocolVersion);
            message.insert("serverBootId", boot);
            message.insert("messageId", QUuid::createUuid().toString(QUuid::WithoutBraces));
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            peer = server.nextPendingConnection();
            peer->setParent(&server);
            send({{"type", "auth_challenge"}, {"issuedAt", 1},
                {"nonce", QString::fromLatin1(QByteArray(32, 'n').toBase64(
                    QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}});
            connect(peer, &QWebSocket::textMessageReceived, this, [&](const QString& encoded) {
                const auto message = QJsonDocument::fromJson(encoded.toUtf8()).object();
                const QString type = message.value("type").toString();
                if (type == QLatin1String("remote_session_close")) ++closeCommands;
                if (type != QLatin1String("auth_response")) return;
                const QString owner = DeviceIdentityStore::endpointIdForInstallation(
                    message.value("installationId").toString(), message.value("instanceId").toString());
                send({{"type", "welcome"},
                    {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"installationId", message.value("installationId")}, {"endpointId", owner},
                    {"instanceId", message.value("instanceId")},
                    {"instanceOrdinal", message.value("instanceOrdinal")}, {"runtimeId", message.value("runtimeId")},
                    {"connectionGeneration", 1}, {"serverMonotonicMs", 0},
                    {"policy", QJsonObject{{"policyVersion", 5}, {"transportTimeoutMs", 5000}, {"heartbeatIntervalMs", 750},
                        {"leaseTimeoutMs", 1500}, {"transportSuspectAfterMs", 1500},
                        {"sessionRecoveryTimeoutMs", 15000}, {"scenePrepareTimeoutMs", 15000},
                        {"sceneActivationLeadMs", 500}, {"sceneMaxClockSkewMs", 50},
                        {"sceneStartedAckTimeoutMs", 5000}, {"sceneMaxStartSkewMs", 750},
                        {"sceneStopTimeoutMs", 5000}, {"uploadIdleTimeoutMs", 45000},
                        {"uploadTargetAckTimeoutMs", 30000}, {"removalAckTimeoutMs", 30000}}}});
                opened = {{"type", "remote_session_opened"}, {"remoteSessionId", "recovery-session"},
                    {"generation", 1}, {"commandReady", true}, {"stateRevision", 2}, {"validUntilServerMonotonicMs", 16500},
                    {"ownerConnectionGeneration", 1}, {"targetConnectionGeneration", 1},
                    {"ownerEndpointId", owner}, {"targetEndpointId", target},
                    {"resumeToken", "recovery-proof"}, {"phase", "Active"}, {"snapshotSequence", 1},
                    {"snapshot", QJsonObject{{"screens", QJsonArray{}}, {"systemUI", QJsonArray{}},
                        {"volumePercent", 50}, {"revision", 1}, {"capturedAtEpochMs", 1}}}};
                send(opened);
            });
        });
        QSignalSpy ready(&client, &WebSocketClient::remoteSessionOpened);
        QSignalSpy expired(&client, &WebSocketClient::remoteSessionRecoveryExpired);
        QSignalSpy globalExpired(&client, &WebSocketClient::leaseExpired);
        QSignalSpy invalidated(&client, &WebSocketClient::sessionsInvalidated);
        client.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 2000);
        QVERIFY(client.canIssueSessionCommands("recovery-session"));
        send(opened); // exact idempotent OPEN replay must not close/reinitialize.
        QTest::qWait(20);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(closeCommands, 0);
        continuousNow = 1500;
        QVERIFY(QMetaObject::invokeMethod(&client, "checkLeaseHealth", Qt::DirectConnection));
        QCOMPARE(connection.getConnectionStatus(), QStringLiteral("Degraded"));
        QVERIFY(client.isConnected()); // Two missed heartbeats degrade, not close.
        QCOMPARE(expired.size(), 0);
        QVERIFY(!client.remoteSessionCoordinator()->byId("recovery-session").remoteSessionId.isEmpty());
        QVERIFY(!client.canIssueSessionCommands("recovery-session"));
        continuousNow = 5000;
        QVERIFY(QMetaObject::invokeMethod(&client, "checkLeaseHealth", Qt::DirectConnection));
        QVERIFY(!client.isConnected()); // Independent transport timeout.
        QCOMPARE(connection.getConnectionStatus(), QStringLiteral("Degraded"));
        continuousNow = 16499;
        QVERIFY(QMetaObject::invokeMethod(&client, "checkLeaseHealth", Qt::DirectConnection));
        QCOMPARE(expired.size(), 0);
        QCOMPARE(connection.getConnectionStatus(), QStringLiteral("Degraded"));
        continuousNow = 16500;
        QVERIFY(QMetaObject::invokeMethod(&client, "checkLeaseHealth", Qt::DirectConnection));
        QCOMPARE(expired.size(), 1);
        QCOMPARE(expired.first().first().toString(), QStringLiteral("recovery-session"));
        QVERIFY(!client.remoteSessionCoordinator()->byId("recovery-session").active);
        QCOMPARE(connection.getConnectionStatus(), QStringLiteral("Disconnected"));
        QVERIFY(QMetaObject::invokeMethod(&client, "checkLeaseHealth", Qt::DirectConnection));
        QCOMPARE(expired.size(), 1);
        client.disconnect();
        QCOMPARE(invalidated.size(), 1);
        QVERIFY(globalExpired.size() <= 1);
    }

    void ownerAcknowledgementsRemainPendingUntilCommandsCanBeSent()
    {
        QTemporaryDir directory;
        WebSocketClient socket(directory.path(), false);
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setWebSocketClient(&socket);
        auto* coordinator = socket.sceneRunCoordinator();
        coordinator->setPrepareTimeoutMs(15000);
        QJsonObject session{{"type", "remote_session_opened"}, {"remoteSessionId", "pending-owner-session"},
            {"generation", 1}, {"ownerConnectionGeneration", 1}, {"targetConnectionGeneration", 1},
            {"ownerEndpointId", socket.endpointId()}, {"targetEndpointId", QString(43, QLatin1Char('B'))},
            {"resumeToken", "test-proof"}, {"phase", "Active"}};
        QVERIFY(coordinator->upsertSession(session));
        SceneRunCoordinator::Run run;
        QVERIFY(coordinator->createOutgoingRun(session.value("targetEndpointId").toString(), 1, {},
            QJsonObject{{"screens", QJsonArray{}}, {"media", QJsonArray{}}}, &run));
        host->m_sceneRunId = run.sceneRunId;
        host->m_sceneDigest = run.digest;
        host->m_sceneLaunching = true;
        host->m_sceneAccepted = true;
        host->m_localVideosPrepared = true;
        host->m_localPrepareChecklist = QJsonArray{QJsonObject{
            {"itemId", "screen"}, {"stage", "screen_render_graph_ready"}, {"ready", true}}};
        host->document()->setEditsLocked(true);
        host->reportLocalScenePrepared();
        QVERIFY(!host->m_localPreparedReported);
        QVERIFY(host->remoteSceneLaunching());
        QVERIFY(host->document()->editsLocked());
        host->m_firstFramePresentedServerMs = 1234;
        host->m_firstFramePresentedLocalMs = 5678;
        host->reportFirstFramePresented();
        QVERIFY(!host->m_firstFrameReported);
        QVERIFY(host->remoteSceneLaunching());
        QCOMPARE(host->m_firstFramePresentedServerMs, qint64(1234));
        emit socket.reconciliationCompleted();
        QVERIFY(host->remoteSceneLaunching());
        QCOMPARE(host->m_firstFramePresentedServerMs, qint64(1234));
        QCOMPARE(host->m_firstFramePresentedLocalMs, qint64(5678));
        emit socket.sceneErrorReceived({{"sceneRunId", host->m_sceneRunId},
            {"digest", host->m_sceneDigest}, {"code", "channel_unavailable"},
            {"errorClass", "temporary"}});
        QVERIFY(host->remoteSceneLaunching());
        QVERIFY(host->document()->editsLocked());
        session.insert("type", "remote_session_resumed");
        session.insert("generation", 2);
        session.insert("ownerConnectionGeneration", 2);
        session.insert("targetConnectionGeneration", 2);
        QVERIFY(coordinator->upsertSession(session));
        emit socket.sceneErrorReceived({{"sceneRunId", run.sceneRunId}, {"digest", run.digest},
            {"remoteSessionId", run.remoteSessionId}, {"generation", 1},
            {"code", "scene_prepare_failed"}, {"message", "Delayed terminal error from old generation"}});
        QVERIFY(host->remoteSceneLaunching());
        QVERIFY(host->document()->editsLocked());
        host->handleRemoteConnectionLost();
        QVERIFY(!host->remoteSceneLaunching());
        QVERIFY(!host->document()->editsLocked());
    }

    void pendingMetadataImportBlocksReadyCanvasWithoutAutoLaunching()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("pending.png"));
        QImage image(32, 24, QImage::Format_RGBA8888);
        image.fill(Qt::cyan);
        QVERIFY(image.save(path));
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        QVERIFY(host->document()->addText(QPointF(30, 40), QStringLiteral("Ready text")));
        QVERIFY(host->testSceneActionEnabled());
        const QString mediaId = host->document()->queueFileImport(path, QPointF(100, 120));
        QVERIFY(!mediaId.isEmpty());
        QVERIFY(host->document()->hasPendingImports());
        QCOMPARE(host->document()->media().size(), 1);
        QVERIFY(!host->testSceneActionEnabled());
        QVERIFY(host->mediaReadinessReason(false).contains(QStringLiteral("analyzed")));
        QVERIFY(host->mediaReadinessReason(true).contains(QStringLiteral("analyzed")));
        host->triggerTestSceneAction();
        QVERIFY(!host->testSceneLaunched());
        QTRY_VERIFY_WITH_TIMEOUT(!host->document()->hasPendingImports(), 5000);
        QVERIFY(host->document()->mediaById(mediaId));
        QTRY_VERIFY_WITH_TIMEOUT(host->testSceneActionEnabled(), 5000);
        QVERIFY(!host->testSceneLaunched());
        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->testSceneActionEnabled()); // Stop remains available.
        host->triggerTestSceneAction();
        QVERIFY(!host->testSceneLaunched());
    }

    void localSceneStopsAfterPersistentMemoryPressure()
    {
        auto& manager = MediaResidencyManager::instance();
        const MediaResidencyManager::MemorySnapshot healthy{
            8ULL << 30, 6ULL << 30, 128ULL << 20, false, 0};
        manager.setMemorySnapshotForTesting(healthy);
        const auto restoreMemory = qScopeGuard([&manager, healthy] {
            manager.setMemorySnapshotForTesting(healthy);
        });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("pressure.png"));
        QImage image(48, 40, QImage::Format_RGBA8888);
        image.fill(QColor(12, 34, 56));
        QVERIFY(image.save(path));
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        CanvasMedia* media = host->document()->addPreparedFile(
            path, image.size(), false, QPointF(70, 80));
        QVERIFY(media);
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
        const QString owner = media->residencyOwnerId();
        QVERIFY(owner != media->mediaId());
        const QString digest = manager.sha256(owner);
        const auto assetRow = [&manager, digest] {
            for (const auto& row : manager.assets()) {
                const auto fields = row.toMap();
                if (fields.value(QStringLiteral("assetId")).toString() == digest) return fields;
            }
            return QVariantMap();
        };
        std::weak_ptr<const ResidentMediaAsset> originalAllocation = manager.asset(owner);
        QVERIFY(!originalAllocation.expired());
        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->document()->editsLocked());
        QVERIFY(assetRow().value(QStringLiteral("protected")).toBool());
        QSignalSpy pressureStop(&manager, &MediaResidencyManager::sceneStopRequested);
        QElapsedTimer pressureDuration;
        pressureDuration.start();
        const quint64 reserve = manager.summary().value(QStringLiteral("reserveBytes")).toULongLong();
        QVERIFY(reserve > 0);
        manager.setMemorySnapshotForTesting(
            {8ULL << 30, reserve - 1, 128ULL << 20, false, 2});
        // Critical OS pressure triggers coordinated stop after hysteresis;
        // a reserve deficit alone preserves already prepared scenes.
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->testSceneActionEnabled()); // Stop is still available.
        QVERIFY(media->residencyReady());
        QCOMPARE(pressureStop.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!host->testSceneLaunched(), 4500);
        QVERIFY(pressureDuration.elapsed() >= 1900);
        QCOMPARE(pressureStop.size(), 1);
        QVERIFY(pressureStop.first().at(0).toString().startsWith(QStringLiteral("canvas-preview:")));
        QTRY_COMPARE_WITH_TIMEOUT(media->residencyState(), QStringLiteral("waiting_for_memory"), 1500);
        QVERIFY(!media->residencyError().isEmpty());
        QCOMPARE(manager.summary().value("pressure").toString(), QStringLiteral("critical"));
        QVERIFY(!host->document()->editsLocked());
        QVERIFY(!assetRow().value(QStringLiteral("protected")).toBool());
        QCOMPARE(assetRow().value(QStringLiteral("residentBytes")).toULongLong(), quint64(0));
        QVERIFY(!manager.asset(owner));
        QVERIFY(originalAllocation.expired());
        QVERIFY(!host->testSceneActionEnabled());
        manager.setMemorySnapshotForTesting(healthy);
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
        QVERIFY(host->testSceneActionEnabled());
        QVERIFY(!host->testSceneLaunched());
        QVERIFY(!host->document()->editsLocked());
        QCOMPARE(pressureStop.size(), 1);
    }

    void remoteLaunchPreparesLocalMedia_data()
    {
        QTest::addColumn<bool>("includeImage");
        QTest::addColumn<bool>("missingSource");
        QTest::addColumn<bool>("uploaded");
        QTest::addColumn<bool>("expectLaunch");
        QTest::addColumn<bool>("serverRejectsInventory");
        QTest::addColumn<bool>("memoryReady");
        QTest::newRow("text-only")
            << false << false << false << true << false << true;
        QTest::newRow("text-and-uploaded-image")
            << true << false << true << true << false << true;
        QTest::newRow("text-and-unuploaded-image")
            << true << false << false << false << false << false;
        QTest::newRow("missing-local-image-source")
            << true << true << true << false << false << true;
        QTest::newRow("stale-upload-marker")
            << true << false << true << false << true << true;
        QTest::newRow("uploaded-awaiting-target-memory")
            << true << false << true << false << false << false;
    }

    void remoteLaunchPreparesLocalMedia()
    {
        QFETCH(bool, includeImage);
        QFETCH(bool, missingSource);
        QFETCH(bool, uploaded);
        QFETCH(bool, expectLaunch);
        QFETCH(bool, serverRejectsInventory);
        QFETCH(bool, memoryReady);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QWebSocketServer server(QStringLiteral("scene-preparation-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString targetId(43, QLatin1Char('B'));
        QString ownerId;
        QPointer<QWebSocket> serverPeer;
        QJsonObject scenePrepare;
        QJsonObject stateSnapshot;
        QJsonObject prepared;
        QJsonObject progress;
        int preparedCount = 0;
        int progressCount = 0;
        int stopCount = 0;
        QJsonObject fixtureSessionState;
        auto send = [&](QWebSocket* peer, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), WebSocketClient::ProtocolVersion);
            if (message.contains(QStringLiteral("remoteSessionId"))) {
                message.insert(QStringLiteral("stateRevision"), 1);
                message.insert(QStringLiteral("commandReady"), true);
                message.insert(QStringLiteral("validUntilServerMonotonicMs"), 20000);
                if (message.value(QStringLiteral("phase")).toString() == QLatin1String("Active"))
                    fixtureSessionState = message;
            }
            if (message.value(QStringLiteral("type")).toString() == QLatin1String("heartbeat_ack")
                && !fixtureSessionState.isEmpty()) {
                auto proof = fixtureSessionState;
                proof.insert(QStringLiteral("validUntilServerMonotonicMs"),
                    message.value(QStringLiteral("serverMonotonicMs")).toDouble() + 20000);
                message.insert(QStringLiteral("sessionStates"), QJsonArray{proof});
            }
            message.insert(QStringLiteral("serverBootId"), bootId);
            message.insert(QStringLiteral("messageId"),
                           QUuid::createUuid().toString(QUuid::WithoutBraces));
            peer->sendTextMessage(QString::fromUtf8(
                QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };
        connect(&server, &QWebSocketServer::newConnection, this, [&]() {
            QWebSocket* peer = server.nextPendingConnection();
            QVERIFY(peer);
            serverPeer = peer;
            peer->setParent(&server);
            send(peer, {{"type", "auth_challenge"}, {"issuedAt", 1},
                        {"nonce", QString::fromLatin1(QByteArray(32, 'n').toBase64(
                            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}});
            connect(peer, &QWebSocket::textMessageReceived, &server,
                    [&, peer](const QString& encoded) {
                const QJsonObject message = QJsonDocument::fromJson(encoded.toUtf8()).object();
                const QString type = message.value("type").toString();
                if (type == QLatin1String("auth_response")) {
                    ownerId = DeviceIdentityStore::endpointIdForInstallation(
                        message.value("installationId").toString(),
                        message.value("instanceId").toString());
                    send(peer, {
                        {"type", "welcome"},
                        {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {"installationId", message.value("installationId")},
                        {"endpointId", ownerId},
                        {"instanceId", message.value("instanceId")},
                        {"instanceOrdinal", message.value("instanceOrdinal")},
                        {"runtimeId", message.value("runtimeId")},
                        {"connectionGeneration", 1}, {"serverMonotonicMs", 10},
                        {"policy", QJsonObject{
                            {"policyVersion", 5}, {"transportTimeoutMs", 5000}, {"heartbeatIntervalMs", 250},
                            {"transportSuspectAfterMs", 500}, {"sessionRecoveryTimeoutMs", 20000},
                            {"leaseTimeoutMs", 500}, {"scenePrepareTimeoutMs", 1000},
                            {"sceneActivationLeadMs", 500}, {"sceneMaxClockSkewMs", 50},
                            {"sceneStartedAckTimeoutMs", 5000}, {"sceneMaxStartSkewMs", 750},
                            {"sceneStopTimeoutMs", 5000},
                            {"uploadIdleTimeoutMs", 45000}, {"uploadTargetAckTimeoutMs", 30000},
                            {"removalAckTimeoutMs", 30000}}}
                    });
                    send(peer, {
                        {"type", "remote_session_opened"},
                        {"remoteSessionId", "preparation-session"}, {"generation", 1},
                        {"ownerEndpointId", ownerId}, {"targetEndpointId", targetId},
                        {"ownerConnectionGeneration", 1}, {"targetConnectionGeneration", 1},
                        {"resumeToken", "test-resume-token"}, {"phase", "Active"},
                        {"snapshotSequence", 1},
                        {"snapshot", QJsonObject{
                            {"screens", QJsonArray{}}, {"systemUI", QJsonArray{}},
                            {"volumePercent", QJsonValue::Null}, {"revision", 1},
                            {"capturedAtEpochMs", 1}}}
                    });
                } else if (type == QLatin1String("heartbeat")) {
                    send(peer, {{"type", "heartbeat_ack"}, {"connectionGeneration", 1},
                                {"sequence", message.value("sequence")},
                                {"clientMonotonicMs", message.value("clientMonotonicMs")},
                                {"serverMonotonicMs", message.value("clientMonotonicMs")},
                                {"serverEpochMs", 1}});
                } else if (type == QLatin1String("scene_prepare")) {
                    scenePrepare = message;
                    // The owner must not report readiness before the server's
                    // authoritative acceptance barrier.
                    QCOMPARE(progressCount, 0);
                    QCOMPARE(preparedCount, 0);
                    if (serverRejectsInventory) {
                        send(peer, {
                            {"type", "error"},
                            {"scope", "scene"},
                            {"code", "scene_asset_not_validated"},
                            {"message", "At least one scene media file has not been uploaded"},
                            {"remoteSessionId", message.value("remoteSessionId")},
                            {"generation", message.value("generation")},
                            {"sceneRunId", message.value("sceneRunId")},
                            {"revision", message.value("revision")},
                            {"digest", message.value("digest")},
                            {"ownerEndpointId", ownerId},
                            {"targetEndpointId", targetId}
                        });
                        return;
                    }
                    const QJsonObject accepted{
                        {"type", "prepare_progress"},
                        {"remoteSessionId", message.value("remoteSessionId")},
                        {"generation", message.value("generation")},
                        {"sceneRunId", message.value("sceneRunId")},
                        {"revision", message.value("revision")},
                        {"digest", message.value("digest")},
                        {"ownerEndpointId", ownerId},
                        {"targetEndpointId", targetId},
                        {"aggregate", true},
                        {"percent", 0},
                        {"stage", "accepted"}
                    };
                    send(peer, accepted);
                    send(peer, accepted); // duplicate/replay must stay idempotent
                } else if (type == QLatin1String("prepared")) {
                    prepared = message;
                    ++preparedCount;
                } else if (type == QLatin1String("prepare_progress")) {
                    progress = message;
                    ++progressCount;
                } else if (type == QLatin1String("state_snapshot")) {
                    stateSnapshot = message.value("snapshot").toObject();
                } else if (type == QLatin1String("stop")) {
                    ++stopCount;
                }
            });
        });

        WebSocketClient client(directory.path(), false);
        QSignalSpy sessionOpened(&client, &WebSocketClient::remoteSessionOpened);
        client.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(sessionOpened.count(), 1, 3000);

        FileManager files;
        UploadManager uploads(&files);
        uploads.setWebSocketClient(&client);
        uploads.setTargetClientId(targetId);
        // RemoteFileTracker is process-global; keep data rows isolated even
        // when two fixtures have identical content-addressed file IDs.
        files.unmarkAllForClient(targetId);
        QQuickWindow window;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->controller()->registerWindow(&window);
        host->setFileManager(&files);
        host->setWebSocketClient(&client);
        host->setUploadManager(&uploads);
        host->setRemoteSceneTarget(targetId, QStringLiteral("Client B"));
        host->setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        host->setProjectEditingEnabled(true);
        host->setOverlayActionsEnabled(true);
        ClientWorkspaceViewModel workspace(targetId, host.get(),
            [] {}, &uploads, [] { return true; }, [] { return false; }, [] { return true; });
        auto* listModel = qobject_cast<QAbstractItemModel*>(workspace.mediaModel());
        QVERIFY(listModel);
        const auto rowCached = [listModel](const QString& sourceId) {
            for (int row = 0; row < listModel->rowCount(); ++row) {
                const auto value = listModel->data(listModel->index(row, 0), MediaListModel::ModelDataRole).toMap();
                if (value.value(QStringLiteral("sourceId")).toString() == sourceId)
                    return value.value(QStringLiteral("remoteCached")).toBool();
            }
            return false;
        };
        QVERIFY(host->document()->addText(QPointF(40, 60), QStringLiteral("Scene title")));
        if (includeImage) {
            const QString path = directory.filePath(QStringLiteral("asset.png"));
            QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::blue);
            QVERIFY(image.save(path));
            CanvasMedia* media = host->document()->addPreparedFile(
                path, image.size(), false, QPointF(4, 5));
            QVERIFY(media);
            QVERIFY(!host->remoteSceneActionEnabled());
            QVERIFY(!host->testSceneActionEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
            if (uploaded) {
                files.markFileUploadedToClient(media->fileId(), targetId);
                media->setUploadUploaded();
                QVERIFY(!rowCached(media->fileId())); // Upload alone is not a cache acknowledgement.
                if (memoryReady) {
                    const auto report = [&](const QString& state, int sequence) {
                        send(serverPeer, {{"type", "media_residency"},
                        {"remoteSessionId", "preparation-session"}, {"generation", 1},
                        {"ownerEndpointId", ownerId}, {"targetEndpointId", targetId},
                        {"sequence", sequence}, {"assets", QJsonArray{QJsonObject{
                            {"assetId", media->fileId()}, {"sha256", media->fileId()},
                            {"state", state}, {"progress", state == "ready" ? 1 : 0}, {"error", ""}}}}});
                    };
                    QSignalSpy rowChanges(listModel, &QAbstractItemModel::dataChanged);
                    report(QStringLiteral("ready"), 1);
                    QTRY_VERIFY_WITH_TIMEOUT(uploads.remoteMediaReady(targetId, media->fileId()), 2000);
                    QTRY_VERIFY(rowCached(media->fileId()));
                    QCOMPARE(listModel->rowCount(),1); // Text never enters the source catalogue.
                    QVERIFY(!rowChanges.isEmpty());
                    rowChanges.clear();
                    report(QStringLiteral("waiting_for_memory"), 2);
                    QTRY_VERIFY(!rowCached(media->fileId()));
                    QVERIFY(!rowChanges.isEmpty());
                    report(QStringLiteral("ready"), 3);
                    QTRY_VERIFY(rowCached(media->fileId()));
                    // Source readiness belongs to a target endpoint. The same
                    // source in another workspace has no remote cache evidence.
                    ClientWorkspaceViewModel otherWorkspace(QStringLiteral("another-peer"), host.get(),
                        [] {}, &uploads, [] { return true; }, [] { return false; }, [] { return true; });
                    auto* otherList = qobject_cast<QAbstractItemModel*>(otherWorkspace.mediaModel());
                    QVERIFY(otherList);
                    QCOMPARE(otherList->rowCount(),1);
                    const auto otherRow = otherList->data(otherList->index(0,0), MediaListModel::ModelDataRole).toMap();
                    QCOMPARE(otherRow.value("sourceId").toString(),media->fileId());
                    QCOMPARE(otherRow.value("uploadState").toString(),QStringLiteral("not_uploaded"));
                    QVERIFY(!otherRow.value("remoteCached").toBool());
                    host->setRemoteSceneTarget(QStringLiteral("another-peer"), {});
                    QVERIFY(!host->remoteMediaCached(media->mediaId()));
                    QVERIFY(rowCached(media->fileId())); // Its owning workspace is unchanged.
                    host->setRemoteSceneTarget(targetId, QStringLiteral("Client B"));
                    QVERIFY(host->remoteMediaCached(media->mediaId()));
                    QVERIFY(rowCached(media->fileId()));
                }
            }
            if (missingSource) {
                // The uploaded asset is valid, but the canvas runtime lost its source.
                media->setSourcePath(directory.filePath(QStringLiteral("missing.png")));
            }
        }
        QCOMPARE(host->remoteSceneActionEnabled(), !includeImage || (uploaded && memoryReady && !missingSource));
        host->triggerRemoteSceneAction();

        if (serverRejectsInventory) {
            QTRY_VERIFY_WITH_TIMEOUT(!scenePrepare.isEmpty(), 3000);
            QTRY_VERIFY_WITH_TIMEOUT(!host->remoteSceneLaunching(), 1000);
            // Let the former handshake timeout window elapse. A correlated
            // authoritative rejection must remain terminal and quiet.
            QTest::qWait(1200);
            QVERIFY(progress.isEmpty());
            QVERIFY(prepared.isEmpty());
            QCOMPARE(progressCount, 0);
            QCOMPARE(preparedCount, 0);
            QCOMPARE(stopCount, 0);
            QVERIFY(!host->document()->editsLocked());
            host->handleRemoteConnectionLost();
            client.disconnect();
            files.unmarkAllForClient(targetId);
            return;
        }

        if (!expectLaunch) {
            QTRY_VERIFY_WITH_TIMEOUT(!host->remoteSceneLaunching(), 500);
            QTest::qWait(100);
            QVERIFY(scenePrepare.isEmpty());
            QVERIFY(progress.isEmpty());
            QVERIFY(prepared.isEmpty());
            QCOMPARE(progressCount, 0);
            QCOMPARE(preparedCount, 0);
            QCOMPARE(stopCount, 0);
            QVERIFY(!host->document()->editsLocked());
            host->handleRemoteConnectionLost();
            client.disconnect();
            files.unmarkAllForClient(targetId);
            return;
        }

        QTRY_VERIFY_WITH_TIMEOUT(!scenePrepare.isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!prepared.isEmpty(), 3000);
        QVERIFY(prepared.value("success").toBool());
        QVERIFY(host->remoteSceneLaunching());
        QVERIFY(host->document()->editsLocked());

        const QJsonArray checklist = prepared.value("checklist").toArray();
        QCOMPARE(checklist.size(), includeImage ? 7 : 3);
        int readyCount = 0;
        for (const QJsonValue& value : checklist) {
            const QJsonObject item = value.toObject();
            // Protocol v4 rejects any extra keys, including mediaId.
            QCOMPARE(item.keys(), (QStringList{"itemId", "ready", "stage"}));
            if (item.value("ready").toBool()) ++readyCount;
        }
        QVERIFY(checklist.first().toObject().value("ready").toBool());
        QTRY_VERIFY(!progress.isEmpty());
        QCOMPARE(progress.value("checklist").toArray(), checklist);
        QCOMPARE(progress.value("percent").toInt(), readyCount * 100 / checklist.size());
        QCOMPARE(progressCount, 1);
        QCOMPARE(preparedCount, 1);
        QCOMPARE(readyCount, checklist.size());
        QCOMPARE(stopCount, 0);

        // Device snapshots can arrive during PREPARE or playback. The accepted
        // immutable scene retains its geometry; periodic snapshots carry only
        // the common timeline position.
        const auto frozenScene = host->document()->serializeSceneState();
        const auto mediaRect = host->document()->media().first()->sceneRect();
        host->setScreens({});
        QVERIFY(host->document()->screens().isEmpty());
        QCOMPARE(host->document()->media().first()->sceneRect(), mediaRect);
        client.sceneStartedReceived({{"sceneRunId", scenePrepare.value("sceneRunId")},
                                     {"digest", scenePrepare.value("digest")},
                                     {"allStarted", true}});
        QVERIFY(host->remoteSceneLaunched());
        const ScreenInfo replacement(0, 1280, 720, -1280, -720, true);
        host->setScreens({replacement});
        QCOMPARE(host->document()->screens(), QList<ScreenInfo>{replacement});
        QCOMPARE(host->document()->media().first()->sceneRect(), mediaRect);
        QTRY_VERIFY_WITH_TIMEOUT(!stateSnapshot.isEmpty(), 2500);
        QCOMPARE(stateSnapshot.keys(),QStringList{QStringLiteral("timelinePositionMs")});
        QVERIFY(stateSnapshot.value("timelinePositionMs").toDouble(-1)>=0);
        const auto transmittedScene = scenePrepare.value("scene").toObject();
        QCOMPARE(transmittedScene.value("screens"), frozenScene.value("screens"));
        QCOMPARE(transmittedScene.value("media").toArray().first().toObject().value("spans"),
                 frozenScene.value("media").toArray().first().toObject().value("spans"));
        host->handleRemoteConnectionLost();
        QCOMPARE(host->document()->serializeSceneState().value("screens").toArray(),
                 QJsonArray{replacement.toJson()});
        client.disconnect();
        files.unmarkAllForClient(targetId);
    }

    void localTestAndRemotePrepareAreMutuallyExclusive()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QWebSocketServer server(QStringLiteral("scene-exclusion-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString targetId(43, QLatin1Char('B'));
        int scenePrepareCount = 0;
        QJsonObject firstPrepare;
        QJsonObject fixtureSessionState;
        auto send = [&](QWebSocket* peer, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), WebSocketClient::ProtocolVersion);
            if (message.contains(QStringLiteral("remoteSessionId"))) {
                message.insert(QStringLiteral("stateRevision"), 1);
                message.insert(QStringLiteral("commandReady"), true);
                message.insert(QStringLiteral("validUntilServerMonotonicMs"), 20000);
                if (message.value(QStringLiteral("phase")).toString() == QLatin1String("Active"))
                    fixtureSessionState = message;
            }
            if (message.value(QStringLiteral("type")).toString() == QLatin1String("heartbeat_ack")
                && !fixtureSessionState.isEmpty()) {
                auto proof = fixtureSessionState;
                proof.insert(QStringLiteral("validUntilServerMonotonicMs"),
                    message.value(QStringLiteral("serverMonotonicMs")).toDouble() + 20000);
                message.insert(QStringLiteral("sessionStates"), QJsonArray{proof});
            }
            message.insert(QStringLiteral("serverBootId"), bootId);
            message.insert(QStringLiteral("messageId"),
                           QUuid::createUuid().toString(QUuid::WithoutBraces));
            peer->sendTextMessage(QString::fromUtf8(
                QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };
        connect(&server, &QWebSocketServer::newConnection, this, [&]() {
            QWebSocket* peer = server.nextPendingConnection();
            QVERIFY(peer);
            peer->setParent(&server);
            send(peer, {{"type", "auth_challenge"}, {"issuedAt", 1},
                        {"nonce", QString::fromLatin1(QByteArray(32, 'n').toBase64(
                            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}});
            connect(peer, &QWebSocket::textMessageReceived, &server,
                    [&, peer](const QString& encoded) {
                const QJsonObject message = QJsonDocument::fromJson(encoded.toUtf8()).object();
                const QString type = message.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("auth_response")) {
                    const QString ownerId = DeviceIdentityStore::endpointIdForInstallation(
                        message.value(QStringLiteral("installationId")).toString(),
                        message.value(QStringLiteral("instanceId")).toString());
                    send(peer, {
                        {"type", "welcome"},
                        {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {"installationId", message.value("installationId")},
                        {"endpointId", ownerId},
                        {"instanceId", message.value("instanceId")},
                        {"instanceOrdinal", message.value("instanceOrdinal")},
                        {"runtimeId", message.value("runtimeId")},
                        {"connectionGeneration", 1}, {"serverMonotonicMs", 10},
                        {"policy", QJsonObject{
                            {"policyVersion", 5}, {"transportTimeoutMs", 5000}, {"heartbeatIntervalMs", 250},
                            {"transportSuspectAfterMs", 500}, {"sessionRecoveryTimeoutMs", 20000},
                            {"leaseTimeoutMs", 500}, {"scenePrepareTimeoutMs", 5000},
                            {"sceneActivationLeadMs", 500}, {"sceneMaxClockSkewMs", 50},
                            {"sceneStartedAckTimeoutMs", 5000}, {"sceneMaxStartSkewMs", 750},
                            {"sceneStopTimeoutMs", 5000},
                            {"uploadIdleTimeoutMs", 45000}, {"uploadTargetAckTimeoutMs", 30000},
                            {"removalAckTimeoutMs", 30000}}}
                    });
                    send(peer, {
                        {"type", "remote_session_opened"},
                        {"remoteSessionId", "scene-exclusion-session"}, {"generation", 1},
                        {"ownerEndpointId", ownerId}, {"targetEndpointId", targetId},
                        {"ownerConnectionGeneration", 1}, {"targetConnectionGeneration", 1},
                        {"resumeToken", "scene-exclusion-token"}, {"phase", "Active"},
                        {"snapshotSequence", 1},
                        {"snapshot", QJsonObject{
                            {"screens", QJsonArray{}}, {"systemUI", QJsonArray{}},
                            {"volumePercent", QJsonValue::Null}, {"revision", 1},
                            {"capturedAtEpochMs", 1}}}
                    });
                } else if (type == QLatin1String("heartbeat")) {
                    send(peer, {{"type", "heartbeat_ack"}, {"connectionGeneration", 1},
                                {"sequence", message.value("sequence")},
                                {"clientMonotonicMs", message.value("clientMonotonicMs")},
                                {"serverMonotonicMs", message.value("clientMonotonicMs")},
                                {"serverEpochMs", 1}});
                } else if (type == QLatin1String("scene_prepare")) {
                    ++scenePrepareCount;
                    if (firstPrepare.isEmpty()) firstPrepare = message;
                    for (const QString& field : {QStringLiteral("remoteSessionId"),
                            QStringLiteral("generation"), QStringLiteral("sceneRunId"),
                            QStringLiteral("revision"), QStringLiteral("digest"),
                            QStringLiteral("manifest"), QStringLiteral("scene")}) {
                        QCOMPARE(message.value(field), firstPrepare.value(field));
                    }
                    // Deliberately withhold acceptance: retries are expected,
                    // but every retry must refer to this same immutable run.
                }
            });
        });

        WebSocketClient client(directory.path(), false);
        QSignalSpy sessionOpened(&client, &WebSocketClient::remoteSessionOpened);
        client.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(sessionOpened.count(), 1, 3000);

        FileManager files;
        QQuickWindow window;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->controller()->registerWindow(&window);
        host->setFileManager(&files);
        host->setWebSocketClient(&client);
        host->setRemoteSceneTarget(targetId, QStringLiteral("Client B"));
        host->setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        host->setProjectEditingEnabled(true);
        host->setOverlayActionsEnabled(true);
        QVERIFY(host->document()->addText(QPointF(40, 60), QStringLiteral("Scene title")));
        QVERIFY(host->remoteSceneActionEnabled());
        QVERIFY(host->testSceneActionEnabled());

        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->document()->editsLocked());
        QVERIFY(host->testSceneActionEnabled()); // the local stop action remains available
        QVERIFY(!host->remoteSceneActionEnabled());
        host->triggerRemoteSceneAction();
        QTest::qWait(100);
        QCOMPARE(scenePrepareCount, 0);
        QVERIFY(host->testSceneLaunched());
        QVERIFY(!host->remoteSceneLaunching());
        QVERIFY(!host->remoteSceneLaunched());
        QVERIFY(!host->remoteSceneStopping());
        QVERIFY(host->document()->editsLocked());

        host->triggerTestSceneAction();
        QVERIFY(!host->testSceneLaunched());
        QVERIFY(!host->document()->editsLocked());
        QVERIFY(host->remoteSceneActionEnabled());
        QVERIFY(host->testSceneActionEnabled());

        host->triggerRemoteSceneAction();
        QTRY_VERIFY_WITH_TIMEOUT(scenePrepareCount >= 1, 3000);
        const QString preparedRunId = firstPrepare.value(QStringLiteral("sceneRunId")).toString();
        QVERIFY(!preparedRunId.isEmpty());
        QCOMPARE(host->m_sceneRunId, preparedRunId);
        QVERIFY(host->remoteSceneLaunching());
        QVERIFY(!host->remoteSceneLaunched());
        QVERIFY(!host->remoteSceneStopping());
        QVERIFY(host->document()->editsLocked());
        QVERIFY(!host->testSceneActionEnabled());
        host->triggerTestSceneAction();
        QTest::qWait(100);
        QVERIFY(!host->testSceneLaunched());
        QVERIFY(host->remoteSceneLaunching());
        QVERIFY(!host->remoteSceneLaunched());
        QVERIFY(!host->remoteSceneStopping());
        QVERIFY(host->document()->editsLocked());
        QCOMPARE(host->m_sceneRunId, preparedRunId);
        QTRY_VERIFY_WITH_TIMEOUT(scenePrepareCount >= 2, 3000);
        QCOMPARE(host->m_sceneRunId, preparedRunId);
        QVERIFY(host->remoteSceneLaunching());
        QVERIFY(!host->testSceneLaunched());

        host->handleRemoteConnectionLost();
        QVERIFY(!host->remoteSceneLaunching());
        QVERIFY(!host->document()->editsLocked());
        client.disconnect();
    }

    void ownerWaitsForAClockSampleAfterAllPrepared()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QWebSocketServer server(QStringLiteral("scene-clock-retry-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString targetId(43, QLatin1Char('B'));
        QString ownerId;
        bool allPreparedSent = false;
        int preparedAttempts = 0;
        bool armedBeforeClockReply = false;
        int clockRepliesAfterBarrier = 0;
        int armedCount = 0;
        int stopCount = 0;
        QJsonObject fixtureSessionState;
        auto send = [&](QWebSocket* peer, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), WebSocketClient::ProtocolVersion);
            if (message.contains(QStringLiteral("remoteSessionId"))) {
                message.insert(QStringLiteral("stateRevision"), 1);
                message.insert(QStringLiteral("commandReady"), true);
                message.insert(QStringLiteral("validUntilServerMonotonicMs"), 20000);
                if (message.value(QStringLiteral("phase")).toString() == QLatin1String("Active"))
                    fixtureSessionState = message;
            }
            if (message.value(QStringLiteral("type")).toString() == QLatin1String("heartbeat_ack")
                && !fixtureSessionState.isEmpty()) {
                auto proof = fixtureSessionState;
                proof.insert(QStringLiteral("validUntilServerMonotonicMs"),
                    message.value(QStringLiteral("serverMonotonicMs")).toDouble() + 20000);
                message.insert(QStringLiteral("sessionStates"), QJsonArray{proof});
            }
            message.insert(QStringLiteral("serverBootId"), bootId);
            message.insert(QStringLiteral("messageId"),
                           QUuid::createUuid().toString(QUuid::WithoutBraces));
            peer->sendTextMessage(QString::fromUtf8(
                QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };
        connect(&server, &QWebSocketServer::newConnection, this, [&]() {
            QWebSocket* peer = server.nextPendingConnection();
            QVERIFY(peer);
            peer->setParent(&server);
            send(peer, {{"type", "auth_challenge"}, {"issuedAt", 1},
                        {"nonce", QString::fromLatin1(QByteArray(32, 'n').toBase64(
                            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}});
            connect(peer, &QWebSocket::textMessageReceived, &server,
                    [&, peer](const QString& encoded) {
                const QJsonObject message = QJsonDocument::fromJson(encoded.toUtf8()).object();
                const QString type = message.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("auth_response")) {
                    ownerId = DeviceIdentityStore::endpointIdForInstallation(
                        message.value(QStringLiteral("installationId")).toString(),
                        message.value(QStringLiteral("instanceId")).toString());
                    send(peer, {
                        {"type", "welcome"},
                        {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {"installationId", message.value("installationId")},
                        {"endpointId", ownerId},
                        {"instanceId", message.value("instanceId")},
                        {"instanceOrdinal", message.value("instanceOrdinal")},
                        {"runtimeId", message.value("runtimeId")},
                        {"connectionGeneration", 1}, {"serverMonotonicMs", 10},
                        {"policy", QJsonObject{
                            {"policyVersion", 5}, {"transportTimeoutMs", 5000}, {"heartbeatIntervalMs", 250},
                            {"transportSuspectAfterMs", 500}, {"sessionRecoveryTimeoutMs", 20000},
                            {"leaseTimeoutMs", 500}, {"scenePrepareTimeoutMs", 5000},
                            {"sceneActivationLeadMs", 1000}, {"sceneMaxClockSkewMs", 50},
                            {"sceneStartedAckTimeoutMs", 5000}, {"sceneMaxStartSkewMs", 750},
                            {"sceneStopTimeoutMs", 5000},
                            {"uploadIdleTimeoutMs", 45000}, {"uploadTargetAckTimeoutMs", 30000},
                            {"removalAckTimeoutMs", 30000}}}
                    });
                    send(peer, {
                        {"type", "remote_session_opened"},
                        {"remoteSessionId", "clock-retry-session"}, {"generation", 1},
                        {"ownerEndpointId", ownerId}, {"targetEndpointId", targetId},
                        {"ownerConnectionGeneration", 1}, {"targetConnectionGeneration", 1},
                        {"resumeToken", "clock-retry-token"}, {"phase", "Active"},
                        {"snapshotSequence", 1},
                        {"snapshot", QJsonObject{
                            {"screens", QJsonArray{}}, {"systemUI", QJsonArray{}},
                            {"volumePercent", QJsonValue::Null}, {"revision", 1},
                            {"capturedAtEpochMs", 1}}}
                    });
                } else if (type == QLatin1String("heartbeat")) {
                    // Deliberately leave the clock unmapped until PREPARED is
                    // complete. The launch must wait, not tear down the run.
                    if (preparedAttempts == 0) return;
                    ++clockRepliesAfterBarrier;
                    send(peer, {{"type", "heartbeat_ack"}, {"connectionGeneration", 1},
                                {"sequence", message.value("sequence")},
                                {"clientMonotonicMs", message.value("clientMonotonicMs")},
                                {"serverMonotonicMs", message.value("clientMonotonicMs")},
                                {"serverEpochMs", 1}});
                } else if (type == QLatin1String("scene_prepare")) {
                    send(peer, {
                        {"type", "prepare_progress"},
                        {"remoteSessionId", message.value("remoteSessionId")},
                        {"generation", message.value("generation")},
                        {"sceneRunId", message.value("sceneRunId")},
                        {"revision", message.value("revision")},
                        {"digest", message.value("digest")},
                        {"ownerEndpointId", ownerId},
                        {"targetEndpointId", targetId},
                        {"aggregate", true}, {"percent", 0}, {"stage", "accepted"}
                    });
                } else if (type == QLatin1String("prepared")
                           && message.value(QStringLiteral("success")).toBool()) {
                    if (++preparedAttempts == 1) return; // Lose the first locally queued PREPARED.
                    allPreparedSent = true;
                    send(peer, {
                        {"type", "prepared"},
                        {"remoteSessionId", message.value("remoteSessionId")},
                        {"generation", message.value("generation")},
                        {"sceneRunId", message.value("sceneRunId")},
                        {"revision", message.value("revision")},
                        {"digest", message.value("digest")},
                        {"ownerEndpointId", ownerId},
                        {"targetEndpointId", targetId},
                        {"success", true}, {"allPrepared", true},
                        {"checklist", message.value("checklist")}
                    });
                } else if (type == QLatin1String("armed")) {
                    armedBeforeClockReply = clockRepliesAfterBarrier == 0;
                    ++armedCount;
                } else if (type == QLatin1String("stop")) {
                    ++stopCount;
                }
            });
        });

        WebSocketClient client(directory.path(), false);
        QSignalSpy sessionOpened(&client, &WebSocketClient::remoteSessionOpened);
        client.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(sessionOpened.count(), 1, 3000);

        FileManager files;
        QQuickWindow window;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->controller()->registerWindow(&window);
        host->setFileManager(&files);
        host->setWebSocketClient(&client);
        host->setRemoteSceneTarget(targetId, QStringLiteral("Client B"));
        host->setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        host->setProjectEditingEnabled(true);
        host->setOverlayActionsEnabled(true);
        QVERIFY(host->document()->addText(QPointF(40, 60), QStringLiteral("Scene title")));

        host->triggerRemoteSceneAction();
        QTRY_VERIFY_WITH_TIMEOUT(allPreparedSent, 3000);
        QVERIFY(preparedAttempts >= 2);
        QTRY_COMPARE_WITH_TIMEOUT(armedCount, 1, 3000);
        QVERIFY(!armedBeforeClockReply);
        QVERIFY(clockRepliesAfterBarrier > 0);
        QCOMPARE(stopCount, 0);
        QVERIFY(host->remoteSceneLaunching());

        host->handleRemoteConnectionLost();
        client.disconnect();
    }

    void targetKeepsThePrepareDeadlineUntilCommit_data()
    {
        QTest::addColumn<bool>("includeImage");
        QTest::addColumn<int>("videoCount");
        QTest::addColumn<int>("startMs");
        QTest::addColumn<bool>("visible");
        QTest::addColumn<bool>("offscreen");
        QTest::newRow("text") << false << 0 << 0 << true << false;
        QTest::newRow("cached-image") << true << 0 << 0 << true << false;
        QTest::newRow("cached-video") << false << 1 << 0 << true << false;
        QTest::newRow("cached-video-seek") << false << 1 << 1234 << true << false;
        QTest::newRow("hidden-cached-video") << false << 1 << 1234 << false << false;
        QTest::newRow("offscreen-cached-video") << false << 1 << 1234 << true << true;
        QTest::newRow("mixed-shared-video") << true << 2 << 1234 << true << false;
    }

    void targetKeepsThePrepareDeadlineUntilCommit()
    {
        QFETCH(bool, includeImage);
        QFETCH(int, videoCount);
        QFETCH(int, startMs);
        QFETCH(bool, visible);
        QFETCH(bool, offscreen);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QWebSocketServer server(QStringLiteral("target-prepare-deadline-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString ownerId(43, QLatin1Char('A'));
        QString targetId;
        QWebSocket* peer = nullptr;
        int successfulPreparedCount = 0;
        int failedPreparedCount = 0;
        int stoppedCount = 0;
        QJsonArray preparedChecklist;
        int armedCount = 0;
        QJsonObject startedMessage;
        QList<qint64> presentedTimestamps;
        QJsonObject fixtureSessionState;
        auto send = [&](QWebSocket* socket, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), WebSocketClient::ProtocolVersion);
            if (message.contains(QStringLiteral("remoteSessionId"))) {
                message.insert(QStringLiteral("stateRevision"), 1);
                message.insert(QStringLiteral("commandReady"), true);
                message.insert(QStringLiteral("validUntilServerMonotonicMs"), 20000);
                if (message.value(QStringLiteral("phase")).toString() == QLatin1String("Active"))
                    fixtureSessionState = message;
            }
            if (message.value(QStringLiteral("type")).toString() == QLatin1String("heartbeat_ack")
                && !fixtureSessionState.isEmpty()) {
                auto proof = fixtureSessionState;
                proof.insert(QStringLiteral("validUntilServerMonotonicMs"),
                    message.value(QStringLiteral("serverMonotonicMs")).toDouble() + 20000);
                message.insert(QStringLiteral("sessionStates"), QJsonArray{proof});
            }
            message.insert(QStringLiteral("serverBootId"), bootId);
            message.insert(QStringLiteral("messageId"),
                           QUuid::createUuid().toString(QUuid::WithoutBraces));
            socket->sendTextMessage(QString::fromUtf8(
                QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };
        connect(&server, &QWebSocketServer::newConnection, this, [&]() {
            peer = server.nextPendingConnection();
            QVERIFY(peer);
            peer->setParent(&server);
            send(peer, {{"type", "auth_challenge"}, {"issuedAt", 1},
                        {"nonce", QString::fromLatin1(QByteArray(32, 'n').toBase64(
                            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}});
            connect(peer, &QWebSocket::textMessageReceived, &server,
                    [&, socket = peer](const QString& encoded) {
                const QJsonObject message = QJsonDocument::fromJson(encoded.toUtf8()).object();
                const QString type = message.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("auth_response")) {
                    targetId = DeviceIdentityStore::endpointIdForInstallation(
                        message.value(QStringLiteral("installationId")).toString(),
                        message.value(QStringLiteral("instanceId")).toString());
                    send(socket, {
                        {"type", "welcome"},
                        {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {"installationId", message.value("installationId")},
                        {"endpointId", targetId},
                        {"instanceId", message.value("instanceId")},
                        {"instanceOrdinal", message.value("instanceOrdinal")},
                        {"runtimeId", message.value("runtimeId")},
                        {"connectionGeneration", 1}, {"serverMonotonicMs", 10},
                        {"policy", QJsonObject{
                            {"policyVersion", 5}, {"transportTimeoutMs", 5000}, {"heartbeatIntervalMs", 250},
                            {"transportSuspectAfterMs", 500}, {"sessionRecoveryTimeoutMs", 20000},
                            {"leaseTimeoutMs", 500}, {"scenePrepareTimeoutMs", 8000},
                            {"sceneActivationLeadMs", 500}, {"sceneMaxClockSkewMs", 50},
                            {"sceneStartedAckTimeoutMs", 1000}, {"sceneMaxStartSkewMs", 750},
                            {"sceneStopTimeoutMs", 5000},
                            {"uploadIdleTimeoutMs", 45000}, {"uploadTargetAckTimeoutMs", 30000},
                            {"removalAckTimeoutMs", 30000}}}
                    });
                    send(socket, {
                        {"type", "remote_session_opened"},
                        {"remoteSessionId", "target-deadline-session"}, {"generation", 1},
                        {"ownerEndpointId", ownerId}, {"targetEndpointId", targetId},
                        {"ownerConnectionGeneration", 1}, {"targetConnectionGeneration", 1},
                        {"resumeToken", "target-deadline-token"}, {"phase", "Active"}
                    });
                } else if (type == QLatin1String("heartbeat")) {
                    send(socket, {{"type", "heartbeat_ack"}, {"connectionGeneration", 1},
                                  {"sequence", message.value("sequence")},
                                  {"clientMonotonicMs", message.value("clientMonotonicMs")},
                                  {"serverMonotonicMs", message.value("clientMonotonicMs")},
                                  {"serverEpochMs", 1}});
                } else if (type == QLatin1String("prepared")) {
                    if (message.value(QStringLiteral("success")).toBool()) {
                        ++successfulPreparedCount;
                        preparedChecklist = message.value(QStringLiteral("checklist")).toArray();
                    } else {
                        ++failedPreparedCount;
                    }
                } else if (type == QLatin1String("armed")) {
                    ++armedCount;
                } else if (type == QLatin1String("started")) {
                    startedMessage = message;
                    presentedTimestamps.append(message.value("presentedServerMonotonicMs").toInteger(-1));
                } else if (type == QLatin1String("stopped")) {
                    ++stoppedCount;
                }
            });
        });

        WebSocketClient client(directory.path(), false);
        FileManager files;
        RemoteSceneController controller(&files, &client);
        QSignalSpy sessionOpened(&client, &WebSocketClient::remoteSessionOpened);
        client.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(sessionOpened.count(), 1, 3000);
        QVERIFY(peer);

        std::unique_ptr<QuickCanvasHost> sceneSource(QuickCanvasHost::create());
        QVERIFY(sceneSource);
        sceneSource->setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        QVERIFY(sceneSource->document()->addText(
            QPointF(40, 60), QStringLiteral("Prepared target")));
        auto& residency = MediaResidencyManager::instance();
        QStringList receiverOwners;
        const auto releaseOwners = qScopeGuard([&] {
            for (const auto& owner : receiverOwners) residency.release(owner);
        });
        const RemoteCacheStore::Scope scope{ownerId, "target-deadline-session", 1};
        QHash<QString, QJsonObject> assets;
        const auto addCachedMedia = [&](const QString& path, bool video, int occurrence) {
            CanvasMedia* media = sceneSource->document()->addPreparedFile(
                path, QSize(160, 90), video,
                offscreen ? QPointF(-10000, -10000) : QPointF(100 + occurrence * 200, 100));
            QVERIFY(media);
            QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
            media->setContentVisible(visible);
            const QString fileId = media->fileId();
            QVERIFY(files.registerReceivedFilePath(scope, fileId, path));
            const QString owner = UploadManager::residencyOwnerId(
                scope.remoteSessionId, scope.generation, fileId);
            if (!receiverOwners.contains(owner)) {
                receiverOwners.append(owner);
                residency.acquire(owner, path);
                QTRY_VERIFY_WITH_TIMEOUT(residency.ready(owner), 5000);
            }
            auto asset = assets.value(fileId, QJsonObject{
                {"assetId", fileId}, {"fileId", fileId}, {"sha256", fileId},
                {"size", double(QFileInfo(path).size())},
                {"extension", video ? "mp4" : "png"}});
            auto ids = asset.value(QStringLiteral("mediaIds")).toArray();
            ids.append(media->mediaId());
            asset.insert(QStringLiteral("mediaIds"), ids);
            assets.insert(fileId, asset);
        };
        if (includeImage) {
            const QString path = directory.filePath(QStringLiteral("cached.png"));
            QImage image(160, 90, QImage::Format_RGBA8888);
            image.fill(Qt::cyan);
            QVERIFY(image.save(path));
            addCachedMedia(path, false, 0);
        }
        for (int index = 0; index < videoCount; ++index)
            addCachedMedia(QString::fromUtf8(TEST_VIDEO_FILE), true, index);
        QJsonObject scene = sceneSource->document()->serializeSceneState();
        QJsonArray entries = scene.value(QStringLiteral("media")).toArray();
        for (qsizetype index = 0; index < entries.size(); ++index) {
            auto entry = entries.at(index).toObject();
            if (entry.value(QStringLiteral("type")) != QLatin1String("text"))
                entry.insert(QStringLiteral("assetId"), entry.value(QStringLiteral("fileId")));
            if (entry.value(QStringLiteral("type")) == QLatin1String("video")) {
                SceneTimeline::MediaTrack track;
                QVERIFY(SceneTimeline::MediaTrack::fromJson(entry.value("timeline").toObject(), &track, 5400));
                track.clip = {SceneTimeline::newId(),0,SceneTimeline::SceneSettings{}.nearestSlot(startMs),
                    SceneTimeline::SceneSettings{}.sourceSlots(entry.value(QStringLiteral("durationMs")).toInteger())
                        -SceneTimeline::SceneSettings{}.nearestSlot(startMs)};
                entry.insert(QStringLiteral("timeline"),track.toJson());
                QCOMPARE(entry.value(QStringLiteral("spans")).toArray().isEmpty(), offscreen);
            }
            entries.replace(index, entry);
        }
        scene.insert(QStringLiteral("media"), entries);
        QJsonArray manifest;
        for (const auto& asset : assets) manifest.append(asset);
        QString manifestError;
        manifest = SceneRunCoordinator::normalizeManifest(manifest, &manifestError);
        QVERIFY2(manifestError.isEmpty(), qPrintable(manifestError));
        const QString runId = QStringLiteral("target-prepare-deadline-run");
        const QString digest = SceneRunCoordinator::computeDigest(1, manifest, scene);
        const QJsonObject correlation{
            {"remoteSessionId", "target-deadline-session"}, {"generation", 1},
            {"sceneRunId", runId}, {"revision", 1}, {"digest", digest},
            {"ownerEndpointId", ownerId}, {"targetEndpointId", targetId}
        };
        QJsonObject prepare = correlation;
        prepare.insert(QStringLiteral("type"), QStringLiteral("scene_prepare"));
        prepare.insert(QStringLiteral("manifest"), manifest);
        prepare.insert(QStringLiteral("scene"), scene);
        QElapsedTimer preparationTime;
        preparationTime.start();
        send(peer, prepare);

        QTRY_COMPARE_WITH_TIMEOUT(controller.findChildren<ResidentVideoPlayer*>().size(), videoCount, 3000);
        for (auto* player : controller.findChildren<ResidentVideoPlayer*>()) {
            QTRY_VERIFY_WITH_TIMEOUT(player->preparedAt(qRound64(SceneTimeline::SceneSettings{}.timeMs(SceneTimeline::SceneSettings{}.nearestSlot(startMs)))), 3000);
            QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        }
        QTRY_COMPARE_WITH_TIMEOUT(successfulPreparedCount, 1, 3000);
        qInfo() << "Cached scene prepared in" << preparationTime.elapsed() << "ms";
        QCOMPARE(failedPreparedCount, 0);
        QVERIFY(!preparedChecklist.isEmpty());
        QCOMPARE(preparedChecklist.size(), SceneRunCoordinator::createLocalChecklist(scene).size());
        for (const auto& stage : preparedChecklist)
            QVERIFY(stage.toObject().value(QStringLiteral("ready")).toBool());

        // The former implementation replaced the 8 s PREPARE deadline here
        // with activationLead + startedAck (500 + 1000 ms), even though no
        // COMMIT existed. Staying prepared beyond that interval is the
        // regression boundary.
        QTest::qWait(1800);
        QVERIFY(successfulPreparedCount >= 2); // Retry until the authoritative barrier is acknowledged.
        QCOMPARE(failedPreparedCount, 0);
        bool remoteWindowPresent = false;
        for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
            if (candidate && candidate->objectName()
                == QLatin1String("RemoteScreenWindow_0")) {
                remoteWindowPresent = true;
                QVERIFY(!candidate->isVisible()); // PREPARE must finish while hidden.
                break;
            }
        }
        QVERIFY(remoteWindowPresent);

        // Continue through the real receiver's clock/COMMIT/presentation path.
        // No test callback may manufacture a span-ready or first-frame signal.
        QVERIFY(startedMessage.isEmpty());
        QJsonObject prepared = correlation;
        prepared.insert(QStringLiteral("type"), QStringLiteral("prepared"));
        prepared.insert(QStringLiteral("allPrepared"), true);
        send(peer, prepared);
        QTRY_COMPARE_WITH_TIMEOUT(armedCount, 1, 3000);
        QJsonObject armed = correlation;
        armed.insert(QStringLiteral("type"), QStringLiteral("armed"));
        send(peer, armed);
        QJsonObject commit = correlation;
        commit.insert(QStringLiteral("type"), QStringLiteral("commit"));
        commit.insert(QStringLiteral("startServerMonotonicMs"),
                      double(client.estimatedServerMonotonicMs() + 300));
        commit.insert(QStringLiteral("startEpochMs"),
                      double(QDateTime::currentMSecsSinceEpoch() + 300));
        commit.insert(QStringLiteral("maximumClockUncertaintyMs"), 50);
        commit.insert(QStringLiteral("activationLeadMs"), 500);
        send(peer, commit);
        QTRY_VERIFY_WITH_TIMEOUT(!startedMessage.isEmpty(), 3000);
        QVERIFY(startedMessage.value(QStringLiteral("firstFramePresented")).toBool());
        QCOMPARE(failedPreparedCount, 0);
        const qint64 firstPresentation = startedMessage.value("presentedServerMonotonicMs").toInteger(-1);
        QVERIFY(firstPresentation >= 0);
        // Lose the STARTED response: retry the original observation, without
        // restarting or re-scheduling the already committed presentation.
        QTRY_VERIFY_WITH_TIMEOUT(presentedTimestamps.size() >= 2, 2500);
        send(peer, commit);
        QTRY_VERIFY_WITH_TIMEOUT(presentedTimestamps.size() >= 3, 1000);
        for (qint64 presented : presentedTimestamps) QCOMPARE(presented, firstPresentation);
        QJsonObject startedAck = correlation;
        startedAck.insert("type", "started");
        startedAck.insert("allStarted", true);
        send(peer, startedAck);
        QTest::qWait(50);
        const qsizetype acknowledgedCount = presentedTimestamps.size();
        QTest::qWait(1200);
        QCOMPARE(presentedTimestamps.size(), acknowledgedCount);

        QJsonObject stop = correlation;
        stop.insert(QStringLiteral("type"), QStringLiteral("stop"));
        stop.insert(QStringLiteral("reason"), QStringLiteral("test_cleanup"));
        send(peer, stop);
        QTRY_COMPARE_WITH_TIMEOUT(stoppedCount, 1, 3000);
        client.disconnect();
    }

    void localTestSurvivesRemoteConnectionLoss_data()
    {
        QTest::addColumn<bool>("video");
        QTest::addColumn<bool>("disconnectImmediately");
        QTest::newRow("text-timeline") << false << true;
        QTest::newRow("video-preparation") << true << true;
        QTest::newRow("video-playback") << true << false;
    }

    void localTestSurvivesRemoteConnectionLoss()
    {
        QFETCH(bool, video);
        QFETCH(bool, disconnectImmediately);
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        host->setOverlayActionsEnabled(true);
        CanvasMedia* media = video
            ? host->document()->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE),
                                                QSize(160, 90), true, {})
            : host->document()->addText({}, QStringLiteral("Local timeline"));
        QVERIFY(media);
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
        media->setContentVisible(true);
        media->setMuted(true);
        auto track=media->timelineTrack();
        auto hidden=media->authorElementState(),shown=hidden;
        hidden.visible=false; shown.visible=true;
        SceneTimeline::upsertKeyframe(track,{"hidden",0,hidden},180000);
        SceneTimeline::upsertKeyframe(track,{"shown",6,shown},180000);
        SceneTimeline::upsertKeyframe(track,{"hidden-again",18,hidden},180000);
        media->setTimelineTrack(track);
        bool sceneHidden = false, sceneDisplayed = false, sceneHiddenAgain = false;
        QObject timelineObserver;
        connect(media, &CanvasMedia::presentationChanged, &timelineObserver, [&] {
            if (!media->contentVisible()) {
                if (sceneDisplayed) sceneHiddenAgain = true;
                else sceneHidden = true;
            } else if (sceneHidden) {
                sceneDisplayed = true;
            }
        });
        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        if (!disconnectImmediately) QTRY_VERIFY_WITH_TIMEOUT(media->isPlaying(), 5000);

        host->handleRemoteConnectionLost();
        host->handleRemoteConnectionLost(); // Terminal cleanup may be repeated.
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->testSceneActionEnabled());
        QVERIFY(host->document()->editsLocked());
        QVERIFY(!host->overlayActionsEnabled());
        QVERIFY(!host->remoteSceneActionEnabled());
        // Verify the timeline itself survives, not just the UI's running flag.
        // Record transitions even if native video preparation completes between
        // event-loop polls; the draft's initial visibility is not scene playback.
        QTRY_VERIFY_WITH_TIMEOUT(sceneHiddenAgain, 5000);
        if (video) {
            QTRY_VERIFY_WITH_TIMEOUT(media->isPlaying(), 1500);
            const qint64 position = media->positionMs();
            QTRY_VERIFY_WITH_TIMEOUT(media->positionMs() > position + 100, 1500);
        }
        host->showContentAfterReconnect();
        host->setOverlayActionsEnabled(true);
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->document()->editsLocked());

        host->triggerTestSceneAction();
        QVERIFY(!host->testSceneLaunched());
        QVERIFY(!host->document()->editsLocked());
        QVERIFY(media->authorElementState().visible); // Playback never overwrites author state.
        if (video) QVERIFY(!media->isPlaying());

        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        host->stopScenesForSourceInvalidation();
        QVERIFY(!host->testSceneLaunched());
        QVERIFY(!host->document()->editsLocked());
        QVERIFY(media->authorElementState().visible);
    }

    void testScenePreservesAuthorAndFreezesAtPause()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);host->setProjectEditingEnabled(true);
        auto* media=host->document()->addText({40,60},"Scene title");
        auto a=media->authorElementState(),b=a;a.opacity=0;b.opacity=1;
        SceneTimeline::MediaTrack track = media->timelineTrack();
        SceneTimeline::upsertKeyframe(track,{"a",0,SceneTimeline::materialize(a)},180000);
        SceneTimeline::upsertKeyframe(track,{"b",30,SceneTimeline::materialize(b)},180000);
        media->setTimelineTrack(track);const auto saved=host->serializeProjectState();
        host->timelinePlay();QTRY_VERIFY(host->timelinePlaying());
        QTRY_VERIFY(host->timelinePositionMs()>100);
        host->timelinePause();QVERIFY(!host->document()->editsLocked());
        const auto position=host->timelinePositionMs();const auto displayed=media->displayedElementState().toJson();
        QTest::qWait(80);QCOMPARE(host->timelinePositionMs(),position);QCOMPARE(media->displayedElementState().toJson(),displayed);
        QCOMPARE(host->serializeProjectState(),saved);
    }

    void futureClipPreparesItsSourceFrameBeforeLocalPlayback()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* media = host->document()->addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE),
                                                       QSize(160,90), true, {});
        QVERIFY(media);
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(),5000);
        auto track = media->timelineTrack();
        track.clip.startSlot = 30;
        track.clip.sourceStartSlot = 60;
        track.clip.durationSlots = 30;
        media->setTimelineTrack(track);
        host->timelineSeek(0);
        QTRY_VERIFY_WITH_TIMEOUT(media->player()->preparedAt(2000),5000);
        QVERIFY(!media->clipActive());
        const auto frame = media->player()->preparedFrame(2000);
        QVERIFY(frame.isValid());
        QVERIFY(frame.startTime()/1000 <= 2000);
        QVERIFY(frame.endTime() > 2000000);
        host->timelinePlay();
        QTRY_VERIFY_WITH_TIMEOUT(host->timelinePlaying(),5000);
        QVERIFY(media->player()->preparedAt(2000));
        QCOMPARE(media->player()->position(),2000);
        QVERIFY(!media->player()->isPlaying());
        QVERIFY(media->player()->audioOutput()->isMuted());
        host->timelinePause();
    }

    void testSceneAppliesDisplayAndHideFades_data()
    {
        QTest::addColumn<QString>("mediaType");
        QTest::newRow("text") << QStringLiteral("text");
        QTest::newRow("image") << QStringLiteral("image");
        QTest::newRow("video") << QStringLiteral("video");
    }

    void testSceneAppliesDisplayAndHideFades()
    {
        QFETCH(QString, mediaType);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        CanvasMedia* media = nullptr;
        if (mediaType == QLatin1String("text")) {
            media = host->document()->addText({}, QStringLiteral("Fading title"));
        } else if (mediaType == QLatin1String("image")) {
            QImage image(32, 24, QImage::Format_RGBA8888);
            image.fill(Qt::cyan);
            const QString path = directory.filePath(QStringLiteral("fade.png"));
            QVERIFY(image.save(path));
            media = host->document()->addPreparedFile(path, image.size(), false, {});
        } else {
            const QString override = qEnvironmentVariable("MOUFFETTE_TEST_VIDEO_FILE");
            const QString path = override.isEmpty() ? QString::fromUtf8(TEST_VIDEO_FILE) : override;
            QVERIFY(QFile::exists(path));
            media = host->document()->addPreparedFile(path, QSize(160, 90), true, {});
        }
        QVERIFY(media);
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
        auto a=media->authorElementState(),b=a,c=a;
        a.opacity=0;b.opacity=.4;c.opacity=0;
        SceneTimeline::MediaTrack track=media->timelineTrack();
        // Keep both interpolation samples inside the clip, independently of
        // the configured default duration for a newly created text/image.
        track.clip.durationSlots=std::max<qint64>(track.clip.durationSlots,61);
        SceneTimeline::upsertKeyframe(track,{"start",0,SceneTimeline::materialize(a)},180000);
        SceneTimeline::upsertKeyframe(track,{"middle",30,SceneTimeline::materialize(b)},180000);
        SceneTimeline::upsertKeyframe(track,{"end",60,SceneTimeline::materialize(c)},180000);
        media->setTimelineTrack(track);
        const auto saved=host->serializeProjectState();
        host->timelineSeek(250);QVERIFY(qAbs(media->contentOpacity()-(.4*8/30))<.0001);
        host->timelineSeek(1500);QVERIFY(qAbs(media->contentOpacity()-.2)<.0001);
        host->timelineSeek(0);QCOMPARE(media->contentOpacity(),0.0);
        QCOMPARE(host->serializeProjectState(),saved);
    }



    void repeatedPreviewUsesOneTimelineClock()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);host->setProjectEditingEnabled(true);
        auto* media=host->document()->addText({},"Restart");
        auto a=media->authorElementState(),b=a;a.position={0,0};b.position={100,0};
        SceneTimeline::MediaTrack track = media->timelineTrack();
        SceneTimeline::upsertKeyframe(track,{"a",0,a},180000);SceneTimeline::upsertKeyframe(track,{"b",30,b},180000);
        media->setTimelineTrack(track);
        for(int run=0;run<3;++run) {
            host->timelineSeek(0);host->timelinePlay();QTRY_VERIFY(host->timelinePositionMs()>80);
            host->timelinePause();const auto t=host->timelinePositionMs();
            QTest::qWait(100);QCOMPARE(host->timelinePositionMs(),t);
            QVERIFY(qAbs(media->position().x()-qMin<qreal>(100,t/10.0))<.001);
        }
    }



    void sceneSerializationSeparatesAuthorEvaluationAndUnrecordedDraft()
    {
        CanvasDocument document;auto* media=document.addText({},"Persistent");
        auto a=media->authorElementState(),b=a;b.position={700,100};b.uppercase=true;
        SceneTimeline::MediaTrack track = media->timelineTrack();
        SceneTimeline::upsertKeyframe(track,{"a",3,a},180000);SceneTimeline::upsertKeyframe(track,{"b",33,b},180000);
        media->setTimelineTrack(track);const auto saved=document.serializeProjectState();
        QSignalSpy writes(&document,&CanvasDocument::documentChanged);
        document.setTimelinePosition(600);QCOMPARE(media->position(),(a.position+b.position)/2);
        media->beginElementEdit();media->setUppercase(true);QVERIFY(media->hasElementDraft());
        QCOMPARE(document.serializeProjectState(),saved);QCOMPARE(writes.count(),0);
        document.setTimelinePosition(600);QVERIFY(!media->hasElementDraft());QVERIFY(!media->uppercase());
        QCOMPARE(document.serializeSceneState().value("renderSchemaVersion").toInt(),6);
        const auto serialized=document.serializeSceneState().value("media").toArray()[0].toObject();
        QVERIFY(!serialized.contains("autoDisplay"));QVERIFY(!serialized.contains("projectMediaSettings"));
    }

    void projectRoundTripPreservesTypedSettingsGeometryAndText()
    {
        std::unique_ptr<QuickCanvasHost> source(QuickCanvasHost::create());
        QVERIFY(source);
        source->setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        CanvasMedia* text = source->document()->addText(
            QPointF(120, 80), QStringLiteral("Round trip"));
        QVERIFY(text);
        text->setBaseSize(QSize(640, 220));
        text->setScale(1.25);
        text->setTextColor(QColor(QStringLiteral("#ff55aa")));
        text->setTextColorOverrideEnabled(true);
        text->setOutlineWidthPercent(8.5);
        text->setOutlineWidthOverrideEnabled(true);
        text->setItalic(true);
        text->setUppercase(true);
        MediaSettingsState settings = text->settings();
        settings.opacityOverrideEnabled = true;
        settings.opacityText = QStringLiteral("72.5");
        text->setSettings(settings);
        source->document()->setCamera(1.7, 32, -14);
        const QPointF expectedPosition = text->position();
        const QSize expectedBaseSize = text->baseSize();

        const QJsonObject state = source->serializeProjectState();
        std::unique_ptr<QuickCanvasHost> restored(QuickCanvasHost::create());
        QVERIFY(restored);
        QStringList skipped;
        QVERIFY(restored->restoreProjectState(state, {}, &skipped));
        QVERIFY(skipped.isEmpty());
        QCOMPARE(restored->enumerateMediaItems().size(), 1);
        CanvasMedia* copy = restored->enumerateMediaItems().first();
        QVERIFY(copy && copy->isText());
        QCOMPARE(copy->mediaId(), text->mediaId());
        QCOMPARE(copy->text(), QStringLiteral("Round trip"));
        QCOMPARE(copy->position(), expectedPosition);
        QCOMPARE(copy->baseSize(), expectedBaseSize);
        QCOMPARE(copy->scale(), 1.25);
        QCOMPARE(copy->settings().opacityText, QStringLiteral("72.5"));
        QVERIFY(copy->textColorOverrideEnabled());
        QVERIFY(copy->outlineWidthOverrideEnabled());
        QCOMPARE(copy->textColor(), QColor(QStringLiteral("#ff55aa")));
        QCOMPARE(copy->outlineWidthPercent(), 8.5);
        QCOMPARE(restored->document()->cameraScale(), 1.7);
        QCOMPARE(restored->document()->cameraPanX(), 32.0);
        QCOMPARE(restored->document()->cameraPanY(), -14.0);
    }

    void disabledOverridesKeepRawValuesAndPublishNeutralRendering()
    {
        std::unique_ptr<QuickCanvasHost> source(QuickCanvasHost::create());
        QVERIFY(source);
        CanvasMedia* text = source->document()->addText(
            QPointF(100, 100), QStringLiteral("Overrides"));
        QVERIFY(text);
        text->setFitToTextEnabled(false);
        text->setFontWeight(900);
        text->setTextColor(QColor(QStringLiteral("#8044aa22")));
        text->setOutlineWidthPercent(100.0);
        text->setOutlineColor(QColor(QStringLiteral("#ff22ccdd")));

        MediaSettingsState settings = text->settings();
        settings.opacityOverrideEnabled = false;
        settings.opacityText = QStringLiteral("23.5");
        text->setSettings(settings);

        QCOMPARE(text->fontWeight(), 900);
        QCOMPARE(text->textColor(), QColor(QStringLiteral("#8044aa22")));
        QCOMPARE(text->outlineWidthPercent(), 100.0);
        QCOMPARE(text->outlineColor(), QColor(QStringLiteral("#ff22ccdd")));
        QCOMPARE(text->contentOpacity(), 1.0);

        const QVariantMap disabledProjection = text->toModelMap();
        QCOMPARE(disabledProjection.value(
                     QStringLiteral("textFontWeight")).toInt(), 400);
        QCOMPARE(QColor(disabledProjection.value(
                     QStringLiteral("textColor")).toString()),
                 QColor(Qt::white));
        QCOMPARE(disabledProjection.value(
                     QStringLiteral("textOutlineWidthPx")).toDouble(), 0.0);
        QCOMPARE(QColor(disabledProjection.value(
                     QStringLiteral("textOutlineColor")).toString()),
                 QColor(Qt::black));

        const QJsonObject project = source->serializeProjectState();
        const QJsonObject serializedText =
            project.value(QStringLiteral("media")).toArray().first().toObject();
        QCOMPARE(serializedText.value(QStringLiteral("contentOpacity")).toDouble(),
                 1.0);
        QCOMPARE(serializedText.value(QStringLiteral("fontWeight")).toInt(), 400);
        QCOMPARE(serializedText.value(
                     QStringLiteral("textBorderWidthPercent")).toDouble(),
                 0.0);
        const QJsonObject rawTextSettings = serializedText;
        QCOMPARE(rawTextSettings.value(QStringLiteral("rawFontWeight")).toInt(), 900);
        QCOMPARE(rawTextSettings.value(
                     QStringLiteral("rawOutlineWidthPercent")).toDouble(),
                 100.0);
        QVERIFY(!rawTextSettings.value(
                     QStringLiteral("fontWeightOverrideEnabled")).toBool(true));
        QVERIFY(!rawTextSettings.value(
                     QStringLiteral("outlineWidthOverrideEnabled")).toBool(true));

        std::unique_ptr<QuickCanvasHost> restored(QuickCanvasHost::create());
        QVERIFY(restored);
        QVERIFY(restored->restoreProjectState(project, {}));
        CanvasMedia* copy = restored->enumerateMediaItems().first();
        QVERIFY(copy);
        QVERIFY(!copy->fontWeightOverrideEnabled());
        QVERIFY(!copy->textColorOverrideEnabled());
        QVERIFY(!copy->outlineWidthOverrideEnabled());
        QVERIFY(!copy->outlineColorOverrideEnabled());
        QCOMPARE(copy->fontWeight(), 900);
        QCOMPARE(copy->textColor(), QColor(QStringLiteral("#8044aa22")));
        QCOMPARE(copy->outlineWidthPercent(), 100.0);
        QCOMPARE(copy->outlineColor(), QColor(QStringLiteral("#ff22ccdd")));
        QCOMPARE(copy->settings().opacityText, QStringLiteral("23.5"));
        QCOMPARE(copy->contentOpacity(), 1.0);

        copy->setFontWeightOverrideEnabled(true);
        copy->setTextColorOverrideEnabled(true);
        copy->setOutlineWidthOverrideEnabled(true);
        copy->setOutlineColorOverrideEnabled(true);
        settings = copy->settings();
        settings.opacityOverrideEnabled = true;
        copy->setSettings(settings);
        const QVariantMap enabledProjection = copy->toModelMap();
        QCOMPARE(enabledProjection.value(
                     QStringLiteral("textFontWeight")).toInt(), 900);
        QCOMPARE(QColor(enabledProjection.value(
                     QStringLiteral("textColor")).toString()),
                 QColor(QStringLiteral("#8044aa22")));
        QVERIFY(enabledProjection.value(
                    QStringLiteral("textOutlineWidthPx")).toDouble() > 0.0);
        QCOMPARE(QColor(enabledProjection.value(
                     QStringLiteral("textOutlineColor")).toString()),
                 QColor(QStringLiteral("#ff22ccdd")));
        QVERIFY(qAbs(copy->contentOpacity() - 0.235) < 0.0001);

        CanvasMedia video(CanvasMedia::Type::Video, QSize(320, 180));
        video.initializeVideoRuntime();
        MediaSettingsState videoSettings = video.settings();
        videoSettings.volumeOverrideEnabled = true;
        videoSettings.volumeText = QStringLiteral("37");
        video.setSettings(videoSettings);
        QVERIFY(qAbs(video.volume() - 0.37) < 0.0001);
        videoSettings.volumeOverrideEnabled = false;
        video.setSettings(videoSettings);
        QCOMPARE(video.settings().volumeText, QStringLiteral("37"));
        QVERIFY(qAbs(video.volume() - 1.0) < 0.0001);
        videoSettings.volumeOverrideEnabled = true;
        video.setSettings(videoSettings);
        QVERIFY(qAbs(video.volume() - 0.37) < 0.0001);

        QTemporaryDir volumeDirectory;
        QVERIFY(volumeDirectory.isValid());
        std::unique_ptr<QuickCanvasHost> volumeHost(QuickCanvasHost::create());
        QVERIFY(volumeHost);
        volumeHost->setProjectEditingEnabled(true);
        CanvasMedia* controlledVideo = volumeHost->document()->addPreparedFile(
            QString::fromUtf8(TEST_VIDEO_FILE),
            QSize(320, 180), true, QPointF());
        QVERIFY(controlledVideo);
        volumeHost->controller()->handleOverlayVolumeChange(controlledVideo->mediaId(), 0.42);
        QVERIFY(!controlledVideo->settings().volumeOverrideEnabled);
        QTRY_VERIFY_WITH_TIMEOUT(controlledVideo->residencyReady(), 60000);
        volumeHost->controller()->handleOverlayVolumeChange(
            controlledVideo->mediaId(), 0.42);
        QVERIFY(controlledVideo->settings().volumeOverrideEnabled);
        QCOMPARE(controlledVideo->settings().volumeText, QStringLiteral("42"));
        QVERIFY(qAbs(controlledVideo->volume() - 0.42) < 0.0001);
    }

    void missingFileIsRejectedDuringRestore()
    {
        std::unique_ptr<QuickCanvasHost> source(QuickCanvasHost::create());
        QVERIFY(source);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("asset.png"));
        QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::blue);
        QVERIFY(image.save(path));
        CanvasMedia* media = source->document()->addPreparedFile(
            path, image.size(), false, QPointF(4, 5));
        QVERIFY(media);
        const QString id = media->mediaId();
        const QJsonObject state = source->serializeProjectState();

        std::unique_ptr<QuickCanvasHost> restored(QuickCanvasHost::create());
        QStringList skipped;
        QVERIFY(restored->restoreProjectState(state, {}, &skipped));
        QCOMPARE(restored->enumerateMediaItems().size(), 0);
        QCOMPARE(skipped, QStringList{id});
    }
};

QTEST_MAIN(RemoteSceneLifecycleTest)
#include "tst_RemoteSceneLifecycle.moc"
