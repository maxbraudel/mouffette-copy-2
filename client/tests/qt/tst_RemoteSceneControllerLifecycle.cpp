#include "backend/media/MediaResidencyManager.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/network/UploadManager.h"
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
    scene[QStringLiteral("sceneInstanceId")] = QStringLiteral("lifecycle-test-run");
    scene[QStringLiteral("screens")] = QJsonArray{screen};
    scene[QStringLiteral("media")] = QJsonArray{media};
    return scene;
}

QJsonObject completeTextScene()
{
    return textScene();
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
    media[QStringLiteral("assetId")] = fileId;
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
    media[QStringLiteral("startPositionMs")] = 0;
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

    void visualAutomation_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<int>("hideMode");
        for (const QString& type : {QStringLiteral("text"), QStringLiteral("image"), QStringLiteral("video")}) {
            QTest::newRow(qPrintable(type + "-fade-in")) << type << 0;
            QTest::newRow(qPrintable(type + "-immediate-hide-during-fade")) << type << 1;
            QTest::newRow(qPrintable(type + "-fade-out-during-fade")) << type << 2;
        }
    }

    void visualAutomation()
    {
        QFETCH(QString, type);
        QFETCH(int, hideMode);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString fileId(64, QLatin1Char('d'));
        FileManager files;
        auto& residency = MediaResidencyManager::instance();
        QString owner;
        QString fixture;
        if (type != QLatin1String("text")) {
            fixture = QString::fromUtf8(TEST_VIDEO_FILE);
            if (type == QLatin1String("image")) {
                fixture = temporary.filePath(QStringLiteral("fade.png"));
                QImage image(32, 32, QImage::Format_ARGB32);
                image.fill(Qt::red);
                QVERIFY(image.save(fixture));
            }
            files.registerReceivedFilePath(fileId, fixture);
            owner = UploadManager::residencyOwnerId({}, 0, fileId);
            residency.acquire(owner, fixture);
            QTRY_VERIFY_WITH_TIMEOUT(residency.ready(owner), 60000);
        }
        RemoteSceneController controller(&files, nullptr);
        auto scene = type == QLatin1String("video") ? videoScene(fileId) : textScene();
        auto entry = scene.value("media").toArray().at(0).toObject();
        entry["type"] = type;
        if (type == QLatin1String("image")) {
            entry["assetId"] = fileId;
            entry["fileId"] = fileId;
            entry["fileName"] = QStringLiteral("fade.png");
        }
        entry["autoDisplay"] = true;
        entry["visible"] = false;
        entry["autoDisplayDelayMs"] = 180;
        entry["autoPlay"] = false;
        entry["fadeInSeconds"] = 0.4;
        entry["contentOpacity"] = 0.6;
        entry["autoHide"] = hideMode != 0;
        entry["autoHideDelayMs"] = 150;
        entry["fadeOutSeconds"] = hideMode == 2 ? 0.12 : 0.0;
        scene["media"] = QJsonArray{entry};
        controller.onRemoteSceneStart(QStringLiteral("automation-owner"), scene);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 5000);
        QCOMPARE(controller.m_mediaItems.size(), 1);
        const auto item = controller.m_mediaItems.first();
        // PREPARE must not consume display delays or animation time.
        QTest::qWait(220);
        QCOMPARE(item->renderOpacity, 0.0);
        QVERIFY(!item->displayStarted);
        controller.activateScene();
        QVERIFY(controller.m_sceneActivated);
        int fadeInSnapshots = 0;
        int fadeOutSnapshots = 0;
        bool allSnapshotsApplied = true;
        bool envelopesPreserved = true;
        quint64 sequence = 0;
        QTimer snapshots;
        snapshots.setInterval(25);
        connect(&snapshots, &QTimer::timeout, &controller, [&]() {
            auto currentScene = scene;
            auto currentEntry = entry;
            currentEntry["visible"] = item->contentVisible;
            currentScene["media"] = QJsonArray{currentEntry};
            QJsonArray videos;
            if (item->player) {
                videos.append(QJsonObject{
                    {"mediaId", item->mediaId}, {"positionMs", double(item->player->position())},
                    {"durationMs", double(item->player->duration())}, {"playing", false},
                    {"muted", item->muted}, {"visible", item->contentVisible},
                    {"repeatAvailable", false}
                });
            }
            const auto fade = item->visualFadeAnimation;
            const qreal opacity = item->renderOpacity;
            const bool hiding = item->hiding;
            const bool displayStarted = item->displayStarted;
            allSnapshotsApplied &= controller.applyAuthoritativeStateSnapshot(
                snapshotForScene(currentScene, videos), ++sequence, 0);
            envelopesPreserved &= item->visualFadeAnimation == fade
                && item->renderOpacity == opacity && item->hiding == hiding
                && item->displayStarted == displayStarted;
            if (fade) {
                if (hiding) ++fadeOutSnapshots;
                else ++fadeInSnapshots;
            }
        });
        snapshots.start();
        QTest::qWait(80);
        QCOMPARE(item->renderOpacity, 0.0);
        QTRY_VERIFY_WITH_TIMEOUT(item->renderOpacity > 0.0 && item->renderOpacity < 0.6, 1000);
        auto* model = findRemoteWindow()->findChild<MediaListModel*>();
        QVERIFY(model);
        const QVariantMap rendered = model->data(model->index(0, 0), MediaListModel::ModelDataRole).toMap();
        QVERIFY(rendered.value(QStringLiteral("contentVisible")).toBool());
        QVERIFY(rendered.value(QStringLiteral("renderVisible")).toBool());
        if (hideMode == 0) {
            QTRY_COMPARE_WITH_TIMEOUT(item->renderOpacity, 0.6, 1000);
            QVERIFY(item->renderVisible);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(!item->renderVisible, 1000);
            QCOMPARE(item->renderOpacity, 0.0);
            // The cancelled fade-in must never revive the already hidden item.
            QTest::qWait(450);
            QVERIFY(!item->renderVisible);
            QCOMPARE(item->renderOpacity, 0.0);
        }
        snapshots.stop();
        QVERIFY(allSnapshotsApplied);
        QVERIFY(envelopesPreserved);
        QVERIFY(fadeInSnapshots > 0);
        if (hideMode == 2) QVERIFY(fadeOutSnapshots > 0);
        controller.onRemoteSceneStop(QStringLiteral("automation-owner"),
                                     scene.value("sceneInstanceId").toString());
        QTRY_VERIFY_WITH_TIMEOUT(!findRemoteWindow(), 2000);
        if (!owner.isEmpty()) {
            files.removeReceivedFileMapping(fileId);
            residency.release(owner);
        }
    }

    void authoritativeVisibilityChangesUseFades()
    {
        RemoteSceneController controller(nullptr, nullptr);
        auto scene = textScene();
        auto entry = scene.value("media").toArray().first().toObject();
        entry["visible"] = false;
        entry["fadeInSeconds"] = 0.4;
        scene["media"] = QJsonArray{entry};
        controller.onRemoteSceneStart(QStringLiteral("visibility-owner"), scene);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 5000);
        controller.activateScene();
        QVERIFY(controller.m_sceneActivated);
        const auto item = controller.m_mediaItems.first();
        entry["visible"] = true;
        scene["media"] = QJsonArray{entry};
        QVERIFY(controller.applyAuthoritativeStateSnapshot(snapshotForScene(scene), 1, 0));
        QVERIFY(item->visualFadeAnimation);
        QVERIFY(item->contentVisible);
        QCOMPARE(item->renderOpacity, 0.0);
        QTRY_VERIFY_WITH_TIMEOUT(item->renderOpacity > 0.0 && item->renderOpacity < 1.0, 1000);
        const auto fade = item->visualFadeAnimation;
        const qreal opacity = item->renderOpacity;
        QVERIFY(controller.applyAuthoritativeStateSnapshot(snapshotForScene(scene), 2, 0));
        QCOMPARE(item->visualFadeAnimation, fade);
        QCOMPARE(item->renderOpacity, opacity);
        entry["visible"] = false;
        scene["media"] = QJsonArray{entry};
        QVERIFY(controller.applyAuthoritativeStateSnapshot(snapshotForScene(scene), 3, 0));
        QVERIFY(!item->visualFadeAnimation);
        QVERIFY(!item->renderVisible);
        QTest::qWait(450);
        QVERIFY(!item->renderVisible);
        QCOMPARE(item->renderOpacity, 0.0);
        controller.onRemoteSceneStop(QStringLiteral("visibility-owner"),
                                     scene.value("sceneInstanceId").toString());
        QTRY_VERIFY_WITH_TIMEOUT(!findRemoteWindow(), 2000);
    }

    void playbackAutomation_data()
    {
        QTest::addColumn<int>("endDelayMs");
        QTest::addColumn<bool>("pauseEarly");
        QTest::addColumn<bool>("repeat");
        QTest::newRow("hide-and-mute-before-end") << -250 << false << false;
        QTest::newRow("hide-and-mute-at-end") << 0 << false << false;
        QTest::newRow("hide-and-mute-after-end") << 180 << false << false;
        QTest::newRow("hide-and-mute-only-after-final-repeat") << 0 << false << true;
        QTest::newRow("pause-delay-after-play-delay") << 0 << true << false;
    }

    void playbackAutomation()
    {
        QFETCH(int, endDelayMs);
        QFETCH(bool, pauseEarly);
        QFETCH(bool, repeat);
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        const QString fileId(64, QLatin1Char('e'));
        FileManager files;
        files.registerReceivedFilePath(fileId, fixture);
        const QString owner = UploadManager::residencyOwnerId({}, 0, fileId);
        auto& residency = MediaResidencyManager::instance();
        residency.acquire(owner, fixture);
        QTRY_VERIFY_WITH_TIMEOUT(residency.ready(owner), 60000);
        RemoteSceneController controller(&files, nullptr);
        auto scene = videoScene(fileId);
        auto entry = scene.value("media").toArray().at(0).toObject();
        entry["startPositionMs"] = 1000;
        entry["endPositionMs"] = 1900;
        entry["autoPlayDelayMs"] = 180;
        entry["autoPause"] = pauseEarly;
        entry["autoPauseDelayMs"] = 260;
        entry["autoUnmute"] = true;
        entry["autoUnmuteDelayMs"] = 100;
        entry["audioFadeInSeconds"] = 0.1;
        entry["hideWhenVideoEnds"] = true;
        entry["muteWhenVideoEnds"] = true;
        entry["autoHideDelayMs"] = endDelayMs;
        entry["autoMuteDelayMs"] = endDelayMs;
        entry["repeatEnabled"] = repeat;
        entry["repeatCount"] = repeat ? 1 : 0;
        scene["media"] = QJsonArray{entry};
        controller.onRemoteSceneStart(QStringLiteral("playback-owner"), scene);
        QTRY_VERIFY_WITH_TIMEOUT(controller.m_sceneActivationRequested, 5000);
        const auto item = controller.m_mediaItems.first();
        QVERIFY(item->player);
        if (endDelayMs < 0) {
            // Signed end offsets must also survive authoritative resume snapshots.
            const QJsonArray videos{QJsonObject{
                {"mediaId", "video-1"}, {"positionMs", 1000.0},
                {"durationMs", double(item->player->duration())}, {"playing", false},
                {"muted", true}, {"visible", true}, {"repeatAvailable", false}
            }};
            QVERIFY(controller.applyAuthoritativeStateSnapshot(snapshotForScene(scene, videos), 1, 0));
        }
        int wraps = 0;
        qint64 previous = 1000;
        bool hiddenBeforeFinalRepeat = false;
        connect(item->player, &ResidentVideoPlayer::positionChanged, &controller, [&](qint64 position) {
            if (previous >= 1600 && position == 1000) {
                ++wraps;
                hiddenBeforeFinalRepeat = hiddenBeforeFinalRepeat || !item->renderVisible;
            }
            previous = position;
        });
        controller.activateScene();
        QVERIFY(controller.m_sceneActivated);
        QTest::qWait(70);
        QCOMPARE(item->player->playbackState(), QMediaPlayer::PausedState);
        QVERIFY(item->audio->isMuted());
        QTRY_COMPARE_WITH_TIMEOUT(item->player->playbackState(), QMediaPlayer::PlayingState, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!item->audio->isMuted() && item->audio->volume() > 0.0, 1000);
        if (pauseEarly) {
            QTRY_COMPARE_WITH_TIMEOUT(item->player->playbackState(), QMediaPlayer::PausedState, 1000);
            QVERIFY(item->player->position() > 1000);
            QVERIFY(item->player->position() < 1700);
            QVERIFY(item->renderVisible);
            QTest::qWait(200);
            QCOMPARE(item->player->playbackState(), QMediaPlayer::PausedState);
        } else {
            if (endDelayMs > 0) {
                QTRY_COMPARE_WITH_TIMEOUT(item->player->playbackState(), QMediaPlayer::PausedState, 3000);
                QVERIFY(item->renderVisible);
                QVERIFY(!item->audio->isMuted());
            }
            QTRY_VERIFY_WITH_TIMEOUT(!item->renderVisible, 3500);
            if (endDelayMs < 0) {
                QVERIFY(item->player->position() < 1900);
                QCOMPARE(item->player->playbackState(), QMediaPlayer::PlayingState);
            }
            QTRY_VERIFY_WITH_TIMEOUT(item->audio->isMuted(), 1000);
            QTRY_COMPARE_WITH_TIMEOUT(item->player->playbackState(), QMediaPlayer::PausedState, 1000);
            QCOMPARE(item->player->position(), 1900);
            QCOMPARE(wraps, repeat ? 1 : 0);
            QVERIFY(!hiddenBeforeFinalRepeat);
        }
        controller.onRemoteSceneStop(QStringLiteral("playback-owner"),
                                     scene.value("sceneInstanceId").toString());
        QTRY_VERIFY_WITH_TIMEOUT(!findRemoteWindow(), 2000);
        files.removeReceivedFileMapping(fileId);
        residency.release(owner);
    }

    void clockSnapshotsPreserveFades()
    {
        RemoteSceneController controller(nullptr, nullptr);
        auto item = std::make_shared<RemoteSceneController::RemoteMediaItem>();
        item->mediaId = QStringLiteral("video-1");
        item->type = QStringLiteral("video");
        item->player = new ResidentVideoPlayer(&controller);
        item->audio = new QAudioOutput(&controller);
        item->player->setAudioOutput(item->audio);
        item->muted = true;
        item->audio->setMuted(true);
        item->audio->setVolume(0.0);
        item->volume = 0.75;
        item->audioFadeInSeconds = 0.4;
        item->audioFadeOutSeconds = 0.4;
        item->fadeInSeconds = 0.4;
        item->contentVisible = false;
        item->renderVisible = false;
        item->spans.append(RemoteSceneController::RemoteMediaItem::Span{});
        controller.m_mediaItems.append(item);
        controller.m_sceneActivated = true;
        controller.m_pendingSenderClientId = QStringLiteral("audio-owner");
        controller.m_pendingSceneInstanceId = QStringLiteral("audio-run");
        qint64 sequence = 0;
        auto sync = [&](bool muted) {
            controller.onRemoteSceneVideoSync(QStringLiteral("audio-owner"), QStringLiteral("audio-run"),
                ++sequence, QDateTime::currentMSecsSinceEpoch(), QJsonArray{QJsonObject{
                    {"mediaId", "video-1"}, {"positionMs", 0.0}, {"durationMs", 0.0},
                    {"playing", false}, {"muted", muted}, {"visible", true},
                    {"repeatAvailable", false}
                }});
        };
        sync(false);
        QTRY_VERIFY_WITH_TIMEOUT(item->audio->volume() > 0.0 && item->audio->volume() < 0.75, 1000);
        QVERIFY(item->renderOpacity > 0.0 && item->renderOpacity < 1.0);
        QPointer<QVariantAnimation> fade = item->audioFadeAnimation;
        QPointer<QVariantAnimation> visualFade = item->visualFadeAnimation;
        QVERIFY(fade);
        QVERIFY(visualFade);
        sync(false);
        QCOMPARE(item->audioFadeAnimation, fade);
        QCOMPARE(item->visualFadeAnimation, visualFade);
        QVERIFY(item->audio->volume() < 0.75);
        QTRY_COMPARE_WITH_TIMEOUT(item->audio->volume(), 0.75, 1000);
        sync(true);
        QTRY_VERIFY_WITH_TIMEOUT(item->audio->volume() > 0.0 && item->audio->volume() < 0.75, 1000);
        fade = item->audioFadeAnimation;
        sync(true);
        QCOMPARE(item->audioFadeAnimation, fade);
        QTRY_VERIFY_WITH_TIMEOUT(item->audio->isMuted(), 1000);
        QCOMPARE(item->audio->volume(), 0.0);
    }

    void simultaneousUnmuteAndMuteEndsMuted()
    {
        RemoteSceneController controller(nullptr, nullptr);
        auto item = std::make_shared<RemoteSceneController::RemoteMediaItem>();
        item->sceneEpoch = controller.m_sceneEpoch;
        item->audio = new QAudioOutput(&controller);
        item->muted = true;
        item->audio->setMuted(true);
        item->autoMute = true;
        item->autoMuteDelayMs = 0;
        controller.m_mediaItems.append(item);
        // Match activation's insertion order: unmute first, then auto-mute.
        QTimer::singleShot(0, &controller, [&] { controller.applyAudioMuteState(item, false); });
        controller.scheduleMuteTimer(item);
        QTest::qWait(50);
        QVERIFY(item->muted);
        QVERIFY(item->audio->isMuted());
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

    void boundedRemotePlayback_data()
    {
        QTest::addColumn<bool>("loop");
        QTest::addColumn<bool>("naturalEnd");
        QTest::newRow("stop-at-end") << false << false;
        QTest::newRow("loop-to-start") << true << false;
        QTest::newRow("start-only-loop") << true << true;
    }

    void boundedRemotePlayback()
    {
        QFETCH(bool, loop);
        QFETCH(bool, naturalEnd);
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        QVERIFY(QFile::exists(fixture));
        const QString fileId(64, QLatin1Char('c'));
        FileManager files;
        files.registerReceivedFilePath(fileId, fixture);
        const QString residentOwner = UploadManager::residencyOwnerId({}, 0, fileId);
        auto& residency = MediaResidencyManager::instance();
        residency.acquire(residentOwner, fixture);
        QTRY_VERIFY_WITH_TIMEOUT(residency.ready(residentOwner), 60000);
        RemoteSceneController controller(&files, nullptr);
        auto scene = videoScene(fileId);
        auto entries = scene.value("media").toArray();
        auto entry = entries[0].toObject();
        entry["startPositionMs"] = 1000;
        if (!naturalEnd) entry["endPositionMs"] = 1800;
        entry["continuousLoop"] = loop;
        entry["autoPlay"] = false;
        entries[0] = entry;
        scene["media"] = entries;
        QVERIFY(QMetaObject::invokeMethod(&controller, "onRemoteSceneStart", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("range-owner")), Q_ARG(QJsonObject, scene)));
        auto* player = controller.findChild<ResidentVideoPlayer*>();
        QVERIFY(player);
        int wraps = 0;
        qint64 previous = 0;
        connect(player, &ResidentVideoPlayer::positionChanged, &controller, [&](qint64 pos) {
            if (previous >= 1500 && pos == 1000) ++wraps;
            previous = pos;
        });
        QTRY_VERIFY_WITH_TIMEOUT(player->position() == 1000
            && player->playbackState() == QMediaPlayer::PausedState, 5000);
        // Drive the renderer through the same authoritative playback snapshot
        // used by the owner; preparation alone deliberately never auto-plays.
        const QJsonArray videos{QJsonObject{
            {"mediaId", "video-1"},
            {"positionMs", naturalEnd ? double(player->duration() - 1000) : 1000.0},
            {"durationMs", double(player->duration())}, {"playing", true},
            {"muted", true}, {"visible", true}, {"repeatAvailable", loop}
        }};
        bool applied = false;
        QVERIFY(QMetaObject::invokeMethod(&controller, "applyAuthoritativeStateSnapshot", Qt::DirectConnection,
            Q_RETURN_ARG(bool, applied),
            Q_ARG(QJsonObject, snapshotForScene(scene, videos)),
            Q_ARG(quint64, quint64(1)), Q_ARG(qint64, qint64(0))));
        QVERIFY(applied);
        if (loop) {
            QTRY_VERIFY_WITH_TIMEOUT(wraps >= (naturalEnd ? 1 : 2), 5000);
            QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(player->position() == 1800
                && player->playbackState() == QMediaPlayer::PausedState, 5000);
            QCOMPARE(wraps, 0);
        }
        QVERIFY(QMetaObject::invokeMethod(&controller, "onRemoteSceneStop", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("range-owner")),
            Q_ARG(QString, QStringLiteral("video-lifecycle-test-run"))));
        files.removeReceivedFileMapping(fileId);
        residency.release(residentOwner);
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
        const QString residentOwner = UploadManager::residencyOwnerId({}, 0, fileId);
        auto& residency = MediaResidencyManager::instance();
        residency.acquire(residentOwner, fixture);
        QTRY_VERIFY_WITH_TIMEOUT(residency.ready(residentOwner), 60000);
        RemoteSceneController controller(&files, nullptr);
        QJsonObject scene = videoScene(fileId);
        QVERIFY(QMetaObject::invokeMethod(
            &controller, "onRemoteSceneStart", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("video-snapshot-owner")),
            Q_ARG(QJsonObject, scene)));
        ResidentVideoPlayer* player = controller.findChild<ResidentVideoPlayer*>();
        QAudioOutput* audio = controller.findChild<QAudioOutput*>();
        if (!player) {
            files.removeReceivedFileMapping(fileId);
        residency.release(residentOwner);
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
        residency.release(residentOwner);
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
