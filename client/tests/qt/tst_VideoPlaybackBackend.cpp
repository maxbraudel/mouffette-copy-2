#include <QApplication>
#include <QAudioOutput>
#include <QFile>
#include <QMediaPlayer>
#include <QJsonArray>
#include "frontend/qml/MediaSettingsViewModel.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include <QQuickItem>
#include <QQuickView>
#include <QVideoSink>
#include <QtTest>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacVideoThumbnailer.h"
#elif defined(Q_OS_WIN)
#include "backend/platform/windows/WindowsVideoThumbnailer.h"
#endif

namespace {
QString videoFixture()
{
    const QString override = qEnvironmentVariable("MOUFFETTE_TEST_VIDEO_FILE");
    return override.isEmpty() ? QString::fromUtf8(TEST_VIDEO_FILE) : override;
}
}

class VideoPlaybackBackendTest final : public QObject
{
    Q_OBJECT

private slots:
    void seekBeforeFirstPlay_data()
    {
        QTest::addColumn<bool>("whileLoading");
        QTest::newRow("loaded") << false;
        QTest::newRow("restored-while-loading") << true;
    }

    void seekBeforeFirstPlay()
    {
        QFETCH(bool, whileLoading);
        CanvasDocument document;
        auto* video = document.addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        auto* player = video->player();
        QSignalSpy states(player, &QMediaPlayer::playbackStateChanged);
        if (!whileLoading)
            QTRY_VERIFY(player->duration() > 3000 && player->isSeekable());
        for (qint64 target : {qint64(1234), qint64(2678), qint64(789), qint64(0)}) {
            video->setPositionMs(target);
            QCOMPARE(video->positionMs(), target);
            QTRY_VERIFY(player->duration() > 3000 && player->isSeekable());
            // setPosition() first reports the requested value optimistically.
            // Allow the asynchronous native seek to complete: Qt's original
            // Darwin backend then changed 1234 to 1000 before the first play.
            QTest::qWait(700);
            QVERIFY2(qAbs(player->position() - target) <= 1,
                     qPrintable(QStringLiteral("Requested %1 ms, settled at %2 ms")
                                    .arg(target).arg(player->position())));
            QVERIFY(!video->isPlaying());
        }
        for (const auto& state : states)
            QVERIFY(state[0].value<QMediaPlayer::PlaybackState>() != QMediaPlayer::PlayingState);
    }

    void seekAfterPauseAndSourceReload()
    {
        CanvasDocument document;
        auto* video = document.addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        video->setMuted(true);
        QTRY_VERIFY(video->player()->duration() > 3000);
        video->togglePlayPause();
        QTRY_VERIFY(video->positionMs() > 100);
        video->togglePlayPause();
        video->setPositionMs(1789);
        QTest::qWait(700);
        QVERIFY(qAbs(video->positionMs() - 1789) <= 1);
        QVERIFY(!video->isPlaying());

        video->setSourcePath(QString());
        video->setSourcePath(videoFixture());
        QTRY_VERIFY(video->player()->duration() > 3000 && video->player()->isSeekable());
        video->seekToRatio(0.1234);
        const qint64 target = qRound64(video->player()->duration() * 0.1234);
        QTest::qWait(700);
        QVERIFY(qAbs(video->positionMs() - target) <= 1);
        QVERIFY(!video->isPlaying());
        QVERIFY(video->muted());
    }

