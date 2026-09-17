#include <QtTest>
#include <QJsonDocument>
#include <QImage>
#include <QQuickWindow>
#include <QApplication>
#include "backend/network/SceneRunCoordinator.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include <QFile>
#include <QCryptographicHash>
#include "backend/files/FileManager.h"
#include "backend/network/UploadManager.h"
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/managers/network/ConnectionManager.h"

namespace {
QJsonObject liveTextScene()
{
    QJsonObject screen;
    screen[QStringLiteral("id")] = 0;
    screen[QStringLiteral("x")] = 0;
    screen[QStringLiteral("y")] = 0;
    screen[QStringLiteral("width")] = 1920;
    screen[QStringLiteral("height")] = 1080;
    screen[QStringLiteral("primary")] = true;

    QJsonObject span;
    span[QStringLiteral("screenId")] = 0;
    span[QStringLiteral("normX")] = 0.0;
    span[QStringLiteral("normY")] = 0.0;
    span[QStringLiteral("normW")] = 1.0;
    span[QStringLiteral("normH")] = 1.0;
    span[QStringLiteral("spanDestNormX")] = 0.0;
    span[QStringLiteral("spanDestNormY")] = 0.0;
    span[QStringLiteral("spanDestNormW")] = 1.0;
    span[QStringLiteral("spanDestNormH")] = 1.0;
    span[QStringLiteral("spanSourceNormX")] = 0.0;
    span[QStringLiteral("spanSourceNormY")] = 0.0;
    span[QStringLiteral("spanSourceNormW")] = 1.0;
    span[QStringLiteral("spanSourceNormH")] = 1.0;

    QJsonObject media;
    media[QStringLiteral("mediaId")] = QStringLiteral("text-1");
    media[QStringLiteral("fileId")] = QString();
    media[QStringLiteral("fileName")] = QString();
    media[QStringLiteral("type")] = QStringLiteral("text");
    media[QStringLiteral("text")] = QStringLiteral("teardown-test");
    media[QStringLiteral("x")] = 0.0;
    media[QStringLiteral("y")] = 0.0;
    media[QStringLiteral("width")] = 320.0;
    media[QStringLiteral("height")] = 180.0;
    media[QStringLiteral("baseWidth")] = 320;
    media[QStringLiteral("baseHeight")] = 180;
    media[QStringLiteral("visible")] = true;
    media[QStringLiteral("z")] = 1.0;
    media[QStringLiteral("autoDisplay")] = false;
    media[QStringLiteral("autoDisplayDelayMs")] = 0;
    media[QStringLiteral("autoHide")] = false;
    media[QStringLiteral("autoHideDelayMs")] = 0;
    media[QStringLiteral("hideWhenVideoEnds")] = false;
    media[QStringLiteral("fadeInSeconds")] = 0.0;
    media[QStringLiteral("fadeOutSeconds")] = 0.0;
    media[QStringLiteral("contentOpacity")] = 1.0;
    media[QStringLiteral("fontFamily")] = QStringLiteral("Arial");
    media[QStringLiteral("fontItalic")] = false;
    media[QStringLiteral("fontUnderline")] = false;
    media[QStringLiteral("fontUppercase")] = false;
    media[QStringLiteral("fontWeight")] = 400;
    media[QStringLiteral("fontPixelSize")] = 20;
    media[QStringLiteral("textColor")] = QStringLiteral("#ffffffff");
    media[QStringLiteral("textOutlineWidthPx")] = 0.0;
    media[QStringLiteral("textBorderColor")] = QStringLiteral("#00000000");
    media[QStringLiteral("textFitToTextEnabled")] = false;
    media[QStringLiteral("textHighlightEnabled")] = false;
    media[QStringLiteral("textHighlightColor")] = QStringLiteral("#00000000");
    media[QStringLiteral("horizontalAlignment")] = QStringLiteral("center");
    media[QStringLiteral("verticalAlignment")] = QStringLiteral("center");
    media[QStringLiteral("spans")] = QJsonArray{span};

    QJsonObject scene;
    scene[QStringLiteral("renderSchemaVersion")] = 2;
    scene[QStringLiteral("screens")] = QJsonArray{screen};
    scene[QStringLiteral("media")] = QJsonArray{media};
    return scene;
}

}

