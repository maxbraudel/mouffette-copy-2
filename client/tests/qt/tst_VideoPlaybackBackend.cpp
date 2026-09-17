#include <QApplication>
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QMediaPlayer>
#include <QJsonArray>
#include "frontend/qml/MediaSettingsViewModel.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include <QQuickItem>
#include <QQuickView>
#include <QScopeGuard>
#include <QVideoSink>
#include <QtTest>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "backend/media/MediaDecoder.h"
#include "backend/media/MediaResidencyManager.h"

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
    void init()
    {
        MediaResidencyManager::instance().setMemorySnapshotForTesting(
            {8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
    }

    void cleanup()
    {
        MediaResidencyManager::instance().clearMemorySnapshotForTesting();
    }

    void suspensionReleasesVideoAndPreservesPreview_data()
    {
        QTest::addColumn<bool>("whileLoading");
        QTest::newRow("loaded") << false;
        QTest::newRow("audio-discovery-pending") << true;
    }

    void suspensionReleasesVideoAndPreservesPreview()
    {
        QFETCH(bool, whileLoading);
        CanvasDocument document;
        auto* video = document.addPreparedFile(videoFixture(), QSize(160, 90), true, {12, 34});
        QVERIFY(video && video->player());
        auto* player = video->player();
        video->setMuted(true);
        video->setVolume(0.27);
        video->setPositionMs(1234);
        QVERIFY(video->setPlaybackRange(1000, 2400));
        if (!whileLoading)
            QTRY_VERIFY(player->asset() && video->hasRenderedFrame());
        std::weak_ptr<const ResidentMediaAsset> asset = player->asset();
        const QJsonObject saved = document.serializeProjectState();

        document.setMediaResidencySuspended(true);
        QVERIFY(!video->residencyReady());
        QVERIFY(!player->asset());
        QTRY_VERIFY(asset.expired());
        QVERIFY(!video->isPlaying());
        QVERIFY(!video->hasRenderedFrame());
        QVERIFY(!video->firstFramePrimed());
        if (video->videoSink()) QVERIFY(!video->videoSink()->videoFrame().isValid());
        QCOMPARE(video->positionMs(), 1234);
        QCOMPARE(document.serializeProjectState(), saved);

        // A completed device future cannot recreate sinks or decode assets
        // after suspension, but must be restartable on the next activation.
        QTRY_VERIFY(video->findChildren<QFutureWatcherBase*>().isEmpty());
        if (whileLoading) {
            QVERIFY(!video->audioOutput());
            QVERIFY(!video->videoSink());
        }
        QVERIFY(!player->asset());
        document.setMediaResidencySuspended(false);
        QTRY_VERIFY(player->asset() && video->hasRenderedFrame());
        QCOMPARE(video->player(), player);
        QCOMPARE(video->positionMs(), 1234);
        QVERIFY(!video->isPlaying());
        QVERIFY(video->muted());
        QCOMPARE(video->volume(), 0.27);
        QCOMPARE(video->startMarkerMs(), 1000);
        QCOMPARE(video->endMarkerMs(), 2400);
    }

    void restoringSuspendedVideoRetainsPendingCursor()
    {
        CanvasDocument original;
        auto* source = original.addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(source);
        source->setPositionMs(1789);
        const QJsonObject saved = original.serializeProjectState();
        CanvasDocument restored;
        restored.setMediaResidencySuspended(true);
        QVERIFY(restored.restoreProjectState(saved, {{source->mediaId(), videoFixture()}}));
        auto* video = restored.mediaById(source->mediaId());
        QVERIFY(video);
        QVERIFY(video->residencySuspended());
        QVERIFY(!video->player());
        QCOMPARE(video->positionMs(), 1789);
        restored.setMediaResidencySuspended(false);
        QTRY_VERIFY(video->player() && video->player()->asset());
        QCOMPARE(video->positionMs(), 1789);
        QVERIFY(!video->isPlaying());
    }

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
        QSignalSpy states(player, &ResidentVideoPlayer::playbackStateChanged);
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
        connect(video->player(), &ResidentVideoPlayer::positionChanged, &document,
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
            QTRY_COMPARE_WITH_TIMEOUT(video->positionMs(), qMax<qint64>(0, start), 5000);
            // The first stop can cancel an in-flight source load. A subsequent
            // scene must replace its queued preview seek and actually prime.
            if (start >= 0) {
                QTRY_VERIFY_WITH_TIMEOUT(video->player()->preparedAt(start), 5000);
                QVERIFY(host->testSceneLaunched());
                QCOMPARE(video->positionMs(), start);
            }
            host->triggerTestSceneAction();
            QCOMPARE(video->positionMs(), 2200);
        }
    }

    void reportedDelayedVideoCanLaunchAndRelaunch()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* video = host->document()->addPreparedFile(
            QString::fromUtf8(TEST_REPORTED_VIDEO_FILE), QSize(720, 1280), true, {});
        QVERIFY(video);
        QTRY_VERIFY2_WITH_TIMEOUT(video->residencyReady(), qPrintable(MediaResidencyManager::instance().errorString(video->residencyOwnerId())), 10000);
        auto settings = video->settings();
        settings.unmuteAutomatically = false;
        video->setSettings(settings);
        video->setMuted(true);
        for (int attempt = 0; attempt < 3; ++attempt) {
            video->setPositionMs(2200);
            host->triggerTestSceneAction();
            QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 5000);
            QVERIFY(host->testSceneLaunched());
            QTRY_VERIFY_WITH_TIMEOUT(video->positionMs() > 100, 2000);
            if (attempt == 1) {
                // A decoder failure during a scene must unlock and restore the
                // draft, without destroying players inside their signal stack.
                emit video->player()->errorOccurred(QMediaPlayer::FormatError, "simulated decoder failure");
                QVERIFY(host->testSceneLaunched());
                QTRY_VERIFY_WITH_TIMEOUT(!host->testSceneLaunched(), 1000);
            } else {
                host->triggerTestSceneAction();
            }
            QCOMPARE(video->positionMs(), qint64(2200));
            QVERIFY(!video->isPlaying());
        }
    }

    void testScenePlayAndPauseDelays()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* video = host->document()->addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->player()->duration() > 3000, 5000);
        auto settings = video->settings();
        settings.unmuteAutomatically = false;
        settings.playDelayEnabled = true;
        settings.playDelayText = QStringLiteral("0.3");
        settings.pauseDelayEnabled = true;
        settings.pauseDelayText = QStringLiteral("0.4");
        video->setSettings(settings);
        video->setPositionMs(2200);
        QElapsedTimer playingFor;
        const auto playingConnection = connect(video->player(), &ResidentVideoPlayer::playbackStateChanged, this,
                [&](QMediaPlayer::PlaybackState state) {
            if (state == QMediaPlayer::PlayingState && !playingFor.isValid()) playingFor.start();
        });
        const auto disconnectPlaying = qScopeGuard([playingConnection] {
            QObject::disconnect(playingConnection);
        });

        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QTRY_VERIFY_WITH_TIMEOUT(video->contentVisible() && video->muted(), 5000);
        QVERIFY(!video->isPlaying());
        QTest::qWait(100);
        QVERIFY(!video->isPlaying());
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(), 1500);
        QVERIFY(playingFor.elapsed() >= 330);
        QVERIFY(video->positionMs() >= 250);
        QVERIFY(video->positionMs() < 700);
        const qint64 pausedAt = video->positionMs();
        QTest::qWait(200);
        QCOMPARE(video->positionMs(), pausedAt);
        host->triggerTestSceneAction();
        QCOMPARE(video->positionMs(), 2200);
        QVERIFY(!video->isPlaying());
    }

    void testSceneAudioDelaysAndFadesPreserveConfiguredVolume()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* video = host->document()->addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->audioOutput(), 5000);
        auto settings = video->settings();
        settings.playAutomatically = false;
        settings.volumeOverrideEnabled = true;
        settings.volumeText = QStringLiteral("37");
        settings.unmuteDelayEnabled = true;
        settings.unmuteDelayText = QStringLiteral("0.25");
        settings.audioFadeInEnabled = true;
        settings.audioFadeInText = QStringLiteral("0.3");
        settings.muteDelayEnabled = true;
        settings.muteDelayText = QStringLiteral("1");
        settings.audioFadeOutEnabled = true;
        settings.audioFadeOutText = QStringLiteral("0.3");
        video->setSettings(settings);
        video->setMuted(false);
        auto* audio = video->audioOutput();

        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QTRY_VERIFY_WITH_TIMEOUT(video->contentVisible() && video->muted(), 5000);
        QTest::qWait(100);
        QVERIFY(video->muted());
        QTRY_VERIFY_WITH_TIMEOUT(!video->muted() && audio->volume() > 0.02
            && audio->volume() < 0.35, 1000);
        QCOMPARE(video->volume(), 0.37);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(audio->volume() - 0.37) < 0.001, 1000);
        QVERIFY(!video->muted());
        QTRY_VERIFY_WITH_TIMEOUT(video->muted() && !audio->isMuted()
            && audio->volume() > 0.02 && audio->volume() < 0.35, 1000);
        QCOMPARE(video->volume(), 0.37);
        QTRY_VERIFY_WITH_TIMEOUT(audio->isMuted(), 1000);
        QVERIFY(video->muted());
        QVERIFY(audio->volume() < 0.001);
        QCOMPARE(video->volume(), 0.37);

        host->triggerTestSceneAction();
        QVERIFY(!video->muted());
        QVERIFY(qAbs(audio->volume() - 0.37) < 0.001);
        QCOMPARE(video->volume(), 0.37);
    }

    void testSceneDisabledAutomaticActionsStayDisabled()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* video = host->document()->addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->audioOutput(), 5000);
        auto settings = video->settings();
        settings.displayAutomatically = false;
        settings.playAutomatically = false;
        settings.unmuteAutomatically = false;
        settings.displayDelayEnabled = true;
        settings.displayDelayText = QStringLiteral("0.1");
        settings.playDelayEnabled = true;
        settings.playDelayText = QStringLiteral("0.1");
        settings.unmuteDelayEnabled = true;
        settings.unmuteDelayText = QStringLiteral("0.1");
        settings.fadeInEnabled = true;
        settings.fadeInText = QStringLiteral("0.2");
        settings.audioFadeInEnabled = true;
        settings.audioFadeInText = QStringLiteral("0.2");
        video->setSettings(settings);
        video->setMuted(false);
        video->setPositionMs(2200);

        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QTRY_VERIFY_WITH_TIMEOUT(!video->contentVisible() && video->muted(), 5000);
        QTest::qWait(600);
        QVERIFY(!video->contentVisible());
        QCOMPARE(video->animatedDisplayOpacity(), 0.0);
        QVERIFY(!video->isPlaying());
        QCOMPARE(video->positionMs(), 0);
        QVERIFY(video->muted());
        host->triggerTestSceneAction();
        QVERIFY(video->contentVisible());
        QVERIFY(!video->muted());
        QCOMPARE(video->positionMs(), 2200);
    }

    void testSceneEndActionsWaitForLastRepeat_data()
    {
        QTest::addColumn<bool>("explicitEndMarker");
        QTest::addColumn<int>("endDelayMs");
        QTest::newRow("marker-before-end") << true << -250;
        QTest::newRow("marker-at-end") << true << 0;
        QTest::newRow("marker-after-end") << true << 250;
        QTest::newRow("natural-before-end") << false << -250;
        QTest::newRow("natural-at-end") << false << 0;
        QTest::newRow("natural-after-end") << false << 250;
    }

    void testSceneEndActionsWaitForLastRepeat()
    {
        QFETCH(bool, explicitEndMarker);
        QFETCH(int, endDelayMs);
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* video = host->document()->addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->audioOutput()
            && video->player()->duration() > 3000, 5000);
        const qint64 end = explicitEndMarker ? 1600 : video->player()->duration();
        const qint64 start = end - 700;
        QVERIFY(video->setPlaybackRange(start, explicitEndMarker ? end : -1));
        auto settings = video->settings();
        settings.repeatEnabled = true;
        settings.repeatCountText = QStringLiteral("1");
        settings.hideWhenVideoEnds = true;
        settings.muteWhenVideoEnds = true;
        settings.hideDelayEnabled = true;
        settings.hideDelayText = QString::number(endDelayMs / 1000.0);
        settings.muteDelayEnabled = true;
        settings.muteDelayText = settings.hideDelayText;
        settings.fadeOutEnabled = true;
        settings.fadeOutText = QStringLiteral("0.1");
        settings.audioFadeOutEnabled = true;
        settings.audioFadeOutText = QStringLiteral("0.1");
        video->setSettings(settings);
        int wraps = 0;
        qint64 previous = start;
        const auto wrapConnection = connect(video->player(), &ResidentVideoPlayer::positionChanged, this, [&](qint64 position) {
            if (previous > start + 350 && position == start) ++wraps;
            previous = position;
        });
        const auto disconnectWrap = qScopeGuard([wrapConnection] {
            QObject::disconnect(wrapConnection);
        });
        qint64 hiddenAtPosition = -1;
        qint64 mutedAtPosition = -1;
        const auto hiddenConnection = connect(video, &CanvasMedia::changed, this, [&] {
            if (wraps > 0 && !video->contentVisible() && hiddenAtPosition < 0)
                hiddenAtPosition = video->positionMs();
        });
        const auto mutedConnection = connect(video->audioOutput(), &QAudioOutput::mutedChanged, this, [&](bool muted) {
            if (wraps > 0 && muted && mutedAtPosition < 0) mutedAtPosition = video->positionMs();
        });
        const auto disconnectEndActions = qScopeGuard([hiddenConnection, mutedConnection] {
            QObject::disconnect(hiddenConnection);
            QObject::disconnect(mutedConnection);
        });

        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying() && video->contentVisible() && !video->muted(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(wraps, 1, 3000);
        // Reaching the first end, including a natural EOF, must only repeat.
        QVERIFY(video->contentVisible());
        QVERIFY(!video->muted());
        if (endDelayMs > 0) {
            QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying() && video->positionMs() == end, 3000);
            QVERIFY(video->contentVisible());
            QVERIFY(!video->muted());
        }
        QTRY_VERIFY_WITH_TIMEOUT(!video->contentVisible() && video->audioOutput()->isMuted(), 3000);
        QVERIFY(video->muted());
        QCOMPARE(wraps, 1);
        if (endDelayMs < 0) {
            QVERIFY(hiddenAtPosition >= end + endDelayMs);
            QVERIFY(hiddenAtPosition < end);
            QVERIFY(mutedAtPosition >= end + endDelayMs);
            QVERIFY(mutedAtPosition < end);
        } else {
            QCOMPARE(hiddenAtPosition, end);
            QCOMPARE(mutedAtPosition, end);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying() && video->positionMs() == end, 3000);
        host->triggerTestSceneAction();
        QVERIFY(video->contentVisible());
        QVERIFY(!video->muted());
        QCOMPARE(video->animatedDisplayOpacity(), 1.0);
    }

    void testSceneStopCancelsVideoTimersAndAudioFade_data()
    {
        QTest::addColumn<bool>("stopDuringFade");
        QTest::newRow("pending-unmute-and-play") << false;
        QTest::newRow("active-audio-fade") << true;
    }

    void testSceneStopCancelsVideoTimersAndAudioFade()
    {
        QFETCH(bool, stopDuringFade);
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        auto* video = host->document()->addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->audioOutput(), 5000);
        auto settings = video->settings();
        settings.volumeOverrideEnabled = true;
        settings.volumeText = QStringLiteral("37");
        settings.playDelayEnabled = true;
        settings.playDelayText = QStringLiteral("1");
        settings.pauseDelayEnabled = true;
        settings.pauseDelayText = QStringLiteral("0.3");
        settings.unmuteDelayEnabled = true;
        settings.unmuteDelayText = QStringLiteral("0.25");
        settings.audioFadeInEnabled = true;
        settings.audioFadeInText = QStringLiteral("0.6");
        settings.muteDelayEnabled = true;
        settings.muteDelayText = QStringLiteral("1");
        settings.audioFadeOutEnabled = true;
        settings.audioFadeOutText = QStringLiteral("0.6");
        video->setSettings(settings);
        video->setMuted(false);
        video->setContentVisible(false);
        video->setPositionMs(2200);
        auto* audio = video->audioOutput();
        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QTRY_VERIFY_WITH_TIMEOUT(video->contentVisible() && video->muted(), 5000);
        if (stopDuringFade) {
            QTRY_VERIFY_WITH_TIMEOUT(!video->muted() && audio->volume() > 0.02
                && audio->volume() < 0.35, 1500);
        }
        host->triggerTestSceneAction();
        QVERIFY(!video->contentVisible());
        QVERIFY(!video->muted());
        QVERIFY(!video->isPlaying());
        QCOMPARE(video->positionMs(), 2200);
        QVERIFY(qAbs(audio->volume() - 0.37) < 0.001);

        settings.displayAutomatically = false;
        settings.playAutomatically = false;
        settings.unmuteAutomatically = false;
        settings.muteDelayEnabled = false;
        video->setSettings(settings);
        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QTRY_VERIFY_WITH_TIMEOUT(video->muted(), 5000);
        bool unmutedUnexpectedly = false;
        const auto unmuteConnection = connect(audio, &QAudioOutput::mutedChanged, this, [&](bool muted) {
            unmutedUnexpectedly |= !muted;
        });
        const auto disconnectUnmute = qScopeGuard([unmuteConnection] {
            QObject::disconnect(unmuteConnection);
        });
        QTest::qWait(1700);
        QVERIFY(!unmutedUnexpectedly);
        QVERIFY(!video->contentVisible());
        QVERIFY(video->muted());
        QVERIFY(!video->isPlaying());
        QCOMPARE(video->positionMs(), 0);
        QCOMPARE(video->volume(), 0.37);
        host->triggerTestSceneAction();
        QVERIFY(!video->contentVisible());
        QVERIFY(!video->muted());
        QCOMPARE(video->positionMs(), 2200);
        QVERIFY(qAbs(audio->volume() - 0.37) < 0.001);
    }

    void qmlVideoOutputUsesDocumentRuntimeAndPreservesAudioState()
    {
        const QString fixture = videoFixture();
        if (!QFile::exists(fixture)) QSKIP("Optional real-video fixture is missing");

        CanvasDocument document;
        QuickCanvasController controller(&document);
        QString error;
        QVERIFY2(controller.initialize(&error), qPrintable(error));
        const QSize nativeSize = MediaDecoder::probe(fixture).displaySize;
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

        ResidentVideoPlayer* player = video->player();
        QTRY_VERIFY_WITH_TIMEOUT(video->audioOutput(), 8000);
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

    void droppedVideoFullyLoadsBeforePlayback()
    {
        const QString fixture = videoFixture();
        if (!QFile::exists(fixture)) QSKIP("Optional real-video fixture is missing");

        const QSize nativeSize = MediaDecoder::probe(fixture).displaySize;
        QVERIFY(nativeSize.isValid());

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
        QVERIFY(document.media().isEmpty());

        QVERIFY(controller.commitLocalFileDrop(480, 300));
        QTRY_COMPARE(document.media().size(), 1);
        CanvasMedia* video = document.selectedMedia();
        QVERIFY(video && video->isVideo() && video->player());
        QCOMPARE(video->baseSize(), nativeSize);
        QTRY_VERIFY_WITH_TIMEOUT(video->player()->mediaStatus()
                                     == QMediaPlayer::LoadedMedia
                                 || video->player()->mediaStatus()
                                     == QMediaPlayer::BufferedMedia,
                                 8000);
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady(), 30000);
        QVERIFY(!video->isPlaying());

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
        QVERIFY(video->residencyReady());

        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(video->positionMs() > 100, 5000);
        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(), 3000);
    }
};

QTEST_MAIN(VideoPlaybackBackendTest)
#include "tst_VideoPlaybackBackend.moc"
