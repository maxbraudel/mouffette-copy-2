#include <QApplication>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QPushButton>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

#include "backend/domain/media/MediaItems.h"
#include "backend/network/WebSocketClient.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.h"

namespace {
QPushButton* findButton(ScreenCanvas& canvas, const QString& text)
{
    for (QPushButton* button : canvas.findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

QString sceneInstanceIdFrom(const QSignalSpy& messages)
{
    for (const QList<QVariant>& arguments : messages) {
        if (arguments.isEmpty()) continue;
        const QJsonObject message = QJsonDocument::fromJson(
            arguments.constFirst().toString().toUtf8()).object();
        if (message.value(QStringLiteral("type")).toString()
            == QLatin1String("remote_scene_start")) {
            return message.value(QStringLiteral("scene"))
                .toObject()
                .value(QStringLiteral("sceneInstanceId"))
                .toString();
        }
    }
    return {};
}
}

class RemoteSceneLifecycleTest final : public QObject {
    Q_OBJECT

private slots:
    void acknowledgedStopRestoresSelectedMediaWithoutReentrantSettingsPush();
};

void RemoteSceneLifecycleTest::acknowledgedStopRestoresSelectedMediaWithoutReentrantSettingsPush()
{
    QWebSocketServer server(QStringLiteral("remote-scene-lifecycle-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    WebSocketClient socket;
    QSignalSpy connected(&socket, &WebSocketClient::connected);
    socket.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
    QScopedPointer<QWebSocket> peer(server.nextPendingConnection());
    QVERIFY(peer);

    ScreenCanvas canvas;
    canvas.resize(960, 600);
    canvas.setScreens({ScreenInfo(1, 1920, 1080, 0, 0, true)});
    canvas.setWebSocketClient(&socket);
    const QString targetId = QStringLiteral("target-client-b");
    canvas.setRemoteSceneTarget(targetId, QStringLiteral("Target B"));

    auto* settingsHost = canvas.findChild<CanvasGlobalOverlayHost*>();
    QVERIFY(settingsHost);
    settingsHost->setSettingsChecked(true);

    QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::magenta);
    auto* media = new ResizablePixmapItem(
        QPixmap::fromImage(image), 8, 16, QStringLiteral("regression.png"));
    media->setFileId(QString(64, QLatin1Char('a')));
    canvas.scene()->addItem(media);
    media->setSelected(true);
    QVERIFY(media->isSelected());

    QPushButton* launch = findButton(canvas, QStringLiteral("Launch Remote Scene"));
    QVERIFY(launch);
    QSignalSpy outbound(peer.data(), &QWebSocket::textMessageReceived);
    launch->click();
    QTRY_VERIFY_WITH_TIMEOUT(!sceneInstanceIdFrom(outbound).isEmpty(), 3000);
    const QString sceneInstanceId = sceneInstanceIdFrom(outbound);

    // Drive the real sender lifecycle with the same correlated messages a
    // target returns through the relay server.
    socket.remoteSceneValidationReceived(targetId, sceneInstanceId, true, QString());
    socket.remoteSceneLaunchedReceived(targetId, sceneInstanceId);
    QTRY_VERIFY_WITH_TIMEOUT(canvas.isRemoteSceneLaunched(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(canvas.isHostSceneActive(), 2000);
    QVERIFY(!media->isSelected());

    launch = findButton(canvas, QStringLiteral("Stop Remote Scene"));
    QVERIFY(launch);
    launch->click();
    socket.remoteSceneStoppedReceived(targetId, sceneInstanceId, true, QString());

    // This selection restoration used to recurse synchronously between
    // pullSettingsFromMedia() and pushSettingsToMedia() until the GUI thread
    // exhausted its stack and crashed.
    QVERIFY(!canvas.isRemoteSceneLaunched());
    QVERIFY(!canvas.isHostSceneActive());
    QVERIFY(media->isSelected());
    QTRY_VERIFY(findButton(canvas, QStringLiteral("Launch Remote Scene")) != nullptr);

    // A target transport loss is terminal even if A did not initiate STOP.
    // The hardened relay emits one correlated successful terminal result, and
    // A must not keep a ghost local scene running afterward.
    outbound.clear();
    launch = findButton(canvas, QStringLiteral("Launch Remote Scene"));
    QVERIFY(launch);
    launch->click();
    QTRY_VERIFY_WITH_TIMEOUT(!sceneInstanceIdFrom(outbound).isEmpty(), 3000);
    const QString disconnectedRunId = sceneInstanceIdFrom(outbound);
    QVERIFY(disconnectedRunId != sceneInstanceId);
    socket.remoteSceneValidationReceived(targetId, disconnectedRunId, true, QString());
    socket.remoteSceneLaunchedReceived(targetId, disconnectedRunId);
    QTRY_VERIFY_WITH_TIMEOUT(canvas.isHostSceneActive(), 2000);

    socket.remoteSceneStoppedReceived(targetId, disconnectedRunId, true, QString());
    QVERIFY(!canvas.isRemoteSceneLaunched());
    QVERIFY(!canvas.isHostSceneActive());
    QVERIFY(media->isSelected());

    socket.disconnect();
}

QTEST_MAIN(RemoteSceneLifecycleTest)
#include "tst_RemoteSceneLifecycle.moc"
