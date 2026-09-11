#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QPointer>
#include <QTimer>
#include <QVideoSink>
#include <QWidget>
#include <QtTest>

#include "backend/files/FileManager.h"
#include "frontend/rendering/remote/RemoteSceneController.h"

namespace {
QJsonObject textScene()
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

    QJsonObject media;
    media[QStringLiteral("mediaId")] = QStringLiteral("text-1");
    media[QStringLiteral("type")] = QStringLiteral("text");
    media[QStringLiteral("text")] = QStringLiteral("teardown-test");
    media[QStringLiteral("x")] = 0.0;
    media[QStringLiteral("y")] = 0.0;
    media[QStringLiteral("width")] = 320.0;
    media[QStringLiteral("height")] = 180.0;
    media[QStringLiteral("spans")] = QJsonArray{span};

    QJsonObject scene;
    scene[QStringLiteral("sceneInstanceId")] = QStringLiteral("lifecycle-test-run");
    scene[QStringLiteral("screens")] = QJsonArray{screen};
    scene[QStringLiteral("media")] = QJsonArray{media};
    return scene;
}

QJsonObject videoScene(const QString& fileId)
{
    QJsonObject scene = textScene();

    QJsonObject span;
    span[QStringLiteral("screenId")] = 0;
    span[QStringLiteral("normX")] = 0.0;
    span[QStringLiteral("normY")] = 0.0;
    span[QStringLiteral("normW")] = 1.0;
    span[QStringLiteral("normH")] = 1.0;

    QJsonObject media;
    media[QStringLiteral("mediaId")] = QStringLiteral("video-1");
    media[QStringLiteral("fileId")] = fileId;
    media[QStringLiteral("fileName")] = QStringLiteral("video-1080p.mp4");
    media[QStringLiteral("type")] = QStringLiteral("video");
    media[QStringLiteral("x")] = 0.0;
    media[QStringLiteral("y")] = 0.0;
    media[QStringLiteral("width")] = 1920.0;
    media[QStringLiteral("height")] = 1080.0;
    media[QStringLiteral("baseWidth")] = 1920;
    media[QStringLiteral("baseHeight")] = 1080;
    media[QStringLiteral("autoDisplay")] = true;
    media[QStringLiteral("autoPlay")] = true;
    media[QStringLiteral("muted")] = true;
    media[QStringLiteral("spans")] = QJsonArray{span};

    scene[QStringLiteral("sceneInstanceId")] = QStringLiteral("video-lifecycle-test-run");
    scene[QStringLiteral("media")] = QJsonArray{media};
    return scene;
}
}

class RemoteSceneControllerLifecycleTest final : public QObject {
    Q_OBJECT

private slots:
    void teardownDoesNotPumpNestedApplicationEvents()
    {
        RemoteSceneController controller(nullptr, nullptr);
        const QJsonObject scene = textScene();
        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStart",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-1")),
            Q_ARG(QJsonObject, scene)));

        QPointer<QWidget> remoteWindow;
        for (QWidget* window : QApplication::topLevelWidgets()) {
            if (window && window->objectName() == QLatin1String("RemoteScreenWindow_0")) {
                remoteWindow = window;
                break;
            }
        }
        QVERIFY2(remoteWindow, "the remote scene must create its target window");

        bool unrelatedQueuedCallbackRan = false;
        QTimer::singleShot(0, &controller, [&unrelatedQueuedCallbackRan]() {
            unrelatedQueuedCallbackRan = true;
        });

        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStop",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-1")),
            Q_ARG(QString, QStringLiteral("lifecycle-test-run"))));

        // A retry of the same correlated STOP must be an idempotent no-op, not
        // a second entry into the teardown path.
        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStop",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-1")),
            Q_ARG(QString, QStringLiteral("lifecycle-test-run"))));

        // STOP/clearScene runs inside a WebSocket callback in production. It
        // must return before any unrelated queued work can run.
        QVERIFY(!unrelatedQueuedCallbackRan);
        QVERIFY(remoteWindow);
        QVERIFY(!remoteWindow->isVisible());

        // QObject/native-window destruction is intentionally deferred to the
        // normal event loop and must still converge promptly.
        QTRY_VERIFY_WITH_TIMEOUT(remoteWindow.isNull(), 2000);
        QVERIFY(unrelatedQueuedCallbackRan);
    }

    void activeVideoDecoderStopsWithoutNestedEventProcessing()
    {
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }

        const QString fileId(64, QLatin1Char('a'));
        FileManager files;
        files.registerReceivedFilePath(fileId, fixture);
        RemoteSceneController controller(&files, nullptr);
        const QJsonObject scene = videoScene(fileId);
        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStart",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-video")),
            Q_ARG(QJsonObject, scene)));

        QPointer<QMediaPlayer> player = controller.findChild<QMediaPlayer*>();
        QVERIFY2(player, "the video scene must start a real QMediaPlayer decoder");
        const QList<QVideoSink*> sinks =
            player->findChildren<QVideoSink*>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY2(!sinks.isEmpty(), "remote video priming must create a player-owned sink");
        for (QVideoSink* sink : sinks) {
            QCOMPARE(sink->parent(), player.data());
        }

        bool unrelatedQueuedCallbackRan = false;
        QTimer::singleShot(0, &controller, [&unrelatedQueuedCallbackRan]() {
            unrelatedQueuedCallbackRan = true;
        });

        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStop",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-video")),
            Q_ARG(QString, QStringLiteral("video-lifecycle-test-run"))));

        QVERIFY(!unrelatedQueuedCallbackRan);
        QVERIFY(player);
        QCOMPARE(player->videoSink(), nullptr);
        QCOMPARE(player->audioOutput(), nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(player.isNull(), 2000);
        QVERIFY(unrelatedQueuedCallbackRan);
        files.removeReceivedFileMapping(fileId);
    }

    void controllerDestructionDuringTeardownCancelsCooldownSafely()
    {
        auto* rawController = new RemoteSceneController(nullptr, nullptr);
        QPointer<RemoteSceneController> controller(rawController);
        const QJsonObject scene = textScene();
        QVERIFY(QMetaObject::invokeMethod(
            rawController,
            "onRemoteSceneStart",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-destroy")),
            Q_ARG(QJsonObject, scene)));

        QPointer<QWidget> remoteWindow;
        for (QWidget* window : QApplication::topLevelWidgets()) {
            if (window && window->objectName() == QLatin1String("RemoteScreenWindow_0")) {
                remoteWindow = window;
                break;
            }
        }
        QVERIFY(remoteWindow);

        QVERIFY(QMetaObject::invokeMethod(
            rawController,
            "onRemoteSceneStop",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-destroy")),
            Q_ARG(QString, QStringLiteral("lifecycle-test-run"))));

        // The cooldown timer is a controller child. Destroying its context while
        // it is active must cancel the callback; the independently queued native
        // window delete still has to converge without dereferencing the controller.
        delete rawController;
        QVERIFY(controller.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(remoteWindow.isNull(), 2000);
    }
};

QTEST_MAIN(RemoteSceneControllerLifecycleTest)
#include "tst_RemoteSceneControllerLifecycle.moc"
