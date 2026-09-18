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
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/domain/project/ProjectManager.h"
#include <QScopeGuard>
#include "../fixtures/MultiInstanceProtocolWorker.h"
#include <QDir>
#include <memory>
#include <vector>
#include <algorithm>

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

    SceneTimeline::ElementState state;
    state.type = QStringLiteral("text");
    state.text = QStringLiteral("teardown-test");
    state.fontFamily = QStringLiteral("Arial");
    state.fontPixelSize = 20;
    state.fitToText = false;
    state.size = state.baseSize = QSizeF(320,180);
    QJsonObject media = state.toJson();
    media[QStringLiteral("mediaId")] = QStringLiteral("text-1");
    media[QStringLiteral("fileId")] = QString();
    media[QStringLiteral("fileName")] = QString();
    media[QStringLiteral("spans")] = QJsonArray{span};
    media[QStringLiteral("timeline")] = SceneTimeline::MediaTrack{}.toJson();

    QJsonObject scene;
    scene[QStringLiteral("renderSchemaVersion")] = SceneTimeline::RenderSchemaVersion;
    scene[QStringLiteral("timeline")] = SceneTimeline::SceneSettings{}.toJson();
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
    void pendingRequestsRecover_data();
    void pendingRequestsRecover();
    void synchronizationTimeoutKeepsRetrying();
    void degradedSessionPresentation_data();
    void degradedSessionPresentation();
    void recovery_data();
    void recovery();
    void duplicateOpenAndMetadataRefresh();
    void appliedBarrierRecoversLostAcknowledgement_data();
    void appliedBarrierRecoversLostAcknowledgement();
    void localProofExpiryClosesAStillHealthyServerSession();
    void uploadResumesFromDurableOffsetAfterTransportLoss();
    void cleanupReceiptRetainsItsOriginalDispatchedGeneration();
    void reconnectsAutomaticallyAfterProlongedServerOutage();
    void concurrentProcessesShareInstallationAndKeepStableSlots();
    void retainedServerPresenceDoesNotKeepProjectlessRows();
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