    void previewAndMarkersAreIndependentAndPersist()
    {
        const QString fixture = videoFixture();
        QVERIFY(QFile::exists(fixture));
        CanvasDocument document;
        QuickCanvasController controller(&document);
        controller.setProjectEditingEnabled(true);
        auto* video = document.addPreparedFile(fixture, QSize(160, 90), true, {});
        QVERIFY(video);
        QTRY_VERIFY(video->player()->duration() > 3000);
        MediaSettingsViewModel settings;
        settings.setController(&controller);
        QTRY_VERIFY(settings.available());
        video->setPositionMs(1200);
        QCOMPARE(document.serializeSceneState().value("media").toArray()[0]
                     .toObject().value("startPositionMs").toInt(), 0);
        QVERIFY(settings.canPlaceVideoStart());
        QVERIFY(settings.canPlaceVideoEnd());
        settings.toggleVideoStart();
        QCOMPARE(video->startMarkerMs(), 1200);
        QVERIFY(settings.hasVideoStart());
        QVERIFY(!settings.canPlaceVideoEnd());
        video->setPositionMs(900);
        QVERIFY(!settings.canPlaceVideoEnd());
        settings.toggleVideoEnd();
        QCOMPARE(video->endMarkerMs(), -1);
        video->setPositionMs(2400);
        QVERIFY(settings.canPlaceVideoEnd());
        settings.toggleVideoEnd();
        QCOMPARE(video->endMarkerMs(), 2400);
        video->setPositionMs(600);
        const auto scene = document.serializeSceneState().value("media").toArray()[0].toObject();
        QCOMPARE(scene.value("startPositionMs").toInt(), 1200);
        QCOMPARE(scene.value("endPositionMs").toInt(), 2400);
        QVERIFY(!video->setPlaybackRange(2400, 2400));
        QVERIFY(!video->setPlaybackRange(2500, 2400));

        const auto project = document.serializeProjectState();
        QCOMPARE(project.value("media").toArray()[0].toObject().value("previewPositionMs").toInt(), 600);
        CanvasDocument restored;
        QVERIFY(restored.restoreProjectState(project, {{video->mediaId(), fixture}}));
        auto* copy = restored.mediaById(video->mediaId());
        QVERIFY(copy);
        QCOMPARE(copy->startMarkerMs(), 1200);
        QCOMPARE(copy->endMarkerMs(), 2400);
        settings.toggleVideoStart();
        QCOMPARE(video->startMarkerMs(), -1);
        video->setPositionMs(2400);
        QVERIFY(!settings.canPlaceVideoStart());
        settings.toggleVideoStart();
        QCOMPARE(video->startMarkerMs(), -1);
        video->setPositionMs(2399);
        QVERIFY(settings.canPlaceVideoStart());
        settings.toggleVideoEnd();
        QVERIFY(!settings.hasVideoEnd());

        // Old projects only stored the preview cursor. Never migrate it into a marker.
        auto legacy = project;
        auto entries = legacy.value("media").toArray();
        auto entry = entries[0].toObject();
        entry.remove("videoStartMarkerMs");
        entry.remove("videoEndMarkerMs");
        entry.remove("previewPositionMs");
        entries[0] = entry;
        legacy["media"] = entries;
        CanvasDocument oldProject;
        QVERIFY(oldProject.restoreProjectState(legacy, {{video->mediaId(), fixture}}));
        QCOMPARE(oldProject.mediaById(video->mediaId())->startMarkerMs(), -1);
        QCOMPARE(oldProject.serializeSceneState().value("media").toArray()[0]
                     .toObject().value("startPositionMs").toInt(), 0);
    }

    void boundedPlayback_data()
    {
        QTest::addColumn<bool>("continuous");
        QTest::addColumn<int>("repeats");
        QTest::newRow("stop-at-end") << false << 0;
        QTest::newRow("continuous-range") << true << 0;
        QTest::newRow("one-repeat") << false << 1;
    }