class RemoteSessionIntegrationTest final : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void recovery_data();
    void recovery();
    void duplicateOpenAndMetadataRefresh();
    void appliedBarrierRecoversLostAcknowledgement_data();
    void appliedBarrierRecoversLostAcknowledgement();
    void localProofExpiryClosesAStillHealthyServerSession();
    void uploadResumesFromDurableOffsetAfterTransportLoss();
    void cleanupReceiptRetainsItsOriginalDispatchedGeneration();
    void reconnectsAutomaticallyAfterProlongedServerOutage();
private:
    void startRelay(quint16 port = 0);
    void configure(WebSocketClient& peer, const QString& name);
    void command(QJsonObject body);
    QProcess m_relay;
    QByteArray m_output;
    QString m_url;
    int m_command = 0;
};

void RemoteSessionIntegrationTest::init()
{
    m_command = 0;
    m_relay.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_relay, &QProcess::readyReadStandardOutput, this, [this]() {
        m_output += m_relay.readAllStandardOutput();
    });
    startRelay();
}

void RemoteSessionIntegrationTest::startRelay(quint16 port)
{
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    QVERIFY2(!node.isEmpty(), "Install Node.js to run the real Qt/Node protocol tests");
    m_output.clear();
    m_relay.start(node, {QStringLiteral(MOUFFETTE_RELAY_FIXTURE), QString::number(port)});
    QVERIFY(m_relay.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(m_output.contains("TEST_READY "), 5000);
    const QRegularExpression expression(QStringLiteral("TEST_READY (\\d+)"));
    const auto match = expression.match(QString::fromUtf8(m_output));
    QVERIFY2(match.hasMatch(), m_output.constData());
    m_url = QStringLiteral("ws://127.0.0.1:%1").arg(match.captured(1));
}

void RemoteSessionIntegrationTest::reconnectsAutomaticallyAfterProlongedServerOutage()
{
    QTemporaryDir identityDirectory;
    QVERIFY(identityDirectory.isValid());
    WebSocketClient client(identityDirectory.path(), false);
    ConnectionManager manager(&client);
    configure(client, QStringLiteral("outage-test"));
    QSignalSpy errors(&manager, &ConnectionManager::connectionError);
    QSignalSpy fatal(&client, &WebSocketClient::fatalError);
    QSignalSpy restarted(&client, &WebSocketClient::serverRestarted);
    QSignalSpy authenticated(&client, &WebSocketClient::connected);
    manager.connectToServer(m_url);
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ConnectionManager::State::Connected, 4000);
    QVERIFY(client.hasUnexpiredLease());
    const QString originalBootId = client.serverBootId();
    const quint16 port = static_cast<quint16>(QUrl(m_url).port());

    // Kill the real server, including all sockets. Expiration of the old lease
    // must not clear the network intent or the background retry loop.
    m_relay.kill();
    QVERIFY(m_relay.waitForFinished(2000));
    QTRY_VERIFY_WITH_TIMEOUT(!client.hasUnexpiredLease(), 6000);
    for (int attempt = 0; attempt < 32; ++attempt) {
        const qsizetype failures = errors.count();
        // Accelerate only the waiting between these genuine refused TCP
        // connections; the production attempt/error scheduling is unchanged.
        QVERIFY(QMetaObject::invokeMethod(&manager, "attemptReconnect", Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(errors.count() > failures, 2000);
        QCOMPARE(manager.state(), ConnectionManager::State::Reconnecting);
        QVERIFY(manager.connectionEnabled());
    }
    startRelay(port);
    // No Enable, connectToServer or forced retry after the server returns.
    // Wait for the actual capped timer (30 seconds, plus up to 20% jitter).
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ConnectionManager::State::Connected, 40000);
    QCOMPARE(authenticated.count(), 2);
    QCOMPARE(restarted.count(), 1);
    QVERIFY(client.serverBootId() != originalBootId);
    QVERIFY(fatal.isEmpty());

    // A voluntary Disable during a later outage still overrides that intent.
    m_relay.kill();
    QVERIFY(m_relay.waitForFinished(2000));
    QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(), 1000);
    manager.setConnectionEnabled(false);
    manager.completeDisconnect(manager.transitionId());
    startRelay(port);
    QTest::qWait(1200);
    QCOMPARE(manager.state(), ConnectionManager::State::Disconnected);
    QCOMPARE(authenticated.count(), 2);
    manager.setConnectionEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ConnectionManager::State::Connected, 4000);
    QCOMPARE(authenticated.count(), 3);
    manager.disconnect();
}

