#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUuid>
#include <QWebSocketServer>
#include <QtTest>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/files/FileManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/security/DeviceIdentityStore.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"

class RemoteSceneLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void remoteLaunchPreparesLocalMedia_data()
    {
        QTest::addColumn<bool>("includeImage");
        QTest::addColumn<bool>("missingSource");
        QTest::newRow("text-only") << false << false;
        QTest::newRow("text-and-image") << true << false;
        QTest::newRow("missing-local-image-source") << true << true;
    }

    void remoteLaunchPreparesLocalMedia()
    {
        QFETCH(bool, includeImage);
        QFETCH(bool, missingSource);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QWebSocketServer server(QStringLiteral("scene-preparation-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString targetId(43, QLatin1Char('B'));
        QJsonObject prepared;
        QJsonObject progress;
        bool stopReceived = false;
        auto send = [&](QWebSocket* peer, QJsonObject message) {
            message.insert(QStringLiteral("protocolVersion"), 3);
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
                const QString type = message.value("type").toString();
                if (type == QLatin1String("auth_response")) {
                    const QString ownerId = DeviceIdentityStore::endpointIdForInstallation(
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
                            {"leaseTimeoutMs", 1000}, {"scenePrepareTimeoutMs", 15000},
                            {"sceneActivationLeadMs", 4000}, {"sceneMaxClockSkewMs", 50},
                            {"sceneStartedAckTimeoutMs", 5000}, {"sceneMaxStartSkewMs", 750},
                            {"uploadIdleTimeoutMs", 45000}, {"uploadTargetAckTimeoutMs", 30000},
                            {"removalAckTimeoutMs", 30000}}}
                    });
                    send(peer, {
                        {"type", "remote_session_opened"},
                        {"remoteSessionId", "preparation-session"}, {"generation", 1},
                        {"ownerEndpointId", ownerId}, {"targetEndpointId", targetId},
                        {"ownerConnectionGeneration", 1}, {"targetConnectionGeneration", 1},
                        {"resumeToken", "test-resume-token"}, {"phase", "Active"}
                    });
                } else if (type == QLatin1String("heartbeat")) {
                    send(peer, {{"type", "heartbeat_ack"}, {"connectionGeneration", 1},
                                {"sequence", message.value("sequence")},
                                {"clientMonotonicMs", message.value("clientMonotonicMs")},
                                {"serverMonotonicMs", message.value("clientMonotonicMs")},
                                {"serverEpochMs", 1}});
                } else if (type == QLatin1String("prepared")) {
                    prepared = message;
                } else if (type == QLatin1String("prepare_progress")) {
                    progress = message;
                } else if (type == QLatin1String("stop")) {
                    stopReceived = true;
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
        if (includeImage) {
            const QString path = directory.filePath(QStringLiteral("asset.png"));
            QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::blue);
            QVERIFY(image.save(path));
            CanvasMedia* media = host->document()->addPreparedFile(
                path, image.size(), false, QPointF(4, 5));
            QVERIFY(media);
            if (missingSource) {
                // The uploaded asset is valid, but the canvas runtime lost its source.
                media->setSourcePath(directory.filePath(QStringLiteral("missing.png")));
            }
        }
        QVERIFY(host->remoteSceneActionEnabled());
        host->triggerRemoteSceneAction();
        QTRY_VERIFY_WITH_TIMEOUT(!prepared.isEmpty(), 3000);
        QCOMPARE(prepared.value("success").toBool(), !missingSource);
        QCOMPARE(host->remoteSceneLaunching(), !missingSource);

        const QJsonArray checklist = prepared.value("checklist").toArray();
        QCOMPARE(checklist.size(), includeImage ? 6 : 3);
        int readyCount = 0;
        for (const QJsonValue& value : checklist) {
            const QJsonObject item = value.toObject();
            // Protocol v3 rejects any extra keys, including mediaId.
            QCOMPARE(item.keys(), (QStringList{"itemId", "ready", "stage"}));
            if (item.value("ready").toBool()) ++readyCount;
        }
        QVERIFY(checklist.first().toObject().value("ready").toBool());
        QTRY_VERIFY(!progress.isEmpty());
        QCOMPARE(progress.value("checklist").toArray(), checklist);
        QCOMPARE(progress.value("percent").toInt(), readyCount * 100 / checklist.size());
        if (missingSource) {
            QVERIFY(readyCount < checklist.size());
            QVERIFY(prepared.value("message").toString().contains(QStringLiteral("missing.png")));
            QTRY_VERIFY(stopReceived);
            QVERIFY(!host->document()->editsLocked());
        } else {
            QCOMPARE(readyCount, checklist.size());
            QVERIFY(!stopReceived);
        }
        host->handleRemoteConnectionLost();
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
                     QStringLiteral("textOutlineWidthPercent")).toDouble(),
                 0.0);
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
        QCOMPARE(enabledProjection.value(
                     QStringLiteral("textOutlineWidthPercent")).toDouble(),
                 100.0);
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
            volumeDirectory.filePath(QStringLiteral("volume-setting-test.mp4")),
            QSize(320, 180), true, QPointF());
        QVERIFY(controlledVideo);
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
