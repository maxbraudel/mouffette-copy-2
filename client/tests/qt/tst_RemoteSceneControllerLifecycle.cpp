#include <QApplication>
#include <QAudioOutput>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVideoSink>
#include <QtTest>

#include "backend/files/FileManager.h"
#include "frontend/rendering/canvas/MediaListModel.h"
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

QJsonObject completeTextScene()
{
    QJsonObject scene = textScene();
    scene[QStringLiteral("renderSchemaVersion")] = 2;
    QJsonObject media = scene.value(QStringLiteral("media")).toArray().first().toObject();
    media[QStringLiteral("fileId")] = QString();
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
    media[QStringLiteral("fontSize")] = 20;
    media[QStringLiteral("fontBold")] = false;
    media[QStringLiteral("fontItalic")] = false;
    media[QStringLiteral("fontUnderline")] = false;
    media[QStringLiteral("fontUppercase")] = false;
    media[QStringLiteral("fontWeight")] = 400;
    media[QStringLiteral("fontPixelSize")] = 20;
    media[QStringLiteral("textColor")] = QStringLiteral("#ffffffff");
    media[QStringLiteral("textBorderWidthPercent")] = 0.0;
    media[QStringLiteral("textOutlineWidthPx")] = 0.0;
    media[QStringLiteral("textBorderColor")] = QStringLiteral("#00000000");
    media[QStringLiteral("textFitToTextEnabled")] = false;
    media[QStringLiteral("textHighlightEnabled")] = false;
    media[QStringLiteral("textHighlightColor")] = QStringLiteral("#00000000");
    media[QStringLiteral("uniformScale")] = 1.0;
    media[QStringLiteral("horizontalAlignment")] = QStringLiteral("center");
    media[QStringLiteral("verticalAlignment")] = QStringLiteral("center");
    QJsonObject span = media.value(QStringLiteral("spans")).toArray().first().toObject();
    span[QStringLiteral("spanDestNormX")] = 0.0;
    span[QStringLiteral("spanDestNormY")] = 0.0;
    span[QStringLiteral("spanDestNormW")] = 1.0;
    span[QStringLiteral("spanDestNormH")] = 1.0;
    span[QStringLiteral("spanSourceNormX")] = 0.0;
    span[QStringLiteral("spanSourceNormY")] = 0.0;
    span[QStringLiteral("spanSourceNormW")] = 1.0;
    span[QStringLiteral("spanSourceNormH")] = 1.0;
    media[QStringLiteral("spans")] = QJsonArray{span};
    scene[QStringLiteral("media")] = QJsonArray{media};
    return scene;
}

QJsonObject snapshotForScene(const QJsonObject& scene, const QJsonArray& videos = {})
{
    return QJsonObject{
        {QStringLiteral("scene"), scene},
        {QStringLiteral("videos"), videos},
        {QStringLiteral("capturedEpochMs"),
         static_cast<double>(QDateTime::currentMSecsSinceEpoch())}
    };
}