void RemoteSessionIntegrationTest::cleanup()
{
    m_relay.write("{\"action\":\"shutdown\"}\n");
    if (!m_relay.waitForFinished(2000)) {
        m_relay.kill();
        m_relay.waitForFinished();
    }
    disconnect(&m_relay, nullptr, this, nullptr);
}

void RemoteSessionIntegrationTest::command(QJsonObject body)
{
    const QString id = QString::number(++m_command);
    body.insert(QStringLiteral("id"), id);
    m_relay.write(QJsonDocument(body).toJson(QJsonDocument::Compact) + '\n');
    QTRY_VERIFY_WITH_TIMEOUT(m_output.contains("TEST_DONE " + id.toUtf8() + '\n'), 2000);
}

void RemoteSessionIntegrationTest::configure(WebSocketClient& peer, const QString& name)
{
    const auto advertise = [&peer, name]() {
        peer.registerClient(name, QStringLiteral("integration-test"), {}, 50);
    };
    connect(&peer, &WebSocketClient::connected, &peer, advertise);
    connect(&peer, &WebSocketClient::localDeviceSnapshotRequested, &peer, advertise);
    // This test has no renderer or uploaded cache. Its empty target can prove
    // teardown immediately, using the real authenticated ACK path.
    connect(&peer, &WebSocketClient::remoteSessionTerminating, &peer,
            [&peer](const QJsonObject& event) {
        if (event.value("targetEndpointId").toString() == peer.endpointId())
            peer.acknowledgeRemoteSessionTeardown(event.value("remoteSessionId").toString(),
                event.value("teardownId").toString(), true, true, true, 0, QString(), 0);
    });
}

void RemoteSessionIntegrationTest::recovery_data()
{
    QTest::addColumn<int>("gapMs");
    QTest::addColumn<bool>("dropBoth");
    QTest::addColumn<bool>("reverseOrder");
    QTest::addColumn<int>("lostReply");
    QTest::newRow("two-seconds") << 2000 << false << false << 0;
    QTest::newRow("four-seconds-past-transport-timeout") << 4000 << false << false << 0;
    QTest::newRow("six-seconds-terminal") << 6000 << false << false << 0;
    QTest::newRow("both-owner-first") << 100 << true << false << 0;
    QTest::newRow("both-target-first") << 100 << true << true << 0;
    QTest::newRow("lost-resume-owner") << 100 << true << false << 1;
    QTest::newRow("lost-resume-target") << 100 << true << false << 2;
}

