#include <QApplication>
#include <QAudioOutput>
#include <QFile>
#include <QGraphicsScene>
#include <QJSValue>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QQuickItem>
#include <QQuickWidget>
#include <QVideoSink>
#include <QWidget>
#include <QtTest>

#include "backend/domain/media/MediaItems.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacVideoThumbnailer.h"
#elif defined(Q_OS_WIN)
#include "backend/platform/windows/WindowsVideoThumbnailer.h"
#endif

namespace {
QVariantMap publishedMedia(QObject* root, const QString& mediaId)
{
    QVariant value = root->property("mediaModel");
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        value = value.value<QJSValue>().toVariant();
    for (const QVariant& entryValue : value.toList()) {
        const QVariantMap entry = entryValue.toMap();
        if (entry.value(QStringLiteral("mediaId")).toString() == mediaId)
            return entry;
    }
    return {};
}
}

class VideoPlaybackBackendTest final : public QObject {
    Q_OBJECT

private slots:
    void nativePreparationReadsDisplaySizeAndFirstFrame() {
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }

#ifdef Q_OS_MACOS
        const QSize dimensions = MacVideoThumbnailer::videoDimensions(fixture);
        const QImage firstFrame = MacVideoThumbnailer::firstFrame(fixture);
#elif defined(Q_OS_WIN)
        const QSize dimensions = WindowsVideoThumbnailer::videoDimensions(fixture);
        const QImage firstFrame = WindowsVideoThumbnailer::firstFrame(fixture);
#else
        QSKIP("Native first-frame preparation is implemented on macOS and Windows");
#endif
        QCOMPARE(dimensions, QSize(1920, 1080));
        QVERIFY(!firstFrame.isNull());
        QCOMPARE(firstFrame.size(), dimensions);
    }

    void quickVideoOutputRestoresAudioAndAvoidsContinuousReadback() {
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }

        QGraphicsScene scene;
        QuickCanvasController controller;
        QWidget host;
        host.resize(960, 600);

        QString error;
        QVERIFY2(controller.initialize(&host, &error), qPrintable(error));
        auto* quickWidget = qobject_cast<QQuickWidget*>(controller.widget());
        QVERIFY(quickWidget);
        quickWidget->resize(host.size());
        controller.setMediaScene(&scene);

        auto* video = new ResizableVideoItem(fixture, QSize(1920, 1080),
                                             8, 16, QStringLiteral("video-1080p.mp4"));
        video->setSourcePath(fixture);
        video->setVolume(0.31);
        scene.addItem(video);
        QCOMPARE(video->baseSizePx(), QSize(1920, 1080));

        QMediaPlayer* player = video->mediaPlayer();
        QVERIFY(player);
        QAudioOutput* audio = player->audioOutput();
        QVERIFY(audio);

        host.show();
        quickWidget->show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        // VideoItem.qml must own the active sink; this is the exact path that
        // used to strand first-frame priming in its temporary muted state.
        QTRY_VERIFY_WITH_TIMEOUT(player->videoSink() != nullptr, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(player->videoSink() != video->videoSink(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(player->hasAudio(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(!player->audioTracks().isEmpty(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(video->firstFramePrimed(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() != QMediaPlayer::PlayingState, 3000);
        QVERIFY(!video->isPlaying());
        QCOMPARE(audio->isMuted(), video->isMuted());
        QVERIFY(!audio->isMuted());
        QVERIFY(qAbs(audio->volume() - 0.31) < 0.02);
        QTRY_VERIFY_WITH_TIMEOUT(!publishedMedia(quickWidget->rootObject(),
                                                 video->mediaId()).isEmpty(), 3000);
        const QVariantMap initialModel = publishedMedia(quickWidget->rootObject(),
                                                        video->mediaId());
        QCOMPARE(initialModel.value("width").toInt(), 1920);
        QCOMPARE(initialModel.value("height").toInt(), 1080);
        QCOMPARE(video->baseSizePx(), QSize(1920, 1080));

        // A burst issues the first seek immediately and retains only the final
        // target while that frame is in flight. The paused state is preserved.
        const int pausedRequestsBefore = video->scrubSeekRequestsIssued();
        video->beginScrub(0.10);
        video->updateScrub(0.25);
        video->updateScrub(0.55);
        video->updateScrub(0.80);
        video->endScrub(0.83);
        QCOMPARE(video->scrubSeekRequestsIssued() - pausedRequestsBefore, 1);
        QVERIFY(video->scrubSeekInFlight());
        QVERIFY(video->isDraggingProgress());
        QTRY_VERIFY_WITH_TIMEOUT(!video->isDraggingProgress(), 4000);
        QVERIFY(!video->scrubSeekInFlight());
        QVERIFY(!video->isPlaying());
        QVERIFY(player->playbackState() != QMediaPlayer::PlayingState);
        const qint64 pausedTarget = qRound64(player->duration() * 0.83);
        QVERIFY2(qAbs(video->displayedFrameTimestampMs() - pausedTarget) <= 100,
                 qPrintable(QStringLiteral("Final paused scrub frame %1ms, target %2ms")
                                .arg(video->displayedFrameTimestampMs()).arg(pausedTarget)));
        QVERIFY(video->scrubSeekRequestsIssued() - pausedRequestsBefore <= 2);
        QCOMPARE(video->baseSizePx(), QSize(1920, 1080));

        video->resetFrameStats();
        const qint64 initialPosition = player->position();
        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(player->playbackState(), QMediaPlayer::PlayingState, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > initialPosition + 500, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(video->displayedFrameTimestampMs() > initialPosition + 300, 5000);
        QVERIFY(!audio->isMuted());

        // A playing video is paused technically during scrub and resumes only
        // after the final target frame has acknowledged the gesture.
        const int playingRequestsBefore = video->scrubSeekRequestsIssued();
        video->beginScrub(0.20);
        video->updateScrub(0.42);
        video->updateScrub(0.68);
        video->endScrub(0.71);
        QVERIFY(video->isPlaying()); // logical user intent is retained
        QVERIFY(player->playbackState() != QMediaPlayer::PlayingState || audio->isMuted());
        QTRY_VERIFY_WITH_TIMEOUT(!video->isDraggingProgress(), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(player->playbackState(), QMediaPlayer::PlayingState, 3000);
        QVERIFY(video->scrubSeekRequestsIssued() - playingRequestsBefore <= 2);
        QVERIFY(!audio->isMuted());
        QCOMPARE(video->baseSizePx(), QSize(1920, 1080));

        // Let enough 1080p frames pass to distinguish GPU rendering from an
        // accidental per-frame QVideoFrame -> QImage conversion loop.
        QTest::qWait(1200);
        int received = 0;
        int processed = 0;
        int skipped = 0;
        int dropped = 0;
        int conversionFailures = 0;
        video->getFrameStatsExtended(received, processed, skipped, dropped, conversionFailures);
        QVERIFY2(received >= 10, qPrintable(QStringLiteral("Only %1 frames received").arg(received)));
        // Both successful and failed conversions are CPU/GPU readback attempts.
        // Allow a small startup tail after reset, but do not require native GPU
        // frames to be CPU-mappable on every multimedia backend.
        QVERIFY2(processed + dropped <= 2,
                 qPrintable(QStringLiteral("Quick path attempted too many readbacks: "
                                            "processed=%1, dropped=%2, skipped=%3, received=%4")
                                .arg(processed).arg(dropped).arg(skipped).arg(received)));
        QCOMPARE(conversionFailures, dropped);

        video->setMuted(true, true);
        QCOMPARE(audio->isMuted(), true);
        video->setMuted(false, true);
        QCOMPARE(audio->isMuted(), false);

        QPointer<QVideoSink> quickSink = player->videoSink();
        const qint64 positionBeforeSuspend = player->position();
        video->setApplicationSuspended(true);
        QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() != QMediaPlayer::PlayingState, 3000);
        video->setApplicationSuspended(false);
        QTRY_COMPARE_WITH_TIMEOUT(player->playbackState(), QMediaPlayer::PlayingState, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(player->videoSink() == quickSink, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > positionBeforeSuspend + 250, 5000);
        QVERIFY(!audio->isMuted());

        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(), 3000);
    }

    void suspendDuringInitialPlaybackDoesNotStrandDecoderOrAudio() {
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }

        QGraphicsScene scene;
        QuickCanvasController controller;
        QWidget host;
        host.resize(640, 400);

        QString error;
        QVERIFY2(controller.initialize(&host, &error), qPrintable(error));
        auto* quickWidget = qobject_cast<QQuickWidget*>(controller.widget());
        QVERIFY(quickWidget);
        quickWidget->resize(host.size());
        controller.setMediaScene(&scene);

        auto* video = new ResizableVideoItem(fixture, QSize(1920, 1080),
                                             8, 16, QStringLiteral("video-1080p.mp4"));
        video->setSourcePath(fixture);
        video->setVolume(0.27);
        scene.addItem(video);

        QMediaPlayer* player = video->mediaPlayer();
        QVERIFY(player);
        QAudioOutput* audio = player->audioOutput();
        QVERIFY(audio);

        // Exercise the narrow race where the application is suspended after
        // an intentional play request but before the first decoded frame.
        video->togglePlayPause();
        QVERIFY(video->isPlaying());
        video->setApplicationSuspended(true);
        QVERIFY(video->isPlaying()); // logical intent is preserved while paused technically
        QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() != QMediaPlayer::PlayingState, 3000);

        host.show();
        quickWidget->show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        video->setApplicationSuspended(false);

        QTRY_VERIFY_WITH_TIMEOUT(player->videoSink() != nullptr, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(video->firstFramePrimed(), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(player->playbackState(), QMediaPlayer::PlayingState, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > 300, 5000);
        QCOMPARE(audio->isMuted(), video->isMuted());
        QVERIFY(!audio->isMuted());
        QVERIFY(qAbs(audio->volume() - 0.27) < 0.02);

        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(), 3000);
    }

    void muteDuringPrimingAndSuspendedErrorRemainTerminal() {
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }

        QGraphicsScene scene;
        auto* video = new ResizableVideoItem(fixture, QSize(1920, 1080),
                                             8, 16, QStringLiteral("video-1080p.mp4"));
        video->setSourcePath(fixture);
        scene.addItem(video);

        QMediaPlayer* player = video->mediaPlayer();
        QVERIFY(player);
        QAudioOutput* audio = player->audioOutput();
        QVERIFY(audio);

        // Keep the technical prime pending deterministically: audio may run,
        // but without a video sink no first-frame callback can complete it.
        player->setVideoSink(nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            player->mediaStatus() == QMediaPlayer::LoadedMedia
                || player->mediaStatus() == QMediaPlayer::BufferedMedia,
            8000);
        QTRY_VERIFY_WITH_TIMEOUT(audio->isMuted(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(player->playbackState(), QMediaPlayer::PlayingState, 5000);
        QVERIFY(!video->firstFramePrimed());
        QVERIFY(!video->isPlaying());

        video->setMuted(true, true);
        QVERIFY(video->isMuted());
        QVERIFY(audio->isMuted());
        video->setMuted(false, true);
        QVERIFY(!video->isMuted());
        QVERIFY(audio->isMuted()); // technical mute must not leak audio

        video->setApplicationSuspended(true);
        QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() != QMediaPlayer::PlayingState, 3000);

        // Emit a deterministic backend-style terminal error while the source
        // itself remains loaded. This makes any erroneous resume/re-prime
        // immediately observable as a transition back to PlayingState.
        player->errorOccurred(QMediaPlayer::ResourceError, QStringLiteral("synthetic suspended error"));
        QCOMPARE(video->lastPlaybackError(), QMediaPlayer::ResourceError);
        QVERIFY(!video->isPlaying());
        QCOMPARE(audio->isMuted(), video->isMuted());

        video->setApplicationSuspended(false);
        QTest::qWait(250);
        QVERIFY(player->playbackState() != QMediaPlayer::PlayingState);
        QVERIFY(!video->isPlaying());
        QCOMPARE(audio->isMuted(), video->isMuted());
    }
};

QTEST_MAIN(VideoPlaybackBackendTest)
#include "tst_VideoPlaybackBackend.moc"