void RemoteSessionIntegrationTest::retainedServerPresenceDoesNotKeepProjectlessRows()
{
    QTemporaryDir runtimeDirectory;
    QTemporaryDir targetIdentity;
    const auto previousProfile = RuntimeProfile::context();
    const auto restoreProfile = qScopeGuard([&] { RuntimeProfile::configure(previousProfile); });
    RuntimeProfileContext profile;
    profile.rootPath = runtimeDirectory.path();
    profile.persistent = false;
    RuntimeProfile::configure(profile);
    ApplicationRuntime observer(profile);
    observer.getProjectManager()->stopAutomaticTimersForTesting();
    auto* observerConnection = observer.findChild<ConnectionManager*>();
    QVERIFY(observerConnection);
    WebSocketClient target(targetIdentity.path(), false);
    ConnectionManager targetConnection(&target);
    configure(target, QStringLiteral("discovery-target"));
    connect(&targetConnection, &ConnectionManager::disconnectRequested, &target,
            [&](quint64 transition) {
        target.beginEndpointDisable(QStringLiteral("disable-%1").arg(transition));
    });
    connect(&target, &WebSocketClient::endpointDisableAcknowledged, &targetConnection,
            [&](const QString&, quint64) {
        targetConnection.completeDisconnect(targetConnection.transitionId());
    });
    QList<ClientInfo> rawPresence;
    connect(observer.getWebSocketClient(), &WebSocketClient::clientListReceived,
            &observer, [&](const QList<ClientInfo>& clients) { rawPresence = clients; });
    const auto targetIsRetainedOffline = [&] {
        return std::any_of(rawPresence.begin(), rawPresence.end(), [&](const ClientInfo& peer) {
            return peer.endpointId() == target.endpointId()
                && peer.availabilityBadgeText() == QLatin1String("Disconnected");
        });
    };
    observerConnection->connectToServer(m_url);
    targetConnection.connectToServer(m_url);
    QTRY_COMPARE_WITH_TIMEOUT(observerConnection->state(), ConnectionManager::State::Connected, 4000);
    QTRY_COMPARE_WITH_TIMEOUT(targetConnection.state(), ConnectionManager::State::Connected, 4000);
    QTRY_COMPARE_WITH_TIMEOUT(observer.displayClients().size(), 1, 4000);
    QCOMPARE(observer.getProjectManager()->projectCount(), 0);

    targetConnection.setConnectionEnabled(false);
    QTRY_VERIFY_WITH_TIMEOUT(targetIsRetainedOffline(), 4000);
    QVERIFY(observer.displayClients().isEmpty());
    observer.activateClient(target.endpointId()); // a delayed click cannot open it
    QVERIFY(!observer.findWorkspace(target.endpointId()));
    QVERIFY(observer.getWebSocketClient()->remoteSessionCoordinator()->all().isEmpty());

    QTRY_COMPARE(targetConnection.state(), ConnectionManager::State::Disconnected);
    targetConnection.setConnectionEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(observer.displayClients().size(), 1, 4000);
    QCOMPARE(observer.getProjectManager()->projectCount(), 0);
    observer.activateClient(target.endpointId());
    QTRY_VERIFY_WITH_TIMEOUT(observer.getProjectManager()->hasProjectForTarget(target.endpointId()), 4000);
    observer.navigateToClients();

    targetConnection.setConnectionEnabled(false);
    QTRY_VERIFY_WITH_TIMEOUT(targetIsRetainedOffline(), 4000);
    QCOMPARE(observer.displayClients().size(), 1);
    QVERIFY(observer.displayClients().first().hasProject());
    QCOMPARE(observer.displayClients().first().endpointId(), target.endpointId());
    QVERIFY(observer.getProjectManager()->deleteProject(target.endpointId()));
    QVERIFY(observer.displayClients().isEmpty());
    QVERIFY(targetIsRetainedOffline()); // visibility did not erase wire presence

    QTRY_COMPARE(targetConnection.state(), ConnectionManager::State::Disconnected);
    targetConnection.setConnectionEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(observer.displayClients().size(), 1, 4000);
    observer.setConnectionEnabled(false);
    QVERIFY(observer.displayClients().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(observerConnection->state(), ConnectionManager::State::Disconnected, 4000);
    observer.handleApplicationAboutToQuit();
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
        QCOMPARE(manager.state(), ConnectionManager::State::Disconnected);
        QVERIFY(manager.connectionEnabled());
    }
    startRelay(port);
    // No Enable, connectToServer or forced retry after the server returns.
    // Wait for the actual hard-capped five-second timer.
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ConnectionManager::State::Connected, 8000);
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

void RemoteSessionIntegrationTest::pendingRequestsRecover_data()
{
    QTest::addColumn<QString>("operation");
    QTest::addColumn<bool>("loseResponse");
    for (const QString& operation : {QStringLiteral("registration"), QStringLiteral("reconciliation"),
                                    QStringLiteral("open"), QStringLiteral("close"), QStringLiteral("disable"), QStringLiteral("upload")}) {
        QTest::newRow(qPrintable(operation + "-request")) << operation << false;
        QTest::newRow(qPrintable(operation + "-response")) << operation << true;
    }
}