void RemoteSessionIntegrationTest::recovery()
{
    QFETCH(int, gapMs);
    QFETCH(bool, dropBoth);
    QFETCH(bool, reverseOrder);
    QFETCH(int, lostReply);
    QTemporaryDir identities;
    WebSocketClient owner(identities.filePath("owner"), false);
    WebSocketClient target(identities.filePath("target"), false);
    configure(owner, QStringLiteral("owner"));
    configure(target, QStringLiteral("target"));
    QSignalSpy ownerRegistered(&owner, &WebSocketClient::registrationConfirmed);
    QSignalSpy targetRegistered(&target, &WebSocketClient::registrationConfirmed);
    QSignalSpy expired(&owner, &WebSocketClient::remoteSessionRecoveryExpired);
    QSignalSpy opened(&owner, &WebSocketClient::remoteSessionOpened);
    QSignalSpy errors(&owner, &WebSocketClient::remoteSessionError);
    owner.connectToServer(m_url);
    target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(!ownerRegistered.isEmpty() && !targetRegistered.isEmpty(), 4000);
    QString request;
    QVERIFY(owner.openRemoteSession(target.endpointId(), &request));
    QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 4000);
    const QString id = opened.first().first().toJsonObject().value("remoteSessionId").toString();
    QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(id) && target.canIssueSessionCommands(id), 3000);
    const quint64 oldGeneration = owner.remoteSessionCoordinator()->byId(id).generation;
    QJsonArray endpoints{owner.endpointId()};
    if (dropBoth) endpoints.append(target.endpointId());
    command({{"action", "drop"}, {"endpoints", endpoints}});
    QTRY_VERIFY_WITH_TIMEOUT(!owner.isConnected(), 1000);
    if (dropBoth) QTRY_VERIFY_WITH_TIMEOUT(!target.isConnected(), 1000);
    QVERIFY(!owner.canIssueSessionCommands(id));
    QTest::qWait(gapMs);
    if (gapMs >= 5000) {
        QVERIFY(!expired.isEmpty());
        QVERIFY(!owner.canIssueSessionCommands(id));
    }
    if (lostReply) command({{"action", "dropFrame"},
        {"endpoint", lostReply == 1 ? owner.endpointId() : target.endpointId()},
        {"type", "remote_session_resumed"}});
    if (reverseOrder && dropBoth) target.connectToServer(m_url);
    owner.connectToServer(m_url);
    if (!reverseOrder && dropBoth) target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && target.isConnected(), 3000);
    if (gapMs >= 5000) {
        QTest::qWait(500);
        QVERIFY(!owner.canIssueSessionCommands(id));
        QCOMPARE(opened.count(), 1); // no implicit new OPEN or scene launch
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(id) && target.canIssueSessionCommands(id), 3000);
        QCOMPARE(owner.remoteSessionCoordinator()->byId(id).remoteSessionId, id);
        QCOMPARE(owner.remoteSessionCoordinator()->byId(id).generation,
                 target.remoteSessionCoordinator()->byId(id).generation);
        QVERIFY(owner.remoteSessionCoordinator()->byId(id).generation > oldGeneration);
        QVERIFY2(expired.isEmpty(), m_output.constData());
        for (const auto& error : errors)
            QVERIFY(error.first().toJsonObject().value("code").toString()
                != QStringLiteral("stale_remote_session_generation"));
    }
    owner.disconnect();
    target.disconnect();
}

void RemoteSessionIntegrationTest::appliedBarrierRecoversLostAcknowledgement_data()
{
    QTest::addColumn<bool>("loseOwner");
    QTest::addColumn<bool>("loseAck");
    QTest::newRow("owner-ack-lost") << true << true;
    QTest::newRow("target-ack-lost") << false << true;
    QTest::newRow("owner-ready-notification-lost") << true << false;
    QTest::newRow("target-ready-notification-lost") << false << false;
}

