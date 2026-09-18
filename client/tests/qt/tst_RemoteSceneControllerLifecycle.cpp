#include "backend/media/MediaResidencyManager.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
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
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantAnimation>
#include <QVideoSink>
#include <QtTest>
#include <qpa/qplatformscreen.h>
#include <qpa/qwindowsysteminterface.h>

#include "backend/files/FileManager.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"

namespace {
// Actual Qt screen-added/removed/geometry events, confined to the offscreen
// plugin so a test cannot disturb the user's native display configuration.
class TestPlatformScreen final : public QPlatformScreen {
public:
    explicit TestPlatformScreen(QString serial) : m_serial(std::move(serial)) {}
    QRect geometry() const override { return bounds; }
    int depth() const override { return 32; }
    QImage::Format format() const override { return QImage::Format_ARGB32_Premultiplied; }
    QString serialNumber() const override { return m_serial; }
    QString name() const override { return m_serial; }
    QString manufacturer() const override { return QStringLiteral("MouffetteTest"); }
    QString model() const override { return QStringLiteral("Hotplug"); }
    QRect bounds{-800, -600, 800, 600};
private:
    QString m_serial;
};

class TestDisplay final {
public:
    explicit TestDisplay(const QString& serial) : platform(new TestPlatformScreen(serial)) {
        QWindowSystemInterface::handleScreenAdded(platform);
    }
    ~TestDisplay() { remove(); }
    void remove() {
        if (platform) QWindowSystemInterface::handleScreenRemoved(platform);
        platform = nullptr;
    }
    TestPlatformScreen* platform;
};

QJsonObject textScene()
{
    SceneTimeline::ElementState state;
    state.type = QStringLiteral("text");
    state.text = QStringLiteral("teardown-test");
    state.fontFamily = QStringLiteral("Arial");
    state.fontPixelSize = 20;
    state.fitToText = false;
    state.size = state.baseSize = QSizeF(320, 180);
    QJsonObject media = state.toJson();
    media.insert("mediaId", "text-1");
    media.insert("fileId", "");
    media.insert("fileName", "");
    media.insert("timeline", SceneTimeline::MediaTrack{}.toJson());
    return {{"renderSchemaVersion", 4}, {"sceneInstanceId", "lifecycle-test-run"},
        {"timeline", SceneTimeline::SceneSettings{}.toJson()},
        {"screens", QJsonArray{QJsonObject{{"id",0},{"x",0},{"y",0},{"width",1920},{"height",1080},{"primary",true}}}},
        {"media", QJsonArray{media}}};
}

QJsonObject completeTextScene() { return textScene(); }

QJsonObject snapshotForScene(const QJsonObject&, const QJsonArray& = {})
{
    return {{"timelinePositionMs", 0}};
}

QJsonObject videoScene(const QString& fileId)
{
    auto scene = textScene();
    SceneTimeline::ElementState state;
    state.type = QStringLiteral("video");
    state.size = state.baseSize = QSizeF(1920,1080);
    state.muted = true;
    state.volume = 0.75;
    auto media = state.toJson();
    media.insert("mediaId", "video-1");
    media.insert("fileId", fileId);
    media.insert("assetId", fileId);
    media.insert("fileName", "video-1080p.mp4");
    SceneTimeline::MediaTrack track;
    track.clipsInitialized = true;
    const auto asset = MediaResidencyManager::instance().asset(UploadManager::residencyOwnerId({},0,fileId));
    const qint64 duration = asset ? (asset->durationUs + 999) / 1000 : 4000;
    media.insert("durationMs", duration);
    track.clips.append({"clip-1",0,0,std::min(SceneTimeline::SceneSettings{}.sourceSlots(duration),qint64(5400))});
    media.insert("timeline", track.toJson());
    scene.insert("sceneInstanceId", "video-lifecycle-test-run");
    scene.insert("media", QJsonArray{media});
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
    void initTestCase()
    {
        MediaResidencyManager::instance().setMemorySnapshotForTesting(
            {8ULL << 30, 6ULL << 30, 128ULL << 20, false, 0});
    }
    void cleanupTestCase()
    {
        MediaResidencyManager::instance().clearMemorySnapshotForTesting();
    }
    void incompleteSceneSchemaIsRejectedBeforeRendererMutation()
    {
        RemoteSceneController controller(nullptr, nullptr);
        QJsonObject incomplete = textScene();
        incomplete.remove(QStringLiteral("renderSchemaVersion"));

        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStart",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("schema-test-owner")),
            Q_ARG(QJsonObject, incomplete)));
        QVERIFY(!findRemoteWindow());
    }

    void screenHotplugPreservesRunAndMatchesIdentity()
    {
        if (QGuiApplication::platformName() != QLatin1String("offscreen"))
            QSKIP("Synthetic QPA screens require the offscreen platform");
        TestDisplay display(QStringLiteral("display-a"));
        RemoteSceneController controller(nullptr, nullptr);
        auto scene = textScene();
        auto screen = scene["screens"].toArray().first().toObject();
        screen["id"] = 1;
        screen["primary"] = false;
        scene["screens"] = QJsonArray{scene["screens"].toArray().first(), screen};
        auto media = scene["media"].toArray().first().toObject();
        media["x"] = 480.0;
        media["width"] = 960.0;
        media["baseWidth"] = 960.0;
        scene["media"] = QJsonArray{media};
        controller.onRemoteSceneStart(QStringLiteral("hotplug-owner"), scene);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 5000);
        QCOMPARE(controller.m_screenWindows.size(), 2);
        auto& primary = controller.m_screenWindows[0];
        auto& secondary = controller.m_screenWindows[1];
        QCOMPARE(secondary.targetScreen, display.platform->screen());
        QVERIFY(!secondary.window->isVisible());
        controller.activateScene();
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_firstFramePresentedLocalSteadyMs >= 0, 5000);
        const auto item = controller.m_mediaItems.first();
        const auto initialPosition = controller.m_timelinePositionMs;
        const auto epoch = controller.m_sceneEpoch;
        const auto firstFrame = controller.m_firstFramePresentedLocalSteadyMs;
        const auto sourceTopology = secondary.sourceScreenDefinition;
        QPointer<QQuickWindow> oldWindow = secondary.window;

        display.platform->bounds = QRect(-1200, -900, 1200, 900);
        QWindowSystemInterface::handleScreenGeometryChange(display.platform->screen(),
            display.platform->bounds, display.platform->bounds);
        QTRY_COMPARE(secondary.window->geometry(), display.platform->screen()->geometry());
        QTRY_COMPARE(secondary.w, secondary.window->width());
        const auto rendered = [&] {
            return secondary.mediaModel->data(secondary.mediaModel->index(0, 0),
                                              MediaListModel::ModelDataRole).toMap();
        };
        QTRY_COMPARE(rendered()["destWidth"].toDouble(), secondary.w * 0.5);
        QCOMPARE(rendered()["destX"].toDouble(), secondary.w * 0.25);
        QCOMPARE(secondary.sourceScreenDefinition, sourceTopology);
        QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(
            display.platform->screen(), 144, 144);
        QTRY_COMPARE(secondary.window->geometry(), display.platform->screen()->geometry());
        QTRY_COMPARE(rendered()["destWidth"].toDouble(), secondary.window->width() * 0.5);

        display.remove();
        QVERIFY(!secondary.window->isVisible()); // Before the deferred refresh.
        QVERIFY(!secondary.targetScreen);
        QVERIFY(primary.window->isVisible());
        QVERIFY(controller.m_sceneActivated);
        QVERIFY(controller.m_timelineTimer.isActive());
        QTest::qWait(80);
        QVERIFY(controller.m_timelinePositionMs > initialPosition);
        QCOMPARE(controller.m_sceneEpoch, epoch);
        QVERIFY(controller.applyAuthoritativeStateSnapshot(snapshotForScene(scene), 1, 0));
        QCOMPARE(controller.m_screenWindows.size(), 2);
        QCOMPARE(secondary.sourceScreenDefinition, sourceTopology);

        TestDisplay unrelated(QStringLiteral("display-b"));
        QTest::qWait(30);
        QVERIFY(!secondary.window->isVisible());
        TestDisplay ambiguous1(QStringLiteral("display-a"));
        TestDisplay ambiguous2(QStringLiteral("display-a"));
        QTest::qWait(30);
        QVERIFY(!secondary.window->isVisible());
        QVERIFY(!secondary.targetScreen);
        ambiguous2.remove();
        QTRY_COMPARE(secondary.targetScreen, ambiguous1.platform->screen());
        QTRY_VERIFY(secondary.window->isVisible());
        QCOMPARE(secondary.window, oldWindow);
        QCOMPARE(controller.m_sceneEpoch, epoch);
        QCOMPARE(controller.m_firstFramePresentedLocalSteadyMs, firstFrame);
        QVERIFY(controller.m_timelinePositionMs > initialPosition);

        controller.onRemoteSceneStop(QStringLiteral("hotplug-owner"), scene["sceneInstanceId"].toString());
        QTRY_VERIFY(oldWindow.isNull());
        ambiguous1.remove();
        TestDisplay afterStop(QStringLiteral("display-a"));
        QTest::qWait(50);
        QVERIFY(controller.m_screenWindows.isEmpty());
        QVERIFY(!findRemoteWindow());
    }

    void absentDisplayCannotCompletePreparationOrActivation()
    {
        RemoteSceneController controller(nullptr, nullptr);
        const auto scene = textScene();
        controller.onRemoteSceneStart(QStringLiteral("missing-screen-owner"), scene);
        QVERIFY(!controller.m_screenWindows.isEmpty());
        controller.refreshScreenBindings({});
        QVERIFY(!controller.remoteRenderGraphsReady());
        // Text may prime synchronously inside onRemoteSceneStart. Re-evaluate
        // readiness with the target absent, as a later media-ready callback does.
        controller.m_sceneActivationRequested = false;
        controller.startSceneActivationIfReady();
        QTest::qWait(60);
        QVERIFY(!controller.m_sceneActivationRequested);
        QVERIFY(!controller.m_screenWindows[0].window->isVisible());
        // Even a COMMIT crossing an unplug must not claim successful output.
        controller.m_sceneActivationRequested = true;
        controller.activateScene();
        QVERIFY(!controller.m_sceneActivated);
        QTRY_VERIFY_WITH_TIMEOUT(!findRemoteWindow(), 2000);
    }

    void displayLostBeforeFirstFrameDoesNotSatisfyBarrier()
    {
        RemoteSceneController controller(nullptr, nullptr);
        const auto scene = textScene();
        controller.onRemoteSceneStart(QStringLiteral("first-frame-hotplug"), scene);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 5000);
        auto& output = controller.m_screenWindows[0];
        output.screenIdentity = QStringLiteral("test-first-frame-screen");
        LocalScreenTopology::Screen target{output.targetScreen, output.screenIdentity,
            output.window->geometry(), {}, true, false};
        controller.activateScene();
        controller.refreshScreenBindings({}); // Before queued compositor events.
        QTest::qWait(80);
        QVERIFY(!output.window->isVisible());
        QVERIFY(controller.m_screensAwaitingFirstFrame.contains(0));
        QCOMPARE(controller.m_firstFramePresentedLocalSteadyMs, qint64(-1));
        controller.refreshScreenBindings({target});
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_screensAwaitingFirstFrame.isEmpty(), 5000);
        QVERIFY(controller.m_firstFramePresentedLocalSteadyMs >= 0);
        controller.onRemoteSceneStop(QStringLiteral("first-frame-hotplug"), scene["sceneInstanceId"].toString());
        QTRY_VERIFY_WITH_TIMEOUT(!findRemoteWindow(), 2000);
    }

    void reportedDelayedVideoPreparesAtStartAndAudioTail()
    {
        const QString fileId = QStringLiteral("reported-delayed-video");
        const QString owner = UploadManager::residencyOwnerId({}, 0, fileId);
        const QString path = QString::fromUtf8(TEST_REPORTED_VIDEO_FILE);
        FileManager files;
        files.registerReceivedFilePath(fileId, path);
        auto& residency = MediaResidencyManager::instance();
        residency.acquire(owner, path);
        const auto cleanup = qScopeGuard([&] {
            files.removeReceivedFileMapping(fileId);
            residency.release(owner);
        });
        QTRY_VERIFY2_WITH_TIMEOUT(residency.ready(owner), qPrintable(residency.errorString(owner)), 10000);
        const qint64 tail = (residency.asset(owner)->durationUs + 999) / 1000 - 1;
        RemoteSceneController controller(&files, nullptr);
        int attempt = 0;
        const SceneTimeline::SceneSettings grid;
        const auto lastSlot=grid.slotAt(tail);
        for (qint64 targetSlot : {qint64(0), lastSlot, qint64(1), qint64(0)}) {
            const qint64 target=qRound64(grid.timeMs(targetSlot));
            auto scene = videoScene(fileId);
            scene["sceneInstanceId"] = QStringLiteral("delayed-video-run-%1").arg(++attempt);
            auto media = scene["media"].toArray().first().toObject();
            SceneTimeline::MediaTrack track;
            track.clipsInitialized = true;
            track.clips = {{"selected-range",attempt == 4 ? 0 : 150,targetSlot,grid.sourceSlots(tail+1)-targetSlot}};
            media["timeline"] = track.toJson();
            scene["media"] = QJsonArray{media};
            controller.onRemoteSceneStart(QStringLiteral("delayed-video-owner"), scene);
            QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 5000);
            const auto item = controller.m_mediaItems.first();
            QVERIFY(item->primedFirstFrame);
            QVERIFY(!item->lastFrameImage.isNull());
            QCOMPARE(item->player->position(), target);
            QCOMPARE(item->timelineRequestedSourceMs, target);
            controller.activateScene();
            QTRY_VERIFY_WITH_TIMEOUT(controller.m_firstFramePresentedLocalSteadyMs >= 0, 5000);
            if (attempt == 4) {
                QTRY_VERIFY_WITH_TIMEOUT(item->player->isPlaying() && item->player->position() > 100, 2000);
                QVERIFY(item->renderVisible);
            } else {
                QCOMPARE(item->player->position(), target);
                QVERIFY(!item->player->isPlaying());
            }
            if (attempt == 2) {
                QPointer<ResidentVideoPlayer> player(item->player);
                emit player->errorOccurred(QMediaPlayer::FormatError, "simulated decoder failure");
                QVERIFY(player);
                QTRY_VERIFY_WITH_TIMEOUT(player.isNull(), 3000);
            } else {
                controller.onRemoteSceneStop(QStringLiteral("delayed-video-owner"), scene["sceneInstanceId"].toString());
            }
            QTRY_VERIFY_WITH_TIMEOUT(!controller.m_teardownInProgress, 3000);
            QVERIFY(!findRemoteWindow());
        }
    }

    void allOutputsAbsentKeepVideoAndAudioTimeline()
    {
        const QString fileId = QStringLiteral("hotplug-video");
        const QString owner = UploadManager::residencyOwnerId({}, 0, fileId);
        FileManager files;
        files.registerReceivedFilePath(fileId, QString::fromUtf8(TEST_VIDEO_FILE));
        auto& residency = MediaResidencyManager::instance();
        residency.acquire(owner, QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(residency.ready(owner), 60000);
        RemoteSceneController controller(&files, nullptr);
        auto scene = videoScene(fileId);
        auto media = scene["media"].toArray().first().toObject();
        scene["media"] = QJsonArray{media};
        controller.onRemoteSceneStart(QStringLiteral("video-hotplug-owner"), scene);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 5000);
        controller.activateScene();
        const auto item = controller.m_mediaItems.first();
        QTRY_COMPARE(item->player->playbackState(), QMediaPlayer::PlayingState);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_firstFramePresentedLocalSteadyMs >= 0, 5000);
        auto& output = controller.m_screenWindows[0];
        output.screenIdentity = QStringLiteral("test-video-screen");
        LocalScreenTopology::Screen target{output.targetScreen, output.screenIdentity,
            output.window->geometry(), {}, true, false};
        const auto player = item->player;
        const auto audio = item->audio;
        const auto frameSource = item->frameSource;
        const auto epoch = controller.m_sceneEpoch;
        controller.refreshScreenBindings({});
        QVERIFY(!output.window->isVisible());
        const qint64 position = player->position();
        QTRY_VERIFY_WITH_TIMEOUT(player->position() != position, 2000);
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QCOMPARE(item->audio, audio);
        QCOMPARE(item->frameSource, frameSource);
        QVERIFY(audio->isMuted());
        QVERIFY(controller.m_sceneActivated);
        QCOMPARE(controller.m_sceneEpoch, epoch);
        controller.refreshScreenBindings({target});
        QVERIFY(output.window->isVisible());
        QCOMPARE(item->player, player);
        QCOMPARE(item->audio, audio);
        QCOMPARE(item->frameSource, frameSource);
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QCOMPARE(controller.m_sceneEpoch, epoch);
        controller.onRemoteSceneStop(QStringLiteral("video-hotplug-owner"), scene["sceneInstanceId"].toString());
        QTRY_VERIFY_WITH_TIMEOUT(!findRemoteWindow(), 2000);
        files.removeReceivedFileMapping(fileId);
        residency.release(owner);
    }

    void timelineInterpolatesBySlotsAndDiscreteValuesChangeAtKeys()
    {
        RemoteSceneController controller(nullptr, nullptr);
        auto scene = textScene();
        auto media = scene["media"].toArray().first().toObject();
        SceneTimeline::ElementState first;
        QVERIFY(SceneTimeline::ElementState::fromMediaJson(media, &first));
        first.position = QPointF(0,0);
        first.opacity = 0;
        auto second = first;
        second.position = QPointF(200,0);
        second.opacity = 1;
        second.uppercase = true;
        SceneTimeline::MediaTrack track;
        track.keyframes = {{"start",0,first},{"end",30,second}};
        media["timeline"] = track.toJson();
        scene["media"] = QJsonArray{media};
        controller.onRemoteSceneStart("timeline-owner", scene);
        QTRY_VERIFY(controller.m_sceneActivationRequested);
        const auto item = controller.m_mediaItems.first();
        controller.evaluateTimelineAt(333,false);
        QVERIFY(qAbs(item->renderOpacity - 0.3) < 0.0001);
        QVERIFY(!item->fontUppercase);
        QVERIFY(qAbs(item->spans.first().nx * 1920 - 60.0) < 0.0001);
        controller.evaluateTimelineAt(1000,false);
        QVERIFY(item->fontUppercase);
        QCOMPARE(item->renderOpacity, 1.0);
        controller.evaluateTimelineAt(200,false);
        QVERIFY(!item->fontUppercase);
        QCOMPARE(item->renderOpacity, 0.2);
        // Both renderers use the exact same grid even on reverse/fractional seeks.
        const SceneTimeline::SceneSettings grid;
        for (qreal time : {999.9, 333.3333333333333, 16.5, 1000.0, 0.0}) {
            controller.evaluateTimelineAt(time,false);
            const auto local = SceneTimeline::evaluateMedia(media,time,grid);
            QCOMPARE(item->renderOpacity,local["contentOpacity"].toDouble());
            QCOMPARE(item->fontUppercase,local["fontUppercase"].toBool());
        }
    }

    void initiallyOffscreenMediaEntersAndLeavesOutput()
    {
        RemoteSceneController controller(nullptr, nullptr);
        auto scene = textScene();
        auto media = scene["media"].toArray().first().toObject();
        SceneTimeline::ElementState first;
        QVERIFY(SceneTimeline::ElementState::fromMediaJson(media, &first));
        first.position = QPointF(3000,0);
        auto second = first;
        second.position = QPointF(100,0);
        SceneTimeline::MediaTrack track;
        track.keyframes = {{"out",0,first},{"in",30,second}};
        media["timeline"] = track.toJson();
        scene["media"] = QJsonArray{media};
        controller.onRemoteSceneStart("offscreen-owner", scene);
        QTRY_VERIFY(controller.m_sceneActivationRequested);
        const auto item = controller.m_mediaItems.first();
        QVERIFY(item->spans.isEmpty());
        const auto model = controller.m_screenWindows[0].mediaModel;
        QCOMPARE(model->rowCount(),0);
        controller.evaluateTimelineAt(1000,false);
        QCOMPARE(item->spans.size(),1);
        QCOMPARE(model->rowCount(),1);
        auto rendered = model->data(model->index(0,0),MediaListModel::ModelDataRole).toMap();
        QVERIFY(rendered["renderVisible"].toBool());
        controller.evaluateTimelineAt(0,false);
        rendered = model->data(model->index(0,0),MediaListModel::ModelDataRole).toMap();
        QVERIFY(!rendered["renderVisible"].toBool());
        QCOMPARE(rendered["destWidth"].toDouble(),0.0);
    }

    void videoClipsDriveSourceCursorAndGateAudio()
    {
        const QString fileId(64, QLatin1Char('e'));
        const QString owner = UploadManager::residencyOwnerId({},0,fileId);
        FileManager files;
        const QString path = QString::fromUtf8(TEST_VIDEO_FILE);
        files.registerReceivedFilePath(fileId,path);
        auto& residency = MediaResidencyManager::instance();
        residency.acquire(owner,path);
        const auto cleanup = qScopeGuard([&]{ files.removeReceivedFileMapping(fileId); residency.release(owner); });
        QTRY_VERIFY_WITH_TIMEOUT(residency.ready(owner),60000);
        RemoteSceneController controller(&files,nullptr);
        auto scene = videoScene(fileId);
        auto media = scene["media"].toArray().first().toObject();
        media["muted"] = false;
        SceneTimeline::MediaTrack track;
        track.clipsInitialized = true;
        track.clips = {{"a",9,30,12},{"b",30,60,12}};
        media["timeline"] = track.toJson();
        scene["media"] = QJsonArray{media};
        controller.onRemoteSceneStart("clip-owner",scene);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested,5000);
        const auto item = controller.m_mediaItems.first();
        QCOMPARE(item->player->position(),1000);
        QVERIFY(item->audio->isMuted());
        controller.evaluateTimelineAt(400,true);
        QCOMPARE(item->timelineRequestedSourceMs,1100);
        QVERIFY(item->player->isPlaying());
        QVERIFY(!item->audio->isMuted());
        controller.evaluateTimelineAt(700,true);
        QCOMPARE(item->timelineRequestedSourceMs,1399);
        QVERIFY(!item->player->isPlaying());
        QVERIFY(item->audio->isMuted());
        QTRY_VERIFY_WITH_TIMEOUT(item->player->preparedAt(1399), 5000);
        // Native backends may report a rounded position within the held
        // frame. Its coverage is sufficient; do not flush it on every tick.
        item->player->setPosition(1390);
        item->timelineSeekGuardUntilMs = 0;
        QSignalSpy heldCursorChanges(item->player, &ResidentVideoPlayer::positionChanged);
        controller.evaluateTimelineAt(700,true);
        QCOMPARE(heldCursorChanges.count(),0);
        controller.evaluateTimelineAt(1050,false);
        QCOMPARE(item->timelineRequestedSourceMs,2050);
        QVERIFY(item->audio->isMuted());
        controller.evaluateTimelineAt(1050,true);
        QVERIFY(!item->audio->isMuted());
        controller.evaluateTimelineAt(1500,true);
        QCOMPARE(item->timelineRequestedSourceMs,2399);
        QVERIFY(item->audio->isMuted());
        QVERIFY(!item->player->isPlaying());
        // A pure editing split is continuous in source time: crossing it must
        // not flush the decoder and introduce an audio gap.
        item->timeline.clips = {{"a",9,30,6},{"a-right",15,36,6}};
        controller.evaluateTimelineAt(499,false);
        controller.evaluateTimelineAt(499,true);
        QSignalSpy cursorChanges(item->player, &ResidentVideoPlayer::positionChanged);
        controller.evaluateTimelineAt(501,true);
        QCOMPARE(cursorChanges.count(),0);
        QCOMPARE(item->timelineRequestedSourceMs,1201);
        // The real source's fractional final slot is occupied but silent.
        const SceneTimeline::SceneSettings grid;
        const auto duration=item->player->duration();
        const auto occupied=grid.sourceSlots(duration);
        QVERIFY(grid.timeMs(occupied)>duration);
        item->timeline.clips={{"padded",0,0,occupied}};
        controller.evaluateTimelineAt(duration+1,true);
        QCOMPARE(item->timelineRequestedSourceMs,duration-1);
        QVERIFY(!item->player->isPlaying()); QVERIFY(item->audio->isMuted());
        QTRY_VERIFY_WITH_TIMEOUT(item->player->preparedAt(duration-1),5000);
        QVERIFY(item->player->videoSink()->videoFrame().isValid());
        item->timeline.clips.clear();
        controller.evaluateTimelineAt(300,false);
        QCOMPARE(item->timelineRequestedSourceMs,0);
        QVERIFY(item->audio->isMuted());
    }

    void zeroStopFinishesWithoutStartingVideoOrAwaitingPresentation()
    {
        const QString fileId(64, QLatin1Char('f'));
        const QString owner = UploadManager::residencyOwnerId({}, 0, fileId);
        FileManager files;
        const QString path = QString::fromUtf8(TEST_VIDEO_FILE);
        files.registerReceivedFilePath(fileId, path);
        auto& residency = MediaResidencyManager::instance();
        residency.acquire(owner, path);
        const auto cleanup = qScopeGuard([&] {
            files.removeReceivedFileMapping(fileId);
            residency.release(owner);
        });
        QTRY_VERIFY_WITH_TIMEOUT(residency.ready(owner), 60000);
        RemoteSceneController controller(&files, nullptr);
        auto scene = videoScene(fileId);
        scene["timeline"] = QJsonObject{{"maxDurationMs", 180000}, {"stopSlot", 0}, {"slotsPerSecond",30}};
        auto media = scene["media"].toArray().first().toObject();
        media["muted"] = false;
        scene["media"] = QJsonArray{media};
        controller.onRemoteSceneStart("zero-stop-owner", scene);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 5000);
        const auto item = controller.m_mediaItems.first();
        controller.activateScene();
        QCOMPARE(controller.m_timelinePositionMs, 0);
        QVERIFY(controller.m_timelineFinished);
        QVERIFY(!controller.m_timelineTimer.isActive());
        QVERIFY(!item->player->isPlaying());
        QVERIFY(item->audio->isMuted());
        QVERIFY(controller.m_screensAwaitingFirstFrame.isEmpty());
        QVERIFY(controller.m_firstFramePresentedLocalSteadyMs < 0);
        const auto& output = controller.m_screenWindows[0];
        QVERIFY(!output.window->isVisible());
        QVERIFY(!output.firstFrameConnection);
        QVERIFY(!controller.m_sceneReadyTimeout || !controller.m_sceneReadyTimeout->isActive());
        controller.onRemoteSceneStop("zero-stop-owner", scene["sceneInstanceId"].toString());
        QTRY_VERIFY_WITH_TIMEOUT(!findRemoteWindow(), 2000);
    }

    void stopDeadlineHidesOutputAndCannotReviveOnDisplayReturn()
    {
        RemoteSceneController controller(nullptr,nullptr);
        auto scene = textScene();
        scene["timeline"] = QJsonObject{{"maxDurationMs",180000},{"stopSlot",3},{"slotsPerSecond",30}};
        controller.onRemoteSceneStart("stop-owner",scene);
        QTRY_VERIFY(controller.m_sceneActivationRequested);
        controller.activateScene();
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_timelineFinished,1000);
        QCOMPARE(controller.m_timelinePositionMs,100);
        QVERIFY(!controller.m_timelineTimer.isActive());
        auto& output = controller.m_screenWindows[0];
        QVERIFY(!output.window->isVisible());
        LocalScreenTopology::Screen target{output.targetScreen, output.screenIdentity,
            output.window->geometry(),{},true,false};
        controller.refreshScreenBindings({});
        controller.refreshScreenBindings({target});
        QVERIFY(!output.window->isVisible());
    }

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
        const QString residentOwner = UploadManager::residencyOwnerId({}, 0, fileId);
        auto& residency = MediaResidencyManager::instance();
        residency.acquire(residentOwner, fixture);
        QTRY_VERIFY_WITH_TIMEOUT(residency.ready(residentOwner), 60000);
        RemoteSceneController controller(&files, nullptr);
        QSignalSpy teardownSpy(&controller, &RemoteSceneController::teardownSettled);
        const QJsonObject scene = videoScene(fileId);
        QVERIFY(QMetaObject::invokeMethod(
            &controller,
            "onRemoteSceneStart",
            Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("sender-video")),
            Q_ARG(QJsonObject, scene)));

        QPointer<ResidentVideoPlayer> player = controller.findChild<ResidentVideoPlayer*>();
        if (!player) {
            files.removeReceivedFileMapping(fileId);
        residency.release(residentOwner);
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
        residency.release(residentOwner);
    }

    void liveSceneSurvivesTransportGraceAndStopsOnlyForItsOwnDeadline()
    {
        QTemporaryDir identity;
        QVERIFY(identity.isValid());
        WebSocketClient socket(identity.path(), false);
        RemoteSceneController controller(nullptr, &socket);
        // Build the renderer fixture with the same local test policy used by
        // the other lifecycle tests; retain the real transport signal wiring.
        controller.m_ws = nullptr;
        controller.onRemoteSceneStart(QStringLiteral("grace-owner"), textScene());
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 2000);
        controller.activateScene();
        QVERIFY(controller.m_sceneActivated);
        controller.m_ws = &socket;
        controller.m_pendingRemoteSessionId = QStringLiteral("grace-session");
        QPointer<QQuickWindow> window = findRemoteWindow();
        QVERIFY(window);
        QSignalSpy settled(&controller, &RemoteSceneController::teardownSettled);

        emit socket.transportHealthChanged(true);
        emit socket.disconnected();
        emit socket.remoteSessionRecoveryExpired(QStringLiteral("other-session"), 1);
        QCoreApplication::processEvents();
        QVERIFY(window);
        QVERIFY(!controller.m_screenWindows.isEmpty());
        QCOMPARE(settled.count(), 0);

        emit socket.remoteSessionRecoveryExpired(QStringLiteral("grace-session"), 1);
        QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 2000);
        QVERIFY(controller.m_screenWindows.isEmpty());
    }

    void queuedActivationCannotStartWithoutAnUnexpiredSessionProof()
    {
        QTemporaryDir identity;
        WebSocketClient socket(identity.path(), false);
        RemoteSceneController controller(nullptr, &socket);
        controller.m_ws = nullptr;
        controller.onRemoteSceneStart(QStringLiteral("expired-owner"), textScene());
        QPointer<QQuickWindow> window = findRemoteWindow();
        QVERIFY(window);
        controller.m_ws = &socket;
        controller.m_pendingRemoteSessionId = QStringLiteral("expired-session");
        QCOMPARE(socket.sessionRecoveryRemainingMs(controller.m_pendingRemoteSessionId), qint64(0));
        // Simulate the scheduled callback running before the watchdog after
        // wake: no command authority remains, so no first frame can be started.
        controller.activateScene();
        QVERIFY(!controller.m_sceneActivated);
        QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 2000);
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
		QSignalSpy teardownSpy(&controller, &RemoteSceneController::teardownSettled);
		QVERIFY(controller.teardownRemoteSession(
			QStringLiteral("unrelated-remote-session")));
		QCoreApplication::processEvents();
		QVERIFY(remoteWindow);
		QCOMPARE(teardownSpy.count(), 1);
		QCOMPARE(teardownSpy.first().at(0).toString(),
		         QStringLiteral("unrelated-remote-session"));

		QVERIFY(QMetaObject::invokeMethod(
			&controller, "onRemoteSceneStop", Qt::DirectConnection,
			Q_ARG(QString, QStringLiteral("owned-renderer")),
			Q_ARG(QString, QStringLiteral("lifecycle-test-run"))));
		QTRY_VERIFY_WITH_TIMEOUT(remoteWindow.isNull(), 2000);
	}

    void timelineSnapshotsRejectMalformedAndCannotReplaceScene()
    {
        RemoteSceneController controller(nullptr,nullptr);
        auto scene = textScene();
        controller.onRemoteSceneStart("snapshot-owner",scene);
        QTRY_VERIFY(controller.m_sceneActivationRequested);
        const auto item = controller.m_mediaItems.first();
        QVERIFY(!controller.applyAuthoritativeStateSnapshot({{"timelinePositionMs",-1}},2,0));
        QVERIFY(!controller.applyAuthoritativeStateSnapshot({{"timelinePositionMs",200000}},2,0));
        QVERIFY(!controller.applyAuthoritativeStateSnapshot({{"timelinePositionMs",100},{"scene",scene}},2,0));
        QCOMPARE(controller.m_lastVideoSyncSequence,0);
        QVERIFY(controller.applyAuthoritativeStateSnapshot({{"timelinePositionMs",100.125}},2,10));
        QCOMPARE(controller.m_timelineAnchorMs,110.125);
        QCOMPARE(controller.m_lastVideoSyncSequence,2);
        QVERIFY(!controller.applyAuthoritativeStateSnapshot({{"timelinePositionMs",200}},2,0));
        QCOMPARE(item->text,QStringLiteral("teardown-test"));
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