void RemoteSessionIntegrationTest::pendingRequestsRecover()
{
    QFETCH(QString, operation);
    QFETCH(bool, loseResponse);
    const AppConfig previousConfig = AppConfig::instance();
    const auto restoreConfig = qScopeGuard([&] { AppConfig::instance() = previousConfig; });
    AppConfig::LoadOptions options;
    options.defaultEnvFilePath.clear();
    options.processEnvironment.insert("MOUFFETTE_UPLOAD_CHANNEL_ATTEMPT_TIMEOUT_MS", "1000");
    options.processEnvironment.insert("MOUFFETTE_UPLOAD_CHANNEL_RETRY_BASE_MS", "100");
    options.processEnvironment.insert("MOUFFETTE_UPLOAD_CHANNEL_RETRY_MAX_MS", "200");
    QString configurationError;
    QVERIFY2(AppConfig::instance().load(options, &configurationError), qPrintable(configurationError));
    QTemporaryDir identities;
    WebSocketClient owner(identities.filePath("owner"), false);
    WebSocketClient target(identities.filePath("target"), false);
    ConnectionManager connection(&owner);
    configure(owner, QStringLiteral("retry-owner"));
    configure(target, QStringLiteral("retry-target"));
    QSignalSpy states(&connection, &ConnectionManager::statusChanged);
    QSignalSpy opened(&owner, &WebSocketClient::remoteSessionOpened);
    QSignalSpy closed(&owner, &WebSocketClient::remoteSessionClosed);
    QSignalSpy disabled(&owner, &WebSocketClient::endpointDisableAcknowledged);
    QSignalSpy targetRegistered(&target, &WebSocketClient::registrationConfirmed);
    const QMap<QString, QPair<QString, QString>> types{
        {"registration", {"endpoint_snapshot", "endpoint_snapshot_applied"}},
        {"reconciliation", {"remote_session_reconcile", "remote_session_reconciled"}},
        {"open", {"remote_session_open", "remote_session_opened"}},
        {"close", {"remote_session_close", "remote_session_closed"}},
        {"disable", {"endpoint_disable", "endpoint_disable_started"}},
        {"upload", {"request_upload_channel", "upload_channel_token"}}
    };
    const QString type = loseResponse ? types.value(operation).second : types.value(operation).first;
    command({{"action", loseResponse ? "dropRawFrame" : "dropIncoming"},
             {"endpoint", owner.endpointId()}, {"type", type}, {"count", operation == "upload" ? 3 : 1}});
    QElapsedTimer attemptClock;
    attemptClock.start();
    connection.connectToServer(m_url);
    target.connectToServer(m_url);
    QTRY_COMPARE_WITH_TIMEOUT(connection.state(), ConnectionManager::State::Connected, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(!targetRegistered.isEmpty(), 4000);
    if (operation == "open" || operation == "close") {
        QString request;
        QVERIFY(owner.openRemoteSession(target.endpointId(), &request));
        QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 4000);
        const QString id = opened.first().first().toJsonObject().value("remoteSessionId").toString();
        QTRY_VERIFY_WITH_TIMEOUT(owner.canIssueSessionCommands(id), 3000);
        if (operation == "close") {
            QVERIFY(owner.closeRemoteSession(id));
            QTRY_VERIFY_WITH_TIMEOUT(!closed.isEmpty(), 4000);
            QVERIFY(!owner.canIssueSessionCommands(id));
        }
    } else if (operation == "upload") {
        QTRY_VERIFY_WITH_TIMEOUT(owner.isUploadChannelConnected(), 6000);
        QVERIFY(attemptClock.elapsed() >= 3000); // each lost token waits for its attempt deadline
        QCOMPARE(m_output.count("TEST_DROPPED " + owner.endpointId().toUtf8() + ':' + type.toUtf8()), 3);
        QVERIFY(connection.isReady());
    } else if (operation == "disable") {
        QVERIFY(owner.beginEndpointDisable());
        QTRY_VERIFY_WITH_TIMEOUT(!disabled.isEmpty(), 3000);
    }
    QTRY_VERIFY_WITH_TIMEOUT(m_output.contains("TEST_DROPPED " + owner.endpointId().toUtf8() + ':' + type.toUtf8()), 3000);
    for (const auto& state : states) {
        QVERIFY(state.first().toString() != QStringLiteral("Reconnecting"));
        QVERIFY(state.first().toString() != QStringLiteral("Authenticating"));
        QVERIFY(state.first().toString() != QStringLiteral("Synchronizing"));
    }
    connection.disconnect();
    target.disconnect();
}