    void boundedPlayback()
    {
        QFETCH(bool, continuous);
        QFETCH(int, repeats);
        CanvasDocument document;
        auto* video = document.addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        video->setMuted(true);
        QTRY_VERIFY(video->player()->duration() > 3000);
        QVERIFY(video->setPlaybackRange(1000, 1700));
        video->setRepeatEnabled(continuous);
        auto settings = video->settings();
        settings.repeatEnabled = repeats > 0;
        settings.repeatCountText = QString::number(repeats);
        video->setSettings(settings);
        video->setPositionMs(2600);
        QCOMPARE(video->positionMs(), 2600); // Paused scrubbing is unrestricted.
        video->beginScenePlayback();
        QCOMPARE(video->positionMs(), 1000);
        int wraps = 0;
        qint64 previous = 1000;
        connect(video->player(), &QMediaPlayer::positionChanged, &document,
                [&](qint64 pos) {
            if (previous > 1400 && pos == 1000) ++wraps;
            previous = pos;
        });
        video->player()->play();
        if (continuous) {
            QTRY_VERIFY_WITH_TIMEOUT(wraps >= 2, 5000);
            QVERIFY(video->isPlaying());
            video->player()->pause();
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying() && video->positionMs() == 1700, 5000);
            QCOMPARE(wraps, repeats);
        }
        video->endScenePlayback();
    }

    void repeatWithMissingMarkers_data()
    {
        QTest::addColumn<qint64>("start");
        QTest::addColumn<qint64>("end");
        QTest::newRow("whole-video") << qint64(-1) << qint64(-1);
        QTest::newRow("start-only") << qint64(1000) << qint64(-1);
        QTest::newRow("end-only") << qint64(-1) << qint64(1700);
    }

    void repeatWithMissingMarkers()
    {
        QFETCH(qint64, start);
        QFETCH(qint64, end);
        CanvasDocument document;
        auto* video = document.addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        video->setMuted(true);
        QTRY_VERIFY(video->player()->duration() > 3000);
        QVERIFY(video->setPlaybackRange(start, end));
        video->setRepeatEnabled(true);
        video->setPositionMs(video->playbackEndMs() - 250);
        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying()
            && video->positionMs() >= video->playbackStartMs()
            && video->positionMs() < video->playbackStartMs() + 500, 4000);
        video->player()->pause();
    }

    void sceneLaunchResetsPreviewAndStopRestoresIt()
    {
        QString error;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
        QVERIFY2(host, qPrintable(error));
        host->setProjectEditingEnabled(true);
        auto* video = host->document()->addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        QTRY_VERIFY(video->player()->duration() > 3000);
        auto settings = video->settings();
        settings.playAutomatically = false;
        video->setSettings(settings);
        for (qint64 start : {qint64(-1), qint64(1100)}) {
            QVERIFY(video->setPlaybackRange(start, -1));
            video->setPositionMs(2200);
            host->triggerTestSceneAction();
            QCOMPARE(video->positionMs(), qMax<qint64>(0, start));
            host->triggerTestSceneAction();
            QCOMPARE(video->positionMs(), 2200);
        }
    }

    void nativePreparationReadsDisplaySizeAndFirstFrame()
    {
        const QString fixture = videoFixture();
        if (!QFile::exists(fixture)) QSKIP("Optional real-video fixture is missing");
#ifdef Q_OS_MACOS
        const QSize dimensions = MacVideoThumbnailer::videoDimensions(fixture);
        QVERIFY(!dimensions.isEmpty());
        QCOMPARE(MacVideoThumbnailer::firstFrame(fixture).size(), dimensions);
#elif defined(Q_OS_WIN)
        const QSize dimensions = WindowsVideoThumbnailer::videoDimensions(fixture);
        QVERIFY(!dimensions.isEmpty());
        QCOMPARE(WindowsVideoThumbnailer::firstFrame(fixture).size(), dimensions);
#else
        QSKIP("Native first-frame preparation is implemented on macOS and Windows");
#endif
    }

    void qmlVideoOutputUsesDocumentRuntimeAndPreservesAudioState()
    {
        const QString fixture = videoFixture();
        if (!QFile::exists(fixture)) QSKIP("Optional real-video fixture is missing");

        CanvasDocument document;
        QuickCanvasController controller(&document);
        QString error;
        QVERIFY2(controller.initialize(&error), qPrintable(error));
        const QSize nativeSize =
#ifdef Q_OS_MACOS
            MacVideoThumbnailer::videoDimensions(fixture);
#elif defined(Q_OS_WIN)
            WindowsVideoThumbnailer::videoDimensions(fixture);
#else
            QSize(1920, 1080);
#endif
        QVERIFY(!nativeSize.isEmpty());
        CanvasMedia* video = document.addPreparedFile(
            fixture, nativeSize, true, QPointF(0, 0));
        QVERIFY(video && video->isVideo());
        video->setVolume(0.31);

        QQuickView view;
        view.resize(960, 600);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setSource(QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/CanvasRoot.qml")));
        QCOMPARE(view.status(), QQuickView::Ready);
        QVERIFY(view.rootObject());
        view.rootObject()->setProperty("sessionViewModel", QVariantMap{
            {QStringLiteral("canvasController"),
             QVariant::fromValue<QObject*>(&controller)}});
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QMediaPlayer* player = video->player();
        QAudioOutput* audio = video->audioOutput();
        QVERIFY(player && audio && video->videoSink());
        QTRY_VERIFY_WITH_TIMEOUT(player->videoSink() != nullptr, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(player->videoSink() != video->videoSink(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(player->mediaStatus() == QMediaPlayer::LoadedMedia
                                 || player->mediaStatus() == QMediaPlayer::BufferedMedia,
                                 8000);
        QVERIFY(qAbs(audio->volume() - 0.31) < 0.02);

        video->setMuted(true);
        QVERIFY(video->muted());
        video->setMuted(false);
        QVERIFY(!video->muted());
        video->setRepeatEnabled(true);
        QCOMPARE(player->loops(), QMediaPlayer::Infinite);

        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > 250, 5000);
        video->seekToRatio(0.5);
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(player->position() - player->duration() / 2) < 350, 4000);
        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(), 3000);

        const QVariantMap projection = video->toModelMap();
        QCOMPARE(projection.value(QStringLiteral("mediaType")).toString(),
                 QStringLiteral("video"));
        QCOMPARE(projection.value(QStringLiteral("width")).toInt(),
                 nativeSize.width());
        QVERIFY(projection.value(QStringLiteral("videoPlayerPtr")).value<QObject*>()
                == player);
    }

    void dragPreviewHandoffAndPlaybackRemainStable()
    {
        const QString fixture = videoFixture();
        if (!QFile::exists(fixture)) QSKIP("Optional real-video fixture is missing");

        const QSize nativeSize =
#ifdef Q_OS_MACOS
            MacVideoThumbnailer::videoDimensions(fixture);
#elif defined(Q_OS_WIN)
            WindowsVideoThumbnailer::videoDimensions(fixture);
#else
            QSize();
#endif
        if (nativeSize.isEmpty()) {
            QSKIP("Native video preview is unavailable on this platform");
        }

        CanvasDocument document;
        QuickCanvasController controller(&document);
        QString error;
        QVERIFY2(controller.initialize(&error), qPrintable(error));
        controller.setProjectEditingEnabled(true);

        QQuickView view;
        view.resize(960, 600);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setSource(QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/CanvasRoot.qml")));
        QCOMPARE(view.status(), QQuickView::Ready);
        QVERIFY(view.rootObject());
        view.rootObject()->setProperty("sessionViewModel", QVariantMap{
            {QStringLiteral("canvasController"),
             QVariant::fromValue<QObject*>(&controller)}});
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QVERIFY(controller.beginLocalFileDrag(
            {QUrl::fromLocalFile(fixture)}, 480, 300));
        const QVariantMap preview = controller.dropPreviewModel();
        QCOMPARE(preview.value(QStringLiteral("width")).toInt(), nativeSize.width());
        QCOMPARE(preview.value(QStringLiteral("height")).toInt(), nativeSize.height());
        QVERIFY(preview.value(QStringLiteral("frameReady")).toBool());
        auto* frameSource = qobject_cast<RemoteVideoFrameSource*>(
            controller.dropPreviewFrameSource());
        QVERIFY(frameSource && frameSource->hasFrame());
        QCOMPARE(frameSource->frame().size(), nativeSize);

        QVERIFY(controller.commitLocalFileDrop(480, 300));
        CanvasMedia* video = document.selectedMedia();
        QVERIFY(video && video->isVideo() && video->player());
        QCOMPARE(video->baseSize(), nativeSize);
        QTRY_VERIFY_WITH_TIMEOUT(video->player()->mediaStatus()
                                     == QMediaPlayer::LoadedMedia
                                 || video->player()->mediaStatus()
                                     == QMediaPlayer::BufferedMedia,
                                 8000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !controller.dropPreviewModel()
                 .value(QStringLiteral("visible")).toBool(),
            5000);
        QVERIFY(!video->isPlaying());

        RemoteVideoFrameSource* posterSource = nullptr;
        for (const QVariant& value : controller.mediaSnapshot()) {
            const QVariantMap media = value.toMap();
            if (media.value(QStringLiteral("mediaId")).toString()
                == video->mediaId()) {
                posterSource = qobject_cast<RemoteVideoFrameSource*>(
                    media.value(QStringLiteral("videoPosterFrameSource"))
                        .value<QObject*>());
                break;
            }
        }
        QVERIFY(posterSource && posterSource->hasFrame());

        const QPointF originalPosition = video->position();
        controller.handleMediaMoveStarted(video->mediaId(),
                                          originalPosition.x(),
                                          originalPosition.y(), false);
        controller.handleMediaMoveUpdated(video->mediaId(),
                                          originalPosition.x() + 80,
                                          originalPosition.y() + 40, false);
        controller.handleMediaMoveEnded(video->mediaId(),
                                        originalPosition.x() + 80,
                                        originalPosition.y() + 40, false);
        QCOMPARE(video->position(), originalPosition + QPointF(80, 40));
        QVERIFY(!controller.dropPreviewModel()
                     .value(QStringLiteral("visible")).toBool());

        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(video->positionMs() > 100, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!posterSource->hasFrame(), 5000);
        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(), 3000);
    }
};

QTEST_MAIN(VideoPlaybackBackendTest)
#include "tst_VideoPlaybackBackend.moc"