void RemoteSessionIntegrationTest::appliedBarrierRecoversLostAcknowledgement()
{
    QFETCH(bool, loseOwner);
    QFETCH(bool, loseAck);
    QTemporaryDir identities;
    WebSocketClient owner(identities.filePath("owner"), false);
    WebSocketClient target(identities.filePath("target"), false);
    configure(owner, QStringLiteral("owner"));
    configure(target, QStringLiteral("target"));
    QSignalSpy registrations(&target, &WebSocketClient::registrationConfirmed);
    QSignalSpy opened(&owner, &WebSocketClient::remoteSessionOpened);
    QSignalSpy expired(&owner, &WebSocketClient::remoteSessionRecoveryExpired);
    owner.connectToServer(m_url);
    target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && !registrations.isEmpty(), 4000);
    const QString endpoint = loseOwner ? owner.endpointId() : target.endpointId();
    const QString type = loseAck ? QStringLiteral("remote_session_state_ack")
                                 : QStringLiteral("remote_session_lease_state");
    command({{"action", loseAck ? "dropIncoming" : "dropFrame"},
             {"endpoint", endpoint}, {"type", type}, {"minimumRevision", 2}});
    QVERIFY(owner.openRemoteSession(target.endpointId()));
    QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 3000);
    const QString id = opened.first().first().toJsonObject().value("remoteSessionId").toString();
    QTRY_VERIFY_WITH_TIMEOUT(m_output.contains("TEST_DROPPED " + endpoint.toUtf8() + ':' + type.toUtf8()), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(id) && target.canIssueSessionCommands(id), 3000);
    QVERIFY(expired.isEmpty());
    QCOMPARE(opened.count(), 1);
    owner.disconnect();
    target.disconnect();
}

void RemoteSessionIntegrationTest::localProofExpiryClosesAStillHealthyServerSession()
{
    QTemporaryDir identities;
    WebSocketClient owner(identities.filePath("owner"), false);
    WebSocketClient target(identities.filePath("target"), false);
    configure(owner, QStringLiteral("owner"));
    configure(target, QStringLiteral("target"));
    QSignalSpy registrations(&target, &WebSocketClient::registrationConfirmed);
    QSignalSpy opened(&owner, &WebSocketClient::remoteSessionOpened);
    QSignalSpy expired(&owner, &WebSocketClient::remoteSessionRecoveryExpired);
    QSignalSpy closed(&owner, &WebSocketClient::remoteSessionClosed);
    owner.connectToServer(m_url);
    target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && !registrations.isEmpty(), 4000);
    QVERIFY(owner.openRemoteSession(target.endpointId()));
    QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 3000);
    const QString id = opened.first().first().toJsonObject().value("remoteSessionId").toString();
    QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(id) && target.canIssueSessionCommands(id), 3000);
    command({{"action", "suppressSessionProof"}, {"endpoint", owner.endpointId()}});
    QTRY_COMPARE_WITH_TIMEOUT(expired.count(), 1, 6500);
    // Generic transport ACKs kept both sockets fresh, but they could not renew
    // the withheld session proof. Local expiry must converge the relay too.
    QVERIFY(owner.isConnected());
    QVERIFY(target.isConnected());
    QTRY_VERIFY_WITH_TIMEOUT(!closed.isEmpty(), 2500);
    QCOMPARE(closed.first().first().toJsonObject().value("remoteSessionId").toString(), id);
    QVERIFY(owner.remoteSessionCoordinator()->byId(id).remoteSessionId.isEmpty());
    QVERIFY(!owner.canIssueSessionCommands(id));
    owner.disconnect();
    target.disconnect();
}