QJsonObject videoScene(const QString& fileId)
{
    QJsonObject scene = completeTextScene();

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
    media[QStringLiteral("visible")] = true;
    media[QStringLiteral("z")] = 2.0;
    media[QStringLiteral("contentOpacity")] = 1.0;
    media[QStringLiteral("autoDisplayDelayMs")] = 0;
    media[QStringLiteral("autoPlayDelayMs")] = 0;
    media[QStringLiteral("autoPause")] = false;
    media[QStringLiteral("autoPauseDelayMs")] = 0;
    media[QStringLiteral("autoHide")] = false;
    media[QStringLiteral("autoHideDelayMs")] = 0;
    media[QStringLiteral("hideWhenVideoEnds")] = false;
    media[QStringLiteral("fadeInSeconds")] = 0.0;
    media[QStringLiteral("fadeOutSeconds")] = 0.0;
    media[QStringLiteral("volume")] = 0.75;
    media[QStringLiteral("continuousLoop")] = false;
    media[QStringLiteral("repeatEnabled")] = false;
    media[QStringLiteral("repeatCount")] = 0;
    media[QStringLiteral("autoUnmute")] = false;
    media[QStringLiteral("autoUnmuteDelayMs")] = 0;
    media[QStringLiteral("autoMute")] = false;
    media[QStringLiteral("autoMuteDelayMs")] = 0;
    media[QStringLiteral("muteWhenVideoEnds")] = false;
    media[QStringLiteral("audioFadeInSeconds")] = 0.0;
    media[QStringLiteral("audioFadeOutSeconds")] = 0.0;
    media[QStringLiteral("spans")] = QJsonArray{span};

    QJsonObject completedSpan = span;
    completedSpan[QStringLiteral("spanDestNormX")] = 0.0;
    completedSpan[QStringLiteral("spanDestNormY")] = 0.0;
    completedSpan[QStringLiteral("spanDestNormW")] = 1.0;
    completedSpan[QStringLiteral("spanDestNormH")] = 1.0;
    completedSpan[QStringLiteral("spanSourceNormX")] = 0.0;
    completedSpan[QStringLiteral("spanSourceNormY")] = 0.0;
    completedSpan[QStringLiteral("spanSourceNormW")] = 1.0;
    completedSpan[QStringLiteral("spanSourceNormH")] = 1.0;
    media[QStringLiteral("spans")] = QJsonArray{completedSpan};

    scene[QStringLiteral("sceneInstanceId")] = QStringLiteral("video-lifecycle-test-run");
    scene[QStringLiteral("media")] = QJsonArray{media};
    return scene;
}

QQuickWindow* findRemoteWindow()
{
    for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
        if (candidate && candidate->objectName()
            == QLatin1String("RemoteScreenWindow_0")) {
            return qobject_cast<QQuickWindow*>(candidate);
        }
    }
    return nullptr;
}
}

