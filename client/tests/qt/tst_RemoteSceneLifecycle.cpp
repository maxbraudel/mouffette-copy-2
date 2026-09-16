#include "backend/network/UploadManager.h"
#include "backend/media/MediaResidencyManager.h"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>
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
#include "backend/network/WebSocketClient.h"
#include "backend/security/DeviceIdentityStore.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
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
        manager.setMemorySnapshotForTesting(
            {8ULL << 30, 1ULL << 30, 128ULL << 20, false, 0});
        // A protected scene survives the first pressure sample.
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->testSceneActionEnabled()); // Stop is still available.
        QVERIFY(media->residencyReady());
        QCOMPARE(pressureStop.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!host->testSceneLaunched(), 4500);
        QVERIFY(pressureDuration.elapsed() >= 1900);
        QCOMPARE(pressureStop.size(), 1);
        QVERIFY(pressureStop.first().at(0).toString().startsWith(QStringLiteral("canvas-test:")));
        QTRY_COMPARE_WITH_TIMEOUT(media->residencyState(), QStringLiteral("waiting_for_memory"), 1500);
        QVERIFY(media->residencyError().contains(QStringLiteral("RAM")));
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
        QJsonObject prepared;
        QJsonObject progress;
        int preparedCount = 0;
        int progressCount = 0;
        int stopCount = 0;
        auto send = [&](QWebSocket* peer, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), 5);
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
                        {"runtimeId", message.value("runtimeId")},
                        {"connectionGeneration", 1}, {"serverMonotonicMs", 10},
                        {"policy", QJsonObject{
                            {"policyVersion", 1}, {"heartbeatIntervalMs", 250},
                            {"leaseTimeoutMs", 3000}, {"scenePrepareTimeoutMs", 1000},
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
        ClientWorkspaceViewModel workspace(QStringLiteral("persistent-workspace"), host.get(),
            [] {}, &uploads, [] { return true; }, [] { return false; }, [] { return true; });
        auto* listModel = qobject_cast<QAbstractItemModel*>(workspace.mediaModel());
        QVERIFY(listModel);
        const auto rowCached = [listModel](const QString& mediaId) {
            for (int row = 0; row < listModel->rowCount(); ++row) {
                const auto value = listModel->data(listModel->index(row, 0), MediaListModel::ModelDataRole).toMap();
                if (value.value(QStringLiteral("mediaId")).toString() == mediaId)
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
                QVERIFY(!rowCached(media->mediaId())); // Upload alone is not a cache acknowledgement.
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
                    QTRY_VERIFY(rowCached(media->mediaId()));
                    QVERIFY(!rowChanges.isEmpty());
                    rowChanges.clear();
                    report(QStringLiteral("waiting_for_memory"), 2);
                    QTRY_VERIFY(!rowCached(media->mediaId()));
                    QVERIFY(!rowChanges.isEmpty());
                    report(QStringLiteral("ready"), 3);
                    QTRY_VERIFY(rowCached(media->mediaId()));
                    // A workspace ID and its current transport endpoint are different identities.
                    host->setRemoteSceneTarget(QStringLiteral("another-peer"), {});
                    QVERIFY(!rowCached(media->mediaId()));
                    host->setRemoteSceneTarget(targetId, QStringLiteral("Client B"));
                    QVERIFY(rowCached(media->mediaId()));
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
        host->handleRemoteConnectionLost();
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
        auto send = [&](QWebSocket* peer, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), 5);
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
                        {"runtimeId", message.value("runtimeId")},
                        {"connectionGeneration", 1}, {"serverMonotonicMs", 10},
                        {"policy", QJsonObject{
                            {"policyVersion", 1}, {"heartbeatIntervalMs", 250},
                            {"leaseTimeoutMs", 3000}, {"scenePrepareTimeoutMs", 5000},
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
                    // Keep the run in PREPARE: the exclusion must not depend
                    // on a later accepted/prepared/commit transition.
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
        QTRY_COMPARE_WITH_TIMEOUT(scenePrepareCount, 1, 3000);
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
        QCOMPARE(scenePrepareCount, 1);

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
        bool armedBeforeClockReply = false;
        int clockRepliesAfterBarrier = 0;
        int armedCount = 0;
        int stopCount = 0;
        auto send = [&](QWebSocket* peer, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), 5);
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
                        {"runtimeId", message.value("runtimeId")},
                        {"connectionGeneration", 1}, {"serverMonotonicMs", 10},
                        {"policy", QJsonObject{
                            {"policyVersion", 1}, {"heartbeatIntervalMs", 250},
                            {"leaseTimeoutMs", 10000}, {"scenePrepareTimeoutMs", 5000},
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
                    if (!allPreparedSent) return;
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
        QTRY_COMPARE_WITH_TIMEOUT(armedCount, 1, 3000);
        QVERIFY(!armedBeforeClockReply);
        QVERIFY(clockRepliesAfterBarrier > 0);
        QCOMPARE(stopCount, 0);
        QVERIFY(host->remoteSceneLaunching());

        host->handleRemoteConnectionLost();
        client.disconnect();
    }

    void targetKeepsThePrepareDeadlineUntilCommit()
    {
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
        auto send = [&](QWebSocket* socket, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), 5);
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
                        {"runtimeId", message.value("runtimeId")},
                        {"connectionGeneration", 1}, {"serverMonotonicMs", 10},
                        {"policy", QJsonObject{
                            {"policyVersion", 1}, {"heartbeatIntervalMs", 250},
                            {"leaseTimeoutMs", 10000}, {"scenePrepareTimeoutMs", 8000},
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
                    } else {
                        ++failedPreparedCount;
                    }
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
        const QJsonObject scene = sceneSource->document()->serializeSceneState();
        const QJsonArray manifest;
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
        send(peer, prepare);

        QTRY_COMPARE_WITH_TIMEOUT(successfulPreparedCount, 1, 3000);
        QCOMPARE(failedPreparedCount, 0);

        // The former implementation replaced the 8 s PREPARE deadline here
        // with activationLead + startedAck (500 + 1000 ms), even though no
        // COMMIT existed. Staying prepared beyond that interval is the
        // regression boundary.
        QTest::qWait(1800);
        QCOMPARE(successfulPreparedCount, 1);
        QCOMPARE(failedPreparedCount, 0);
        bool remoteWindowPresent = false;
        for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
            if (candidate && candidate->objectName()
                == QLatin1String("RemoteScreenWindow_0")) {
                remoteWindowPresent = true;
                break;
            }
        }
        QVERIFY(remoteWindowPresent);

        QJsonObject stop = correlation;
        stop.insert(QStringLiteral("type"), QStringLiteral("stop"));
        stop.insert(QStringLiteral("reason"), QStringLiteral("test_cleanup"));
        send(peer, stop);
        QTRY_COMPARE_WITH_TIMEOUT(stoppedCount, 1, 3000);
        client.disconnect();
    }

    void testSceneRestoresImmutableDraftState()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        CanvasMedia* media = host->document()->addText(
            QPointF(40, 60), QStringLiteral("Scene title"));
        QVERIFY(media);
        media->setContentVisible(true);
        MediaSettingsState settings = media->settings();
        settings.displayAutomatically = false;
        settings.displayDelayEnabled = false;
        media->setSettings(settings);
        host->document()->select(media->mediaId());
        host->setOverlayActionsEnabled(true);
        QVERIFY(host->testSceneActionEnabled());

        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->document()->editsLocked());
        QVERIFY(host->document()->selectedMediaIds().isEmpty());
        QTRY_VERIFY(!media->contentVisible());

        // Scene playback may mutate runtime state, but stop restores the draft.
        media->setAnimatedDisplayOpacity(0.35);
        host->triggerTestSceneAction();
        QVERIFY(!host->testSceneLaunched());
        QVERIFY(!host->document()->editsLocked());
        QVERIFY(media->contentVisible());
        QCOMPARE(media->animatedDisplayOpacity(), 1.0);
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
        settings.displayDelayEnabled = true;
        settings.displayDelayText = QStringLiteral("1.375");
        settings.fadeInEnabled = true;
        settings.fadeInText = QStringLiteral("0.45");
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
        QCOMPARE(copy->settings().displayDelayText, QStringLiteral("1.375"));
        QCOMPARE(copy->settings().fadeInText, QStringLiteral("0.45"));
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
        const QJsonObject rawTextSettings = serializedText.value(
            QStringLiteral("projectTextSettings")).toObject();
        QCOMPARE(rawTextSettings.value(QStringLiteral("fontWeight")).toInt(), 900);
        QCOMPARE(rawTextSettings.value(
                     QStringLiteral("textBorderWidthPercent")).toDouble(),
                 100.0);
        QVERIFY(!rawTextSettings.value(
                     QStringLiteral("fontWeightOverrideEnabled")).toBool(true));
        QVERIFY(!rawTextSettings.value(
                     QStringLiteral("textBorderWidthOverrideEnabled")).toBool(true));

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