void RemoteSessionIntegrationTest::uploadResumesFromDurableOffsetAfterTransportLoss()
{
    QTemporaryDir directory;
    FileManager sourceFiles;
    FileManager receivedFiles;
    WebSocketClient owner(directory.filePath("owner"), false);
    WebSocketClient target(directory.filePath("target"), false);
    UploadManager sending(&sourceFiles, nullptr, directory.filePath("owner-cache"));
    UploadManager receiving(&receivedFiles, nullptr, directory.filePath("target-cache"));
    RemoteSceneController renderer(&sourceFiles, &target);
    sending.setWebSocketClient(&target);
    receiving.setWebSocketClient(&owner);
    sending.setMyClientId(target.endpointId());
    receiving.setMyClientId(owner.endpointId());
    configure(owner, QStringLiteral("owner"));
    configure(target, QStringLiteral("target"));
    QSignalSpy registrations(&target, &WebSocketClient::registrationConfirmed);
    QSignalSpy opened(&owner, &WebSocketClient::remoteSessionOpened);
    QSignalSpy finished(&sending, &UploadManager::uploadFinished);
    QSignalSpy rejected(&sending, &UploadManager::uploadRejected);
    QSignalSpy uploadMessages(&target, &WebSocketClient::uploadMessageReceived);
    owner.connectToServer(m_url);
    target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && !registrations.isEmpty(), 4000);
    QVERIFY(owner.openRemoteSession(target.endpointId()));
    QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 3000);
    const QString sessionId = opened.first().first().toJsonObject().value("remoteSessionId").toString();
    QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(sessionId) && target.canIssueSessionCommands(sessionId), 3000);
    sending.setTargetClientId(owner.endpointId());
    // The opposite direction is an independent RemoteSession: scene assets
    // stay immutable while the same two endpoints transfer unrelated media.
    QSignalSpy uploadSessionOpened(&target, &WebSocketClient::remoteSessionOpened);
    QVERIFY(target.openRemoteSession(owner.endpointId()));
    QTRY_VERIFY_WITH_TIMEOUT(!uploadSessionOpened.isEmpty(), 3000);
    const QString uploadSessionId = uploadSessionOpened.first().first().toJsonObject().value("remoteSessionId").toString();
    QTRY_VERIFY_WITH_TIMEOUT(target.canIssueSessionCommands(uploadSessionId)
                            && owner.canIssueSessionCommands(uploadSessionId), 3000);

    // The owner supplies its local presentation barrier; the recipient uses
    // the real Qt renderer, compositor frame barrier, and server SceneRun.
    const QJsonObject scene = liveTextScene();
    QString runId;
    QSignalSpy sceneErrors(&owner, &WebSocketClient::sceneErrorReceived);
    connect(&owner, &WebSocketClient::scenePreparedReceived, &owner,
            [&owner](const QJsonObject& event) {
        if (event.value("allPrepared").toBool())
            owner.sendSceneArmed(event.value("sceneRunId").toString(), 0);
    });
    connect(&owner, &WebSocketClient::sceneCommitReceived, &owner,
            [&owner](const QJsonObject& event) {
        const qint64 delay = event.value("startServerMonotonicMs").toInteger()
            - owner.estimatedServerMonotonicMs();
        QTimer::singleShot(static_cast<int>(qMax<qint64>(0, delay)), &owner, [&owner, event]() {
            owner.sendSceneStarted(event.value("sceneRunId").toString(), true,
                                   owner.estimatedServerMonotonicMs());
        });
    });
    connect(&owner, &WebSocketClient::sceneStopReceived, &owner,
            [&owner](const QJsonObject& event) {
        owner.sendSceneStopped(event.value("sceneRunId").toString(), true);
    });
    QTRY_VERIFY_WITH_TIMEOUT(owner.estimatedServerMonotonicMs() >= 0, 2000);
    QVERIFY(owner.sendScenePrepare(target.endpointId(), 1, {}, scene, &runId));
    QVERIFY(owner.sendScenePrepared(runId, true, SceneRunCoordinator::createLocalChecklist(scene)));
    QTRY_VERIFY_WITH_TIMEOUT(owner.sceneRunCoordinator()->run(runId).phase == SceneRunCoordinator::Phase::Live
                            || !sceneErrors.isEmpty(), 7000);
    QVERIFY2(sceneErrors.isEmpty(), sceneErrors.isEmpty() ? "" : qPrintable(
        QString::fromUtf8(QJsonDocument(sceneErrors.first().first().toJsonObject()).toJson(QJsonDocument::Compact))));
    QCOMPARE(owner.sceneRunCoordinator()->run(runId).phase, SceneRunCoordinator::Phase::Live);
    QPointer<QQuickWindow> liveWindow;
    for (QWindow* window : QGuiApplication::topLevelWindows())
        if (window->objectName() == QLatin1String("RemoteScreenWindow_0"))
            liveWindow = qobject_cast<QQuickWindow*>(window);
    QVERIFY(liveWindow);

    QImage pixels(640, 640, QImage::Format_ARGB32);
    quint32 noise = 0x12345678;
    for (int y = 0; y < pixels.height(); ++y) {
        auto row = reinterpret_cast<QRgb*>(pixels.scanLine(y));
        for (int x = 0; x < pixels.width(); ++x) {
            noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
            row[x] = 0xff000000U | (noise & 0xffffffU);
        }
    }
    const QString path = directory.filePath("resume.png");
    QVERIFY(pixels.save(path));
    QFile source(path);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray payload = source.readAll();
    QVERIFY(payload.size() > 1024 * 1024);
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    sourceFiles.registerVerifiedLocalFile(hash, path);
    const QString mediaId = QStringLiteral("12345678-1234-4234-8234-123456789abc");
    sourceFiles.associateMediaWithFile(mediaId, hash);
    command({{"action", "dropAfterUploadProgress"}, {"endpoint", target.endpointId()}});
    QVERIFY(sending.toggleUpload({UploadFileInfo{hash, mediaId, path, "resume.png", "png", payload.size()}}));
    QTRY_VERIFY_WITH_TIMEOUT(m_output.contains("TEST_UPLOAD_CUT "), 7000);
    QTRY_VERIFY_WITH_TIMEOUT(!target.isConnected(), 1500);
    QVERIFY(!target.canIssueSessionCommands(uploadSessionId));
    QVERIFY(finished.isEmpty());
    QTest::qWait(1200);
    QVERIFY(liveWindow);
    QCOMPARE(target.sceneRunCoordinator()->run(runId).phase, SceneRunCoordinator::Phase::Live);
    target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(target.canIssueSessionCommands(uploadSessionId) && owner.canIssueSessionCommands(uploadSessionId), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(sessionId) && target.canIssueSessionCommands(sessionId), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty() || !rejected.isEmpty(), 12000);
    QVERIFY2(rejected.isEmpty(), rejected.isEmpty() ? "" : qPrintable(rejected.first().at(1).toString()));
    QVERIFY(!finished.isEmpty());
    bool resumedFromDurableBytes = false;
    for (const auto& event : uploadMessages) {
        const auto message = event.first().toJsonObject();
        if (message.value("type").toString() != QStringLiteral("upload_ready")
            && message.value("type").toString() != QStringLiteral("upload_resume_ready")) continue;
        for (const auto& asset : message.value("assets").toArray())
            resumedFromDurableBytes |= asset.toObject().value("offset").toInteger() > 0;
    }
    QVERIFY(resumedFromDurableBytes);
    const auto binding = owner.remoteSessionCoordinator()->byId(uploadSessionId);
    const QString receivedPath = receivedFiles.getReceivedFilePath({target.endpointId(), uploadSessionId, binding.generation}, hash);
    QFile received(receivedPath);
    QVERIFY2(received.open(QIODevice::ReadOnly), qPrintable(receivedPath));
    QCOMPARE(QCryptographicHash::hash(received.readAll(), QCryptographicHash::Sha256),
             QCryptographicHash::hash(payload, QCryptographicHash::Sha256));
    QVERIFY(liveWindow);
    QCOMPARE(target.sceneRunCoordinator()->run(runId).phase, SceneRunCoordinator::Phase::Live);
    QVERIFY(owner.sendSceneStop(runId));
    QTRY_VERIFY_WITH_TIMEOUT(liveWindow.isNull(), 2000);
    owner.disconnect();
    target.disconnect();
}