class RemoteSceneControllerLifecycleTest final : public QObject {
    Q_OBJECT

private slots:
    void teardownDoesNotPumpNestedApplicationEvents()
    {
        RemoteSceneController controller(nullptr, nullptr);
        QSignalSpy teardownSpy(&controller, &RemoteSceneController::teardownSettled);
        const QJsonObject scene = textScene();
        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStart",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-1")),
            Q_ARG(QJsonObject, scene)));

        QPointer<QQuickWindow> remoteWindow = findRemoteWindow();
        QVERIFY2(remoteWindow, "the remote scene must create its target window");
		QPointer<QQuickItem> sceneRoot =
			remoteWindow->findChild<QQuickItem*>(QStringLiteral("remoteSceneRoot"));
		QPointer<MediaListModel> mediaModel =
			remoteWindow->findChild<MediaListModel*>();
		QVERIFY(sceneRoot);
		QVERIFY(mediaModel);
		bool entireGraphDestroyedAtSettlement = false;
		connect(&controller, &RemoteSceneController::teardownSettled,
				&controller, [&]() {
				entireGraphDestroyedAtSettlement = remoteWindow.isNull()
					&& sceneRoot.isNull()
					&& mediaModel.isNull();
		});

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
        QCOMPARE(teardownSpy.count(), 0);
        QVERIFY(remoteWindow);
		QVERIFY(sceneRoot);
		QVERIFY(mediaModel);
        QVERIFY(!remoteWindow->isVisible());

        // QObject/native-window destruction is intentionally deferred to the
        // normal event loop and must still converge promptly.
        QTRY_VERIFY_WITH_TIMEOUT(remoteWindow.isNull(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(teardownSpy.count(), 1, 2000);
		QVERIFY(entireGraphDestroyedAtSettlement);
        QCOMPARE(teardownSpy.first().at(0).toString(), QString());
        QVERIFY(teardownSpy.first().at(1).toBool());
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
        QSignalSpy teardownSpy(&controller, &RemoteSceneController::teardownSettled);
        const QJsonObject scene = videoScene(fileId);
        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStart",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-video")),
            Q_ARG(QJsonObject, scene)));

        QPointer<QMediaPlayer> player = controller.findChild<QMediaPlayer*>();
        if (!player) {
            files.removeReceivedFileMapping(fileId);
            QSKIP("The installed multimedia backend cannot decode the optional MP4 fixture");
        }
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
        QCOMPARE(teardownSpy.count(), 0);
        QVERIFY(player);
        QCOMPARE(player->videoSink(), nullptr);
        QCOMPARE(player->audioOutput(), nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(player.isNull(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(teardownSpy.count(), 1, 2000);
        QVERIFY(unrelatedQueuedCallbackRan);
        files.removeReceivedFileMapping(fileId);
    }

    void emptySessionTeardownIsAsynchronousAndIdempotent()
    {
        RemoteSceneController controller(nullptr, nullptr);
        QSignalSpy teardownSpy(&controller, &RemoteSceneController::teardownSettled);
        const QString sessionId = QStringLiteral("remote-session-idempotent");

        QVERIFY(controller.teardownRemoteSession(sessionId));
        QVERIFY(controller.teardownRemoteSession(sessionId));
        QCOMPARE(teardownSpy.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(teardownSpy.count(), 1, 1000);
        QCOMPARE(teardownSpy.first().at(0).toString(), sessionId);
        QVERIFY(teardownSpy.first().at(1).toBool());

        // A later retry may represent a lost ACK. Settlement is replayed on
        // the next event-loop turn without entering destruction a second time.
        QVERIFY(controller.teardownRemoteSession(sessionId));
        QCOMPARE(teardownSpy.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(teardownSpy.count(), 2, 1000);
        QCOMPARE(teardownSpy.last().at(0).toString(), sessionId);
        QVERIFY(teardownSpy.last().at(1).toBool());
    }

	void unrelatedSessionCannotRetireAnActiveRendererGraph()
	{
		RemoteSceneController controller(nullptr, nullptr);
		QVERIFY(QMetaObject::invokeMethod(
			&controller, "onRemoteSceneStart", Qt::DirectConnection,
			Q_ARG(QString, QStringLiteral("owned-renderer")),
			Q_ARG(QJsonObject, textScene())));

		QPointer<QQuickWindow> remoteWindow = findRemoteWindow();
		QVERIFY(remoteWindow);
		QVERIFY(!controller.teardownRemoteSession(
			QStringLiteral("unrelated-remote-session")));
		QCoreApplication::processEvents();
		QVERIFY(remoteWindow);

		QVERIFY(QMetaObject::invokeMethod(
			&controller, "onRemoteSceneStop", Qt::DirectConnection,
			Q_ARG(QString, QStringLiteral("owned-renderer")),
			Q_ARG(QString, QStringLiteral("lifecycle-test-run"))));
		QTRY_VERIFY_WITH_TIMEOUT(remoteWindow.isNull(), 2000);
	}

    void authoritativeSnapshotValidatesCompletelyBeforeMutation()
    {
        RemoteSceneController controller(nullptr, nullptr);
        const QJsonObject initialScene = completeTextScene();
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "onRemoteSceneStart", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("snapshot-owner")),
            Q_ARG(QJsonObject, initialScene)));

		QQuickWindow* remoteWindow = findRemoteWindow();
        QVERIFY(remoteWindow);
        MediaListModel* model = remoteWindow->findChild<MediaListModel*>();
        QVERIFY(model);
        QCOMPARE(model->rowCount(), 1);
        const QModelIndex row = model->index(0, 0);
        const QVariantMap initial =
            model->data(row, MediaListModel::ModelDataRole).toMap();
        QCOMPARE(initial.value(QStringLiteral("textContent")).toString(),
                 QStringLiteral("teardown-test"));

        QJsonObject authoritativeScene = initialScene;
        QJsonObject media =
            authoritativeScene.value(QStringLiteral("media")).toArray().first().toObject();
        media[QStringLiteral("text")] = QStringLiteral("authoritative-text");
        media[QStringLiteral("visible")] = false;
        media[QStringLiteral("z")] = 42.0;
        media[QStringLiteral("contentOpacity")] = 0.4;
		QJsonObject authoritativeSpan =
			media.value(QStringLiteral("spans")).toArray().first().toObject();
		authoritativeSpan[QStringLiteral("spanDestNormX")] = 0.25;
		authoritativeSpan[QStringLiteral("spanDestNormW")] = 0.5;
		authoritativeSpan[QStringLiteral("spanSourceNormX")] = 0.25;
		authoritativeSpan[QStringLiteral("spanSourceNormW")] = 0.5;
		media[QStringLiteral("spans")] = QJsonArray{authoritativeSpan};
        authoritativeScene[QStringLiteral("media")] = QJsonArray{media};

        QSignalSpy appliedSpy(&controller,
                              &RemoteSceneController::authoritativeSnapshotApplied);
        QSignalSpy rejectedSpy(&controller,
                               &RemoteSceneController::authoritativeSnapshotRejected);
        bool applied = false;
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "applyAuthoritativeStateSnapshot", Qt::DirectConnection,
            Q_RETURN_ARG(bool, applied),
            Q_ARG(QJsonObject, snapshotForScene(authoritativeScene)),
            Q_ARG(quint64, quint64(1)),
            Q_ARG(qint64, qint64(0))));
        QVERIFY(applied);
        QCOMPARE(appliedSpy.count(), 1);
        QCOMPARE(rejectedSpy.count(), 0);

        QVariantMap after = model->data(row, MediaListModel::ModelDataRole).toMap();
        QCOMPARE(after.value(QStringLiteral("textContent")).toString(),
                 QStringLiteral("authoritative-text"));
        QCOMPARE(after.value(QStringLiteral("z")).toDouble(), 42.0);
		QCOMPARE(after.value(QStringLiteral("sourceX")).toDouble(), 0.25);
		QCOMPARE(after.value(QStringLiteral("sourceWidth")).toDouble(), 0.5);
        QVERIFY(!after.value(QStringLiteral("renderVisible")).toBool());

		// Screen topology belongs to the immutable run. A resumed snapshot may
		// reorder screen entries by id, but it cannot rebuild/remap their source
		// definitions or smuggle media changes through that invalid transaction.
		QJsonObject changedTopologyScene = authoritativeScene;
		QJsonObject changedScreen =
			changedTopologyScene.value(QStringLiteral("screens")).toArray().first().toObject();
		changedScreen[QStringLiteral("width")] = 1280;
		changedTopologyScene[QStringLiteral("screens")] = QJsonArray{changedScreen};
		QJsonObject topologyMedia =
			changedTopologyScene.value(QStringLiteral("media")).toArray().first().toObject();
		topologyMedia[QStringLiteral("text")] = QStringLiteral("must-not-apply");
		changedTopologyScene[QStringLiteral("media")] = QJsonArray{topologyMedia};
		bool topologyApplied = true;
		QVERIFY(QMetaObject::invokeMethod(
			&controller, "applyAuthoritativeStateSnapshot", Qt::DirectConnection,
			Q_RETURN_ARG(bool, topologyApplied),
			Q_ARG(QJsonObject, snapshotForScene(changedTopologyScene)),
			Q_ARG(quint64, quint64(2)),
			Q_ARG(qint64, qint64(0))));
		QVERIFY(!topologyApplied);
		after = model->data(row, MediaListModel::ModelDataRole).toMap();
		QCOMPARE(after.value(QStringLiteral("textContent")).toString(),
				 QStringLiteral("authoritative-text"));

        // A high-sequence malformed snapshot must not partially apply nor
        // poison the sequence, so the corrected snapshot with the same
        // sequence remains admissible.
        QJsonObject malformedScene = authoritativeScene;
        QJsonObject malformedMedia =
            malformedScene.value(QStringLiteral("media")).toArray().first().toObject();
        malformedMedia[QStringLiteral("text")] = 123;
        malformedMedia[QStringLiteral("z")] = 99.0;
        malformedScene[QStringLiteral("media")] = QJsonArray{malformedMedia};
        bool malformedApplied = true;
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "applyAuthoritativeStateSnapshot", Qt::DirectConnection,
            Q_RETURN_ARG(bool, malformedApplied),
            Q_ARG(QJsonObject, snapshotForScene(malformedScene)),
            Q_ARG(quint64, quint64(2)),
            Q_ARG(qint64, qint64(0))));
        QVERIFY(!malformedApplied);
		QCOMPARE(rejectedSpy.count(), 2);
        after = model->data(row, MediaListModel::ModelDataRole).toMap();
        QCOMPARE(after.value(QStringLiteral("textContent")).toString(),
                 QStringLiteral("authoritative-text"));
        QCOMPARE(after.value(QStringLiteral("z")).toDouble(), 42.0);

        bool correctedApplied = false;
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "applyAuthoritativeStateSnapshot", Qt::DirectConnection,
            Q_RETURN_ARG(bool, correctedApplied),
            Q_ARG(QJsonObject, snapshotForScene(authoritativeScene)),
            Q_ARG(quint64, quint64(2)),
            Q_ARG(qint64, qint64(0))));
        QVERIFY(correctedApplied);
        QCOMPARE(appliedSpy.count(), 2);
    }

    void videoSnapshotRequiresAndAppliesEveryVideoState()
    {
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }

        const QString fileId(64, QLatin1Char('b'));
        FileManager files;
        files.registerReceivedFilePath(fileId, fixture);
        RemoteSceneController controller(&files, nullptr);
        QJsonObject scene = videoScene(fileId);
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "onRemoteSceneStart", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("video-snapshot-owner")),
            Q_ARG(QJsonObject, scene)));
        QMediaPlayer* player = controller.findChild<QMediaPlayer*>();
        QAudioOutput* audio = controller.findChild<QAudioOutput*>();
        if (!player) {
            files.removeReceivedFileMapping(fileId);
            QSKIP("The installed multimedia backend cannot decode the optional MP4 fixture");
        }
        QVERIFY(audio);

        bool missingApplied = true;
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "applyAuthoritativeStateSnapshot", Qt::DirectConnection,
            Q_RETURN_ARG(bool, missingApplied),
            Q_ARG(QJsonObject, snapshotForScene(scene)),
            Q_ARG(quint64, quint64(1)),
            Q_ARG(qint64, qint64(0))));
        QVERIFY(!missingApplied);

        QJsonObject media = scene.value(QStringLiteral("media")).toArray().first().toObject();
        media[QStringLiteral("visible")] = false;
        media[QStringLiteral("muted")] = true;
        scene[QStringLiteral("media")] = QJsonArray{media};
        QJsonObject video{
            {QStringLiteral("mediaId"), QStringLiteral("video-1")},
            {QStringLiteral("positionMs"), 1000.0},
            {QStringLiteral("durationMs"), 5000.0},
            {QStringLiteral("playing"), false},
            {QStringLiteral("muted"), true},
            {QStringLiteral("visible"), false},
            {QStringLiteral("repeatAvailable"), false}
        };
        bool applied = false;
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "applyAuthoritativeStateSnapshot", Qt::DirectConnection,
            Q_RETURN_ARG(bool, applied),
            Q_ARG(QJsonObject, snapshotForScene(scene, QJsonArray{video})),
            Q_ARG(quint64, quint64(1)),
            Q_ARG(qint64, qint64(0))));
        QVERIFY(applied);
        QVERIFY(audio->isMuted());
        QVERIFY(player->playbackState() != QMediaPlayer::PlayingState);
        files.removeReceivedFileMapping(fileId);
    }

    void renderGraphReadinessFailsClosedWhenWindowIsLost()
    {
        RemoteSceneController controller(nullptr, nullptr);
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "onRemoteSceneStart", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("renderer-owner")),
            Q_ARG(QJsonObject, completeTextScene())));
        QQuickWindow* remoteWindow = findRemoteWindow();
        QVERIFY(remoteWindow);
        bool ready = false;
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "remoteRenderGraphsReady", Qt::DirectConnection,
            Q_RETURN_ARG(bool, ready)));
        QVERIFY(ready);

		delete remoteWindow;
		QVERIFY(QMetaObject::invokeMethod(
			&controller, "remoteRenderGraphsReady", Qt::DirectConnection,
			Q_RETURN_ARG(bool, ready)));
		QVERIFY(!ready);
    }

    void controllerDestructionDuringTeardownCancelsBarrierSafely()
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

        QPointer<QQuickWindow> remoteWindow = findRemoteWindow();
        QVERIFY(remoteWindow);

        QVERIFY(QMetaObject::invokeMethod(
            rawController,
            "onRemoteSceneStop",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-destroy")),
            Q_ARG(QString, QStringLiteral("lifecycle-test-run"))));

        // Destroying the barrier context while deferred deletions are active
        // must cancel every callback; the independently queued native-window
        // delete still has to converge without dereferencing the controller.
        delete rawController;
        QVERIFY(controller.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(remoteWindow.isNull(), 2000);
    }
};

QTEST_MAIN(RemoteSceneControllerLifecycleTest)
#include "tst_RemoteSceneControllerLifecycle.moc"