void RemoteSessionIntegrationTest::synchronizationTimeoutKeepsRetrying()
{
    const AppConfig previous = AppConfig::instance();
    const auto restore = qScopeGuard([&] { AppConfig::instance() = previous; });
    AppConfig::LoadOptions options;
    options.defaultEnvFilePath.clear();
    options.processEnvironment.insert("MOUFFETTE_CONNECTION_SYNC_TIMEOUT_MS", "1000");
    QString error;
    QVERIFY2(AppConfig::instance().load(options, &error), qPrintable(error));
    QTemporaryDir identity;
    WebSocketClient client(identity.path(), false);
    ConnectionManager connection(&client);
    configure(client, "sync-timeout");
    QSignalSpy connected(&client, &WebSocketClient::connected);
    command({{"action", "dropRawFrame"}, {"endpoint", client.endpointId()},
             {"type", "endpoint_snapshot_applied"}, {"count", 100}});
    connection.connectToServer(m_url);
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(connection.state(), ConnectionManager::State::Synchronizing, 1000);
    QCOMPARE(connection.getConnectionStatus(), QStringLiteral("Connecting"));
    QTRY_COMPARE_WITH_TIMEOUT(connection.state(), ConnectionManager::State::Disconnected, 2000);
    QCOMPARE(connection.retryAction(), RetryAction::Scheduled);
    QVERIFY(connection.connectionDetail().contains("synchronization"));
    command({{"action", "dropRawFrame"}, {"endpoint", client.endpointId()},
             {"type", "endpoint_snapshot_applied"}, {"count", -1}});
    QTRY_VERIFY_WITH_TIMEOUT(connection.isReady(), 4000);
    QCOMPARE(connected.count(), 2);
    connection.disconnect();
}

void RemoteSessionIntegrationTest::degradedSessionPresentation_data()
{
    QTest::addColumn<bool>("recover");
    QTest::newRow("returns-before-deadline") << true;
    QTest::newRow("expires-after-deadline") << false;
}