void RemoteSessionIntegrationTest::cleanupReceiptRetainsItsOriginalDispatchedGeneration()
{
    QTemporaryDir directory;
    WebSocketClient owner(directory.filePath("owner"), false);
    WebSocketClient target(directory.filePath("target"), false);
    configure(owner, QStringLiteral("owner"));
    configure(target, QStringLiteral("target"));
    QSignalSpy registrations(&target, &WebSocketClient::registrationConfirmed);
    QSignalSpy opened(&owner, &WebSocketClient::remoteSessionOpened);
    QSignalSpy instructions(&target, &WebSocketClient::uploadMessageReceived);
    owner.connectToServer(m_url);
    target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && !registrations.isEmpty(), 4000);
    QVERIFY(owner.openRemoteSession(target.endpointId()));
    QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 3000);
    const QString id = opened.first().first().toJsonObject().value("remoteSessionId").toString();
    QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(id) && target.canIssueSessionCommands(id), 3000);
    const QString removal = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject instruction{{"action", "testRemovalInstruction"}, {"sessionId", id}, {"removalId", removal}};
    command(instruction);
    QTRY_COMPARE_WITH_TIMEOUT(instructions.count(), 1, 2000);
    QJsonObject oldReceipt = instructions.first().first().toJsonObject();
    command({{"action", "drop"}, {"endpoints", QJsonArray{owner.endpointId()}}});
    QTRY_VERIFY_WITH_TIMEOUT(!owner.isConnected(), 1000);
    owner.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(id) && target.canIssueSessionCommands(id), 3000);
    QVERIFY(target.remoteSessionCoordinator()->byId(id).generation > oldReceipt.value("generation").toInteger());
    command(instruction);
    QTRY_COMPARE_WITH_TIMEOUT(instructions.count(), 2, 2000);
    oldReceipt.insert("type", "upload_removed");
    oldReceipt.insert("success", true);
    oldReceipt.insert("cacheQuarantined", true);
    oldReceipt.insert("result", "committed");
    oldReceipt.insert("removedFileCount", 1);
    oldReceipt.insert("quarantinedBytes", 1);
    QJsonObject forged = oldReceipt;
    forged.insert("size", 2);
    QVERIFY(!target.sendUploadProtocolResponse(forged));
    QVERIFY(target.sendUploadProtocolResponse(oldReceipt));
    // Receipt authority is consumed after send; a server retry installs a
    // fresh obligation if the ACK was lost, instead of retaining pending work forever.
    QVERIFY(!target.sendUploadProtocolResponse(oldReceipt));
    owner.disconnect();
    target.disconnect();
}

