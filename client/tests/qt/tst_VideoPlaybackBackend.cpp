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
#include "shared/rendering/MediaFrameSource.h"

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
    void authoringPreviewDoesNotAuthorizeRemoteOrPlayback()
    {
        CanvasDocument document;
        QuickCanvasController controller(&document);
        QString error;
        QVERIFY2(controller.initialize(&error), qPrintable(error));
        RemoteVideoFrameSource preview;
        QImage image(160, 90, QImage::Format_RGB32); image.fill(Qt::cyan);
        preview.setFrame(image);
        QQuickView view;
        view.resize(160, 90);
        view.setInitialProperties({{"mediaWidth", 160}, {"mediaHeight", 90},
            {"previewFrameSource", QVariant::fromValue<QObject*>(&preview)}, {"residencyReady", false}});
        view.setSource(QUrl("qrc:/qt/qml/Mouffette/App/resources/qml/VideoItem.qml"));
        QCOMPARE(view.status(), QQuickView::Ready);
        view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto* item = view.rootObject(); QVERIFY(item);
        QTRY_VERIFY(item->property("contentReady").toBool());
        QVERIFY(!item->property("residencyReady").toBool());
        QVERIFY(!item->property("cppMediaPlayer").value<QObject*>());
        auto* loader = item->findChild<QQuickItem*>("videoImportPreview"); QVERIFY(loader);
        QTRY_VERIFY(loader->property("active").toBool());
        QTRY_VERIFY(!view.grabWindow().isNull());
        RemoteVideoFrameSource remote;
        remote.setFrame(image);
        item->setProperty("remoteFrameSource", QVariant::fromValue<QObject*>(&remote));
        QTRY_VERIFY(!item->property("contentReady").toBool());
        QVERIFY(!loader->property("active").toBool());
        item->setProperty("remoteFrameSource", QVariant::fromValue<QObject*>(nullptr));
        QTRY_VERIFY(item->property("contentReady").toBool());
        preview.clear();
        QTRY_VERIFY(!item->property("contentReady").toBool());
    }
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
        SceneTimeline::MediaTrack track;
        track.clip={"clip",30,30,42};
        video->setTimelineTrack(track);
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
        QCOMPARE(video->timelineTrack().clip.sourceStartSlot.value(),30);
        QCOMPARE(video->timelineTrack().clip.sourceEndSlot(),72);
    }

    void restoredVideoHasNoPersistedTransportCursor()
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
        QCOMPARE(video->positionMs(), 0);
        restored.setMediaResidencySuspended(false);
        QTRY_VERIFY(video->player() && video->player()->asset());
        QCOMPARE(video->positionMs(), 0);
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

    void scrubbingPresentsDuringDragAndSettlesOnRelease()
    {
        CanvasDocument document;
        auto* video = document.addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video && video->player());
        auto* player = video->player();
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady(), 10000);
        video->setPositionMs(500);
        QTRY_VERIFY_WITH_TIMEOUT(player->preparedAt(500), 10000);
        QSignalSpy frames(video->videoSink(), &QVideoSink::videoFrameChanged);
        player->setScrubbing(true);
        // A burst retains one decode in flight and the latest source cursor.
        for (qint64 target = 600; target <= 1600; target += 10)
            player->setPosition(target);
        QCOMPARE(player->position(), qint64(1600));
        QTRY_VERIFY_WITH_TIMEOUT(video->videoSink()->videoFrame().startTime() <= 1600000
            && video->videoSink()->videoFrame().endTime() > 1600000, 10000);
        QVERIFY(player->preparedAt(1600)); // The pointer target is visible before release.
        QVERIFY(!frames.isEmpty()); // preview arrives before releasing the pointer
        QCOMPARE(player->position(), qint64(1600));
        player->setPosition(789);
        player->setScrubbing(false); // Prepare the exact release position and its lookahead.
        QTRY_VERIFY_WITH_TIMEOUT(player->preparedAt(789), 10000);
        QCOMPARE(player->position(), qint64(789));
        QVERIFY(!player->isPlaying());
        QTest::qWait(50);
        QVERIFY(player->preparedAt(789));
    }

    void seekAfterPauseAndSourceReload()
    {
        CanvasDocument document;
        auto* video = document.addPreparedFile(videoFixture(), QSize(160, 90), true, {});
        QVERIFY(video);
        video->setMuted(true);
        QTRY_VERIFY(video->player()->duration() > 3000);
        if(video->isPlaying()) video->player()->pause(); else video->player()->play();
        QTRY_VERIFY(video->positionMs() > 100);
        if(video->isPlaying()) video->player()->pause(); else video->player()->play();
        video->setPositionMs(1789);
        QTest::qWait(700);
        QVERIFY(qAbs(video->positionMs() - 1789) <= 1);
        QVERIFY(!video->isPlaying());

        video->setSourcePath(QString());
        video->setSourcePath(videoFixture());
        QTRY_VERIFY(video->player()->duration() > 3000 && video->player()->isSeekable());
        video->setPositionMs(qRound64(video->player()->duration()*0.1234));
        const qint64 target = qRound64(video->player()->duration() * 0.1234);
        QTest::qWait(700);
        QVERIFY(qAbs(video->positionMs() - target) <= 1);
        QVERIFY(!video->isPlaying());
        QVERIFY(video->muted());
    }






























    void timelineClipsPersistAndScrubbingDoesNotChangeTheProject()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);host->setProjectEditingEnabled(true);
        auto* video=host->document()->addPreparedFile(videoFixture(),QSize(160,90),true,{});
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->player()->duration()>3000,5000);
        SceneTimeline::MediaTrack track;
        track.clip={"second",42,60,15};
        video->setTimelineTrack(track);
        const auto saved=host->serializeProjectState();
        QSignalSpy writes(host->document(),&CanvasDocument::documentChanged);
        host->timelineSeek(0);QVERIFY(!video->clipActive());
        host->timelineSeek(1200);QVERIFY(!video->clipActive());
        host->timelineSeek(1500);QTRY_COMPARE(video->positionMs(),qint64(2100));
        QCOMPARE(writes.count(),0);QCOMPARE(host->serializeProjectState(),saved);
        CanvasDocument restored;restored.setMediaResidencySuspended(true);
        QVERIFY(restored.restoreProjectState(saved,{{video->mediaId(),videoFixture()}}));
        QCOMPARE(restored.mediaById(video->mediaId())->timelineTrack().toJson(),track.toJson());
        QCOMPARE(restored.timelinePositionMs(),0);
        QVERIFY(!saved.value("media").toArray()[0].toObject().contains("previewPositionMs"));
    }

    void timelinePlaybackHidesOutsideClipAndStopsAtSceneEnd()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);host->setProjectEditingEnabled(true);
        auto* video=host->document()->addPreparedFile(videoFixture(),QSize(160,90),true,{});
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->player()->duration()>3000,5000);
        video->setMuted(true);
        SceneTimeline::MediaTrack track;track.clip={"clip",11,15,15};
        video->setTimelineTrack(track);
        auto settings=host->document()->timelineSettings();settings.stopSlot=36;QVERIFY(host->document()->setTimelineSettings(settings));
        host->timelineSeek(0);QVERIFY(!video->clipActive());
        QVERIFY(!video->isPlaying());host->timelinePlay();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(),3000);
        QTRY_VERIFY_WITH_TIMEOUT(!host->timelinePlaying(),3000);
        QCOMPARE(host->timelinePositionMs(),qint64(1200));
        QVERIFY(!video->clipActive());
        QVERIFY(video->audioOutput()->isMuted());
    }

    void timelineSeeksKeepVideoPlaybackAndReservations_data()
    {
        QTest::addColumn<bool>("duringPreparation");
        QTest::addColumn<bool>("scrubbing");
        QTest::newRow("running") << false << false;
        QTest::newRow("preparing") << true << false;
        QTest::newRow("scrub-running") << false << true;
        QTest::newRow("scrub-preparing") << true << true;
    }

    void timelineSeeksKeepVideoPlaybackAndReservations()
    {
        QFETCH(bool, duringPreparation);
        QFETCH(bool, scrubbing);
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host); host->setProjectEditingEnabled(true);
        auto* doc = host->document();
        auto* video = doc->addPreparedFile(videoFixture(), {160, 90}, true, {});
        QVERIFY(video);
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->audioOutput(), 5000);
        auto track = video->timelineTrack();
        track.clip.startSlot = 90; // Timeline 3–9 s, starting at source 1 s.
        track.clip.durationSlots = 180;
        track.clip.sourceStartSlot = 30;
        video->setTimelineTrack(track);
        video->setMuted(false);
        video->setVolume(0); // Exercise mute state without audible test playback.
        host->timelineSeek(4000);
        const auto saved = doc->serializeProjectState();
        host->timelinePlay();
        QVERIFY(host->testSceneLaunched());
        if (duringPreparation) QVERIFY(!host->timelinePlaying());
        else QTRY_VERIFY_WITH_TIMEOUT(host->timelinePlaying(), 5000);
        QSignalSpy locks(doc, &CanvasDocument::editsLockedChanged);
        const auto playbackBudget = MediaResidencyManager::instance().summary().value("playbackBudgetBytes");
        QVERIFY(playbackBudget.toULongLong() > 0);
        if (scrubbing) host->timelineBeginScrub();
        // Rapid requests include a gap after the clip while start frames may
        // still be preparing. Only the last requested position should win.
        host->timelineSeek(10000);
        host->timelineSeek(6000);
        QVERIFY(host->testSceneLaunched());
        if (scrubbing) {
            QTest::qWait(150);
            QCOMPARE(doc->timelinePositionMs(), 6000);
            QVERIFY(!host->timelinePlaying());
            QVERIFY(!video->isPlaying());
            QVERIFY(video->audioOutput()->isMuted());
            QVERIFY(doc->editsLocked());
            QCOMPARE(MediaResidencyManager::instance().summary().value("playbackBudgetBytes"), playbackBudget);
            host->timelineEndScrub();
        }
        QTRY_VERIFY_WITH_TIMEOUT(host->timelinePlaying() && video->isPlaying(), 5000);
        QVERIFY(doc->timelinePositionMs() >= 6000 && doc->timelinePositionMs() < 7000);
        const auto followsTimeline = [&] {
            return qAbs(video->positionMs() - (doc->timelinePositionMs() - 2000)) < 300;
        };
        QTRY_VERIFY(followsTimeline());
        QVERIFY(!video->audioOutput()->isMuted());
        host->timelineSeek(1000);
        QVERIFY(host->timelinePlaying());
        QVERIFY(!video->clipActive());
        QVERIFY(!video->isPlaying());
        QVERIFY(video->audioOutput()->isMuted());
        host->timelineSeek(5000);
        QTRY_VERIFY(video->isPlaying() && followsTimeline());
        QVERIFY(!video->audioOutput()->isMuted());
        QTRY_VERIFY(doc->timelinePositionMs() > 5100);
        QVERIFY(doc->timelinePositionMs() < 6000);
        QCOMPARE(locks.count(), 0);
        QCOMPARE(MediaResidencyManager::instance().summary().value("playbackBudgetBytes"), playbackBudget);
        host->timelinePause();
        QVERIFY(video->audioOutput()->isMuted());
        QVERIFY(!video->isPlaying());
        QCOMPARE(doc->serializeProjectState(), saved);
    }

    void timelineAudioUsesKeyframesAndSilencesPausedPreview()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);host->setProjectEditingEnabled(true);
        auto* video=host->document()->addPreparedFile(videoFixture(),QSize(160,90),true,{});
        QTRY_VERIFY_WITH_TIMEOUT(video->residencyReady() && video->audioOutput(),5000);
        auto track=video->timelineTrack();auto a=video->authorElementState(),b=a;
        a.muted=false;a.volume=0;b.muted=false;b.volume=1;
        SceneTimeline::upsertKeyframe(track,{"a",0,a},180000);SceneTimeline::upsertKeyframe(track,{"b",30,b},5400);
        video->setTimelineTrack(track);
        const auto saved=host->serializeProjectState();
        host->timelineSeek(500);QVERIFY(qAbs(video->volume()-.5)<.001);
        QVERIFY(video->audioOutput()->isMuted());QVERIFY(!video->muted());
        host->timelinePlay();QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(!video->audioOutput()->isMuted(),1000);
        host->timelinePause();QVERIFY(video->audioOutput()->isMuted());QVERIFY(!video->isPlaying());
        QCOMPARE(host->serializeProjectState(),saved);
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
        QCOMPARE(player->loops(), QMediaPlayer::Once);

        if(video->isPlaying()) video->player()->pause(); else video->player()->play();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > 250, 5000);
        video->setPositionMs(video->player()->duration()/2);
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(player->position() - player->duration() / 2) < 350, 4000);
        if(video->isPlaying()) video->player()->pause(); else video->player()->play();
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

        QElapsedTimer importing; importing.start();
        QVERIFY(controller.commitLocalFileDrop(480, 300));
        QTRY_COMPARE(document.media().size(), 1);
        CanvasMedia* video = document.selectedMedia();
        QVERIFY(video && video->isVideo() && video->player());
        QCOMPARE(video->baseSize(), nativeSize);
        const auto previewCapture = qEnvironmentVariable("MOUFFETTE_PREVIEW_ARTIFACT");
        if (!previewCapture.isEmpty()) {
            // Opt-in real-source check: the native QML poster must be visible
            // while validation is still in progress, without enabling Play.
            const auto previewItem = [&]() -> QQuickItem* {
                QList<QQuickItem*> pending{view.rootObject()};
                while (!pending.isEmpty()) {
                    auto* item = pending.takeLast();
                    if (item->objectName() == QLatin1String("videoImportPreview")) return item;
                    pending.append(item->childItems());
                }
                return nullptr;
            };
            QQuickItem* preview = nullptr;
            QTRY_VERIFY_WITH_TIMEOUT((preview = previewItem()) && preview->property("active").toBool()
                && preview->property("item").value<QObject*>(), 10000);
            QTRY_VERIFY(preview->property("item").value<QObject*>()->property("hasFrame").toBool());
            auto* surface = preview->parentItem()->parentItem();
            QVERIFY(surface->property("revealProgress").isValid());
            QTRY_COMPARE(surface->property("revealProgress").toReal(), 1.0);
            QSignalSpy presented(&view, &QQuickWindow::frameSwapped);
            view.update();
            QTRY_VERIFY(!presented.isEmpty());
            QVERIFY(!video->residencyReady());
            QVERIFY(!video->player()->asset());
            qInfo() << "Authoring QML preview fully revealed after" << importing.elapsed() << "ms; strict ready false";
            const QImage rendered = view.grabWindow();
            QVERIFY(!rendered.isNull());
            QVERIFY(rendered.save(previewCapture));
            QVERIFY(!video->residencyReady());
        }
        const int importTimeout = qEnvironmentVariableIsSet("MOUFFETTE_TEST_VIDEO_FILE") ? 180000 : 30000;
        QTRY_VERIFY2_WITH_TIMEOUT(video->residencyReady(),
            qPrintable(video->residencyState() + ": " + video->residencyError()), importTimeout);
        QTRY_VERIFY_WITH_TIMEOUT(video->player()->mediaStatus()
                                     == QMediaPlayer::LoadedMedia
                                 || video->player()->mediaStatus()
                                     == QMediaPlayer::BufferedMedia,
                                 8000);
        QTRY_VERIFY_WITH_TIMEOUT(video->player()->videoSink() && video->player()->videoSink()->videoFrame().isValid(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(video->player()->preparedAt(0), 8000);
        const auto loadingSkeleton = [&]() -> QQuickItem* {
            QList<QQuickItem*> pending{view.rootObject()};
            while (!pending.isEmpty()) {
                auto* item = pending.takeLast();
                if (item->objectName() == QLatin1String("mediaLoadingSkeleton")) return item;
                pending.append(item->childItems());
            }
            return nullptr;
        };
        QQuickItem* skeleton = nullptr;
        QTRY_VERIFY(skeleton = loadingSkeleton());
        QTRY_VERIFY(skeleton->parentItem()->property("contentReady").toBool());
        QTRY_COMPARE(skeleton->parentItem()->property("revealProgress").toReal(), 1.0);
        QVERIFY(!skeleton->isVisible());
        const auto capture = qEnvironmentVariable("MOUFFETTE_VIDEO_ARTIFACT");
        if (!capture.isEmpty()) {
            // A native window can be occluded by another application's
            // always-on-top window. Grab renders without requiring scanout.
            const QImage rendered = view.grabWindow();
            QVERIFY(!rendered.isNull());
            QVERIFY(rendered.save(capture));
        }
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

        if(video->isPlaying()) video->player()->pause(); else video->player()->play();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(video->positionMs() > 100, 5000);
        if(video->isPlaying()) video->player()->pause(); else video->player()->play();
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(), 3000);
    }
};

QTEST_MAIN(VideoPlaybackBackendTest)
#include "tst_VideoPlaybackBackend.moc"