void RemoteSessionIntegrationTest::degradedSessionPresentation()
{
    QFETCH(bool, recover);
    QTemporaryDir directory;
    const auto previousProfile = RuntimeProfile::context();
    const auto restore = qScopeGuard([&] { RuntimeProfile::configure(previousProfile); });
    RuntimeProfileContext profile;
    profile.rootPath = directory.filePath("observer");
    profile.persistent = false;
    RuntimeProfile::configure(profile);
    ApplicationRuntime observer(profile);
    observer.getProjectManager()->stopAutomaticTimersForTesting();
    auto* connection = observer.findChild<ConnectionManager*>();
    auto* owner = observer.getWebSocketClient();
    WebSocketClient target(directory.filePath("target"), false);
    configure(target, QStringLiteral("retained-target"));
    connection->connectToServer(m_url);
    target.connectToServer(m_url);
    QTRY_COMPARE_WITH_TIMEOUT(observer.displayClients().size(), 1, 4000);
    observer.activateClient(target.endpointId());
    QTRY_VERIFY_WITH_TIMEOUT(observer.activeProjectExists(), 4000);
    const QString id = owner->remoteSessionCoordinator()->outgoingForPeer(target.endpointId()).remoteSessionId;
    QTRY_VERIFY_WITH_TIMEOUT(owner->canIssueSessionCommands(id) && target.canIssueSessionCommands(id), 3000);
    auto* canvas = observer.getActiveCanvas();
    QVERIFY(canvas);
    command({{"action", "drop"}, {"endpoints", QJsonArray{target.endpointId()}}});
    QTRY_VERIFY_WITH_TIMEOUT(!target.isConnected(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(observer.remoteStatusText(), QStringLiteral("DEGRADED"), 1000);
    QCOMPARE(observer.displayClients().size(), 1);
    QCOMPARE(observer.displayClients().first().availabilityBadgeText(), QStringLiteral("Degraded"));
    QVERIFY(observer.activeProjectExists());
    QCOMPARE(observer.getActiveCanvas(), canvas);
    QVERIFY(!observer.isRemoteOverlayActionsEnabled());
    QTest::qWait(1000);
    QVERIFY(owner->sessionRecoveryRemainingMs(id) > 0);
    QCOMPARE(observer.remoteStatusText(), QStringLiteral("DEGRADED"));
    if (recover) {
        target.connectToServer(m_url);
        QTRY_VERIFY_WITH_TIMEOUT(owner->canIssueSessionCommands(id) && target.canIssueSessionCommands(id), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(observer.remoteStatusText(), QStringLiteral("CONNECTED"), 1000);
        QCOMPARE(owner->remoteSessionCoordinator()->outgoingForPeer(target.endpointId()).remoteSessionId, id);
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(owner->sessionRecoveryRemainingMs(id) == 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(observer.remoteStatusText(), QStringLiteral("DISCONNECTED"), 1000);
        QCOMPARE(observer.displayClients().size(), 1);
        QCOMPARE(observer.displayClients().first().availabilityBadgeText(), QStringLiteral("Disconnected"));
        QVERIFY(observer.activeProjectExists());
        QCOMPARE(observer.getActiveCanvas(), canvas);
    }
    target.disconnect();
    observer.handleApplicationAboutToQuit();
}

void RemoteSessionIntegrationTest::recovery_data()
{
    QTest::addColumn<int>("gapMs");
    QTest::addColumn<bool>("dropBoth");
    QTest::addColumn<bool>("reverseOrder");
    QTest::addColumn<int>("lostReply");
    QTest::newRow("two-seconds") << 2000 << false << false << 0;
    QTest::newRow("four-seconds-terminal") << 4000 << false << false << 0;
    QTest::newRow("six-seconds-terminal") << 6000 << false << false << 0;
    QTest::newRow("both-owner-first") << 100 << true << false << 0;
    QTest::newRow("both-target-first") << 100 << true << true << 0;
    QTest::newRow("lost-resume-owner") << 100 << true << false << 1;
    QTest::newRow("lost-resume-target") << 100 << true << false << 2;
    QTest::newRow("lost-resume-request-owner") << 100 << true << false << 3;
    QTest::newRow("lost-resume-request-target") << 100 << true << true << 4;
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
    if (gapMs >= 3000) {
        QVERIFY(!expired.isEmpty());
        QVERIFY(!owner.canIssueSessionCommands(id));
    }
    if (lostReply) command({{"action", lostReply <= 2 ? "dropFrame" : "dropIncoming"},
        {"endpoint", (lostReply == 1 || lostReply == 3) ? owner.endpointId() : target.endpointId()},
        {"type", lostReply <= 2 ? "remote_session_resumed" : "remote_session_resume"}});
    if (reverseOrder && dropBoth) target.connectToServer(m_url);
    owner.connectToServer(m_url);
    if (!reverseOrder && dropBoth) target.connectToServer(m_url);
    QTRY_VERIFY_WITH_TIMEOUT(owner.isConnected() && target.isConnected(), 3000);
    if (gapMs >= 3000) {
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

void RemoteSessionIntegrationTest::concurrentProcessesShareInstallationAndKeepStableSlots()
{
    struct Peer {
        QProcess process;
        QByteArray pending;
        QList<QJsonObject> events;
        ~Peer() { stop(); }
        void stop() {
            if (process.state() == QProcess::NotRunning) return;
            process.closeWriteChannel();
            if (!process.waitForFinished(5000)) {
                process.kill();
                process.waitForFinished(2000);
            }
        }
        QJsonObject last(const QString& event) const {
            for (auto it = events.crbegin(); it != events.crend(); ++it)
                if (it->value("event").toString() == event) return *it;
            return {};
        }
        QJsonObject identity() const { return last(QStringLiteral("identity")); }
        QString endpoint() const { return identity().value("endpointId").toString(); }
        QString state() const { return last(QStringLiteral("state")).value("state").toString(); }
        QJsonObject outgoing(const QString& target) const {
            for (auto it = events.crbegin(); it != events.crend(); ++it)
                if (it->value("event") == QLatin1String("session")
                    && it->value("ownerEndpointId").toString() == endpoint()
                    && it->value("targetEndpointId").toString() == target) return *it;
            return {};
        }
        void send(QJsonObject message) {
            process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        }
    };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    std::vector<std::unique_ptr<Peer>> workers;
    const auto startPeer = [&]() -> Peer* {
        auto peer = std::make_unique<Peer>();
        auto* raw = peer.get();
        connect(&raw->process, &QProcess::readyReadStandardOutput, this, [raw] {
            raw->pending += raw->process.readAllStandardOutput();
            int newline;
            while ((newline = raw->pending.indexOf('\n')) >= 0) {
                const auto event = QJsonDocument::fromJson(raw->pending.left(newline)).object();
                raw->pending.remove(0, newline + 1);
                if (!event.isEmpty()) raw->events.append(event);
            }
        });
        raw->process.start(QCoreApplication::applicationFilePath(),
            {QStringLiteral("--multi-instance-worker"), root.path(), m_url});
        const bool started = raw->process.waitForStarted(5000);
        workers.push_back(std::move(peer));
        return started ? raw : nullptr;
    };
    for (int i = 0; i < 3; ++i) QVERIFY(startPeer());
    for (const auto& peer : workers) peer->process.write("go\n");
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(workers.begin(), workers.end(), [](const auto& peer) {
        return peer->state() == QLatin1String("Connected");
    }), 10000);
    const auto byOrdinal = [&](int ordinal) -> Peer* {
        for (const auto& peer : workers)
            if (peer->process.state() != QProcess::NotRunning
                && peer->identity().value("ordinal").toInt() == ordinal) return peer.get();
        return nullptr;
    };
    Peer* primary = byOrdinal(1);
    Peer* secondary = byOrdinal(2);
    Peer* third = byOrdinal(3);
    QVERIFY(primary && secondary && third);
    const QString installation = primary->identity().value("installationId").toString();
    QVERIFY(!installation.isEmpty());
    QSet<QString> endpoints;
    QSet<QString> directories;
    for (const auto& peer : workers) {
        QCOMPARE(peer->identity().value("installationId").toString(), installation);
        QCOMPARE(peer->identity().value("identityRoot"), primary->identity().value("identityRoot"));
        endpoints.insert(peer->endpoint());
        directories.insert(peer->identity().value("root").toString());
        QTRY_COMPARE_WITH_TIMEOUT(peer->last("clients").value("clients").toArray().size(), 2, 3000);
        for (const auto& value : peer->last("clients").value("clients").toArray()) {
            const auto discovered = value.toObject();
            const int ordinal = discovered.value("instanceOrdinal").toInt();
            QVERIFY(ordinal >= 1 && ordinal <= 3);
            QCOMPARE(discovered.value("installationId").toString(), installation);
            QCOMPARE(discovered.value("displayName").toString(),
                     QStringLiteral("shared-computer (%1)").arg(ordinal));
            QVERIFY(!discovered.value("runtimeId").toString().isEmpty());
        }
    }
    QCOMPARE(endpoints.size(), 3);
    QCOMPARE(directories.size(), 3);

    primary->send({{"action", "open"}, {"target", secondary->endpoint()}});
    secondary->send({{"action", "open"}, {"target", primary->endpoint()}});
    third->send({{"action", "open"}, {"target", primary->endpoint()}});
    QTRY_VERIFY_WITH_TIMEOUT(primary->outgoing(secondary->endpoint()).value("commandReady").toBool(), 4000);
    QTRY_VERIFY_WITH_TIMEOUT(secondary->outgoing(primary->endpoint()).value("commandReady").toBool(), 4000);
    QTRY_VERIFY_WITH_TIMEOUT(third->outgoing(primary->endpoint()).value("commandReady").toBool(), 4000);
    const QString independentSession = third->outgoing(primary->endpoint()).value("remoteSessionId").toString();
    secondary->send({{"action", "disable"}});
    QTRY_COMPARE_WITH_TIMEOUT(secondary->state(), QStringLiteral("Disconnected"), 4000);
    third->send({{"action", "inspect"}, {"id", "independent"}, {"session", independentSession}});
    QTRY_COMPARE_WITH_TIMEOUT(third->last("result").value("id").toString(), QStringLiteral("independent"), 2000);
    QVERIFY(third->last("result").value("ready").toBool());
    QCOMPARE(primary->state(), QStringLiteral("Connected"));

    const auto oldSecondary = secondary->identity();
    secondary->stop();
    QCOMPARE(secondary->process.exitCode(), 0);
    QVERIFY(!QFileInfo::exists(oldSecondary.value("root").toString()));
    secondary = startPeer();
    QVERIFY(secondary);
    secondary->process.write("go\n");
    QTRY_COMPARE_WITH_TIMEOUT(secondary->state(), QStringLiteral("Connected"), 7000);
    QCOMPARE(secondary->identity().value("ordinal").toInt(), 2);
    QCOMPARE(secondary->endpoint(), oldSecondary.value("endpointId").toString());
    QCOMPARE(secondary->identity().value("installationId").toString(), installation);
    QVERIFY(secondary->identity().value("runtimeId") != oldSecondary.value("runtimeId"));
    QVERIFY(secondary->identity().value("root") != oldSecondary.value("root"));
    QVERIFY(secondary->outgoing(primary->endpoint()).isEmpty());

    const auto oldPrimary = primary->identity();
    const QString markerPath = QDir(oldPrimary.value("root").toString()).filePath("persistent-marker");
    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::WriteOnly));
    QCOMPARE(marker.write("persistent"), qint64(10));
    marker.close();
    primary->send({{"action", "disable"}});
    QTRY_COMPARE_WITH_TIMEOUT(primary->state(), QStringLiteral("Disconnected"), 4000);
    primary->stop();
    QVERIFY(QFileInfo::exists(markerPath));
    QCOMPARE(third->state(), QStringLiteral("Connected"));
    primary = startPeer();
    QVERIFY(primary);
    primary->process.write("go\n");
    QTRY_COMPARE_WITH_TIMEOUT(primary->state(), QStringLiteral("Connected"), 7000);
    QCOMPARE(primary->identity().value("ordinal").toInt(), 1);
    QCOMPARE(primary->endpoint(), oldPrimary.value("endpointId").toString());
    QCOMPARE(primary->identity().value("root"), oldPrimary.value("root"));
    QVERIFY(primary->identity().value("runtimeId") != oldPrimary.value("runtimeId"));
    QVERIFY(QFileInfo::exists(markerPath));

    const auto crashed = secondary->identity();
    secondary->process.kill();
    QVERIFY(secondary->process.waitForFinished(2000));
    QVERIFY(QFileInfo::exists(crashed.value("root").toString()));
    secondary = startPeer();
    QVERIFY(secondary);
    secondary->process.write("go\n");
    QTRY_COMPARE_WITH_TIMEOUT(secondary->state(), QStringLiteral("Connected"), 7000);
    QCOMPARE(secondary->identity().value("ordinal").toInt(), 2);
    QCOMPARE(secondary->endpoint(), crashed.value("endpointId").toString());
    QVERIFY(secondary->identity().value("runtimeId") != crashed.value("runtimeId"));
    QVERIFY(!QFileInfo::exists(crashed.value("root").toString()));

    // All enabled processes recover after a server restart. A disabled one
    // stays disconnected despite sharing the installation with them.
    secondary->send({{"action", "disable"}});
    QTRY_COMPARE_WITH_TIMEOUT(secondary->state(), QStringLiteral("Disconnected"), 4000);
    const quint16 port = static_cast<quint16>(QUrl(m_url).port());
    m_relay.kill();
    QVERIFY(m_relay.waitForFinished(2000));
    QTRY_VERIFY_WITH_TIMEOUT(primary->state() != QLatin1String("Connected")
                             && third->state() != QLatin1String("Connected"), 2000);
    startRelay(port);
    QTRY_COMPARE_WITH_TIMEOUT(primary->state(), QStringLiteral("Connected"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(third->state(), QStringLiteral("Connected"), 10000);
    QCOMPARE(secondary->state(), QStringLiteral("Disconnected"));
    QCOMPARE(third->identity().value("ordinal").toInt(), 3);
    for (const auto& peer : workers) QVERIFY(peer->last("failure").isEmpty());
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (app.arguments().value(1) == QLatin1String("--multi-instance-worker"))
        return runMultiInstanceProtocolWorker(app, app.arguments());
    RemoteSessionIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_RemoteSessionIntegration.moc"