void RemoteSessionIntegrationTest::duplicateOpenAndMetadataRefresh()
{
    QTemporaryDir identities;
    WebSocketClient owner(identities.filePath("owner"), false);
    WebSocketClient target(identities.filePath("target"), false);
    configure(owner, QStringLiteral("owner"));
    configure(target, QStringLiteral("target"));
    QSignalSpy registrations(&target, &WebSocketClient::registrationConfirmed);
    QSignalSpy opened(&owner, &WebSocketClient::remoteSessionOpened);
    QSignalSpy terminating(&owner, &WebSocketClient::remoteSessionTerminating);
    owner.connectToServer(m_url);
    target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && !registrations.isEmpty(), 4000);
    QString request;
    QVERIFY(owner.openRemoteSession(target.endpointId(), &request));
    QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 3000);
    const QString id = opened.first().first().toJsonObject().value("remoteSessionId").toString();
    QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(id), 3000);
    QVERIFY(owner.replayRemoteSessionOpen(target.endpointId(), request));
    target.registerClient(QStringLiteral("target"), QStringLiteral("integration-test"), {}, 55);
    QTest::qWait(500);
    QVERIFY(owner.canIssueSessionCommands(id));
    QVERIFY(terminating.isEmpty());
    QVERIFY(owner.closeRemoteSession(id));
    QTRY_VERIFY_WITH_TIMEOUT(owner.remoteSessionCoordinator()->byId(id).remoteSessionId.isEmpty(), 3000);
    const int oldTerminations = terminating.count();
    owner.registerClient(QStringLiteral("owner"), QStringLiteral("integration-test"), {}, 57);
    QTest::qWait(300);
    QCOMPARE(terminating.count(), oldTerminations);
    owner.disconnect();
    target.disconnect();
}

QTEST_MAIN(RemoteSessionIntegrationTest)
#include "tst_RemoteSessionIntegration.moc"
