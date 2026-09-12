#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QPixmap>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "backend/domain/project/ProjectManager.h"
#include "backend/domain/project/ProjectStore.h"
#include "backend/domain/media/MediaItems.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

namespace {
QJsonObject mediaStateById(const QJsonObject& canvasState, const QString& mediaId)
{
    for (const QJsonValue& value : canvasState.value(QStringLiteral("media")).toArray()) {
        const QJsonObject media = value.toObject();
        if (media.value(QStringLiteral("mediaId")).toString() == mediaId) {
            return media;
        }
    }
    return {};
}

void compareMediaSettings(const ResizableMediaBase::MediaSettingsState& actual,
                          const ResizableMediaBase::MediaSettingsState& expected)
{
    QCOMPARE(actual.displayAutomatically, expected.displayAutomatically);
    QCOMPARE(actual.displayDelayEnabled, expected.displayDelayEnabled);
    QCOMPARE(actual.displayDelayText, expected.displayDelayText);
    QCOMPARE(actual.unmuteAutomatically, expected.unmuteAutomatically);
    QCOMPARE(actual.unmuteDelayEnabled, expected.unmuteDelayEnabled);
    QCOMPARE(actual.unmuteDelayText, expected.unmuteDelayText);
    QCOMPARE(actual.playAutomatically, expected.playAutomatically);
    QCOMPARE(actual.playDelayEnabled, expected.playDelayEnabled);
    QCOMPARE(actual.playDelayText, expected.playDelayText);
    QCOMPARE(actual.pauseDelayEnabled, expected.pauseDelayEnabled);
    QCOMPARE(actual.pauseDelayText, expected.pauseDelayText);
    QCOMPARE(actual.repeatEnabled, expected.repeatEnabled);
    QCOMPARE(actual.repeatCountText, expected.repeatCountText);
    QCOMPARE(actual.fadeInEnabled, expected.fadeInEnabled);
    QCOMPARE(actual.fadeInText, expected.fadeInText);
    QCOMPARE(actual.fadeOutEnabled, expected.fadeOutEnabled);
    QCOMPARE(actual.fadeOutText, expected.fadeOutText);
    QCOMPARE(actual.audioFadeInEnabled, expected.audioFadeInEnabled);
    QCOMPARE(actual.audioFadeInText, expected.audioFadeInText);
    QCOMPARE(actual.audioFadeOutEnabled, expected.audioFadeOutEnabled);
    QCOMPARE(actual.audioFadeOutText, expected.audioFadeOutText);
    QCOMPARE(actual.opacityOverrideEnabled, expected.opacityOverrideEnabled);
    QCOMPARE(actual.opacityText, expected.opacityText);
    QCOMPARE(actual.volumeOverrideEnabled, expected.volumeOverrideEnabled);
    QCOMPARE(actual.volumeText, expected.volumeText);
    QCOMPARE(actual.hideDelayEnabled, expected.hideDelayEnabled);
    QCOMPARE(actual.hideDelayText, expected.hideDelayText);
    QCOMPARE(actual.hideWhenVideoEnds, expected.hideWhenVideoEnds);
    QCOMPARE(actual.muteDelayEnabled, expected.muteDelayEnabled);
    QCOMPARE(actual.muteDelayText, expected.muteDelayText);
    QCOMPARE(actual.muteWhenVideoEnds, expected.muteWhenVideoEnds);
}

class TerminalSceneCanvas final : public ScreenCanvas {
public:
    void failRun(const QString& sceneRunId)
    {
        m_pendingRemoteSceneInstanceId = sceneRunId;
        m_sceneLaunching = true;
        failPendingSceneRun(QStringLiteral("decoder failed"), false);
    }

    void prepareStopTimeout(const QString& sceneRunId)
    {
        m_pendingRemoteSceneInstanceId = sceneRunId;
        m_sceneLaunched = true;
        m_sceneStopping = true;
        m_remoteSceneStopRetrySent = true;
    }
};
}

class RemoteSceneLifecycleTest final : public QObject {
    Q_OBJECT

private slots:
    void acknowledgedStopRestoresSelectedMediaWithoutReentrantSettingsPush();
    void liveRunCheckpointKeepsImmutableDraftAndStopRestoresRuntimeState();
    void projectRoundTripPreservesRawSettingsAndPausedVideoPosition();
    void terminalSceneNotificationsDeduplicateLateAcknowledgements();
};

void RemoteSceneLifecycleTest::terminalSceneNotificationsDeduplicateLateAcknowledgements()
{
    QStandardPaths::setTestModeEnabled(true);
    const QString previousApplicationName = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName(
        QStringLiteral("Mouffette-TerminalScene-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QString historyPath = HistoryStore::defaultFilePath();

    {
        TerminalSceneCanvas canvas;
        ToastNotificationSystem notifications(&canvas);
        ToastNotificationSystem::setInstance(&notifications);
        NotificationCenter* center = notifications.notificationCenter();
        QVERIFY(center);
        QSignalSpy toastSpy(center, &NotificationCenter::toastRequested);

        const QString failedRun =
            QStringLiteral("11111111-2222-4333-8444-555566667788");
        canvas.failRun(failedRun);
        QCOMPARE(center->entries().size(), 1);
        QCOMPARE(toastSpy.count(), 1);
        QCOMPARE(center->entries().first().sceneRunId, failedRun);
        QCOMPARE(center->entries().first().correlationId,
                 NotificationCorrelation::sceneRun(failedRun));
        QVERIFY(center->entries().first().terminal);
        QCOMPARE(center->entries().first().severity, NotificationSeverity::Error);

        const QJsonObject lateFailureAcknowledgement{
            {QStringLiteral("sceneRunId"), failedRun},
        };
        QVERIFY(QMetaObject::invokeMethod(
            &canvas, "onSceneStoppedReceived", Qt::DirectConnection,
            Q_ARG(QJsonObject, lateFailureAcknowledgement)));
        QCOMPARE(center->entries().size(), 1);
        QCOMPARE(toastSpy.count(), 1);

        const QString timedOutRun =
            QStringLiteral("88888888-7777-4666-8555-444433332222");
        canvas.prepareStopTimeout(timedOutRun);
        QVERIFY(QMetaObject::invokeMethod(
            &canvas, "onRemoteSceneStopTimeout", Qt::DirectConnection));
        QCOMPARE(center->entries().size(), 2);
        QCOMPARE(toastSpy.count(), 2);
        QCOMPARE(center->entries().first().sceneRunId, timedOutRun);
        QCOMPARE(center->entries().first().correlationId,
                 NotificationCorrelation::sceneRun(timedOutRun));
        QVERIFY(center->entries().first().terminal);
        QCOMPARE(center->entries().first().severity, NotificationSeverity::Warning);

        const QJsonObject lateStopAcknowledgement{
            {QStringLiteral("sceneRunId"), timedOutRun},
        };
        QVERIFY(QMetaObject::invokeMethod(
            &canvas, "onSceneStoppedReceived", Qt::DirectConnection,
            Q_ARG(QJsonObject, lateStopAcknowledgement)));
        QCOMPARE(center->entries().size(), 2);
        QCOMPARE(toastSpy.count(), 2);
        ToastNotificationSystem::setInstance(nullptr);
    }

    QVERIFY(!QFileInfo::exists(historyPath) || QFile::remove(historyPath));
    QCoreApplication::setApplicationName(previousApplicationName);
}

void RemoteSceneLifecycleTest::acknowledgedStopRestoresSelectedMediaWithoutReentrantSettingsPush()
{
    ScreenCanvas canvas;
    canvas.resize(960, 600);
    canvas.setScreens({ScreenInfo(1, 1920, 1080, 0, 0, true)});

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

    // The local graph uses the same selection lock/restoration path as a
    // committed remote SceneRun, without bypassing protocol-v2 authentication
    // in this focused regression test.
    canvas.startHostSceneState(ScreenCanvas::HostSceneMode::Test);
    QVERIFY(canvas.isHostSceneActive());
    QVERIFY(!media->isSelected());
    canvas.stopHostSceneState(false);

    // This selection restoration used to recurse synchronously between
    // pullSettingsFromMedia() and pushSettingsToMedia() until the GUI thread
    // exhausted its stack and crashed.
    QVERIFY(!canvas.isHostSceneActive());
    QVERIFY(media->isSelected());
}

void RemoteSceneLifecycleTest::liveRunCheckpointKeepsImmutableDraftAndStopRestoresRuntimeState()
{
    ScreenCanvas canvas;
    canvas.resize(960, 600);
    canvas.setScreens({ScreenInfo(1, 1920, 1080, 0, 0, true)});

    QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::cyan);
    auto* hiddenImage = new ResizablePixmapItem(
        QPixmap::fromImage(image), 8, 16, QStringLiteral("hidden.png"));
    hiddenImage->restorePersistentMediaId(QStringLiteral("hidden-image"));
    hiddenImage->setFileId(QString(64, QLatin1Char('a')));
    canvas.scene()->addItem(hiddenImage);
    hiddenImage->hideImmediateNoFade();

    auto* video = new ResizableVideoItem(
        QString(), QSize(640, 360), 8, 16, QStringLiteral("draft.mp4"));
    video->restorePersistentMediaId(QStringLiteral("draft-video"));
    video->setFileId(QString(64, QLatin1Char('b')));
    auto videoSettings = video->mediaSettingsState();
    videoSettings.playAutomatically = false;
    video->setMediaSettingsState(videoSettings);
    canvas.scene()->addItem(video);
    video->showImmediateNoFade();
    video->pauseAndSetPosition(3'210);
    video->setMuted(false, true);

    const QJsonObject draft = canvas.serializeProjectState();
    QCOMPARE(mediaStateById(draft, QStringLiteral("hidden-image"))
                 .value(QStringLiteral("visible")).toBool(), false);
    QCOMPARE(qRound64(mediaStateById(draft, QStringLiteral("draft-video"))
                          .value(QStringLiteral("startPositionMs")).toDouble()),
             qint64(3'210));

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
    ProjectManager projects(&store);
    projects.stopAutomaticTimersForTesting();
    ClientInfo target(QStringLiteral("device-a"), QStringLiteral("Studio A"),
                      QStringLiteral("Linux"));
    target.setClientId(QStringLiteral("device-a"));
    QVERIFY(!projects.ensureProject(
        ClientSnapshot::fromClientInfo(target, 1'000),
        ProjectLifecycleState::Visible, 1'000).isEmpty());
    QVERIFY(projects.updateCanvasState(QStringLiteral("device-a"), draft, {}, 1'000));
    connect(&projects, &ProjectManager::projectCheckpointDue, &projects,
            [&canvas, &projects](const QString& targetDeviceId) {
        projects.updateCanvasState(targetDeviceId, canvas.serializeProjectState(), {}, 15'000);
    });

    canvas.startHostSceneState(ScreenCanvas::HostSceneMode::Test);
    QVERIFY(canvas.isHostSceneActive());

    // Model the mutable execution graph after automation/playback has started.
    hiddenImage->showImmediateNoFade();
    video->hideImmediateNoFade();
    video->pauseAndSetPosition(7'777);
    video->setMuted(true, true);

    const QJsonObject runtime = canvas.serializeSceneState();
    QVERIFY(mediaStateById(runtime, QStringLiteral("hidden-image"))
                .value(QStringLiteral("visible")).toBool());
    QCOMPARE(qRound64(mediaStateById(runtime, QStringLiteral("draft-video"))
                          .value(QStringLiteral("startPositionMs")).toDouble()),
             qint64(7'777));

    // This is the exact path used by the 15-second visible-project checkpoint.
    QCOMPARE(canvas.serializeProjectState(), draft);
    projects.checkpointVisibleProjects(15'000);
    const ProjectRecord* checkpointed =
        projects.projectForTarget(QStringLiteral("device-a"));
    QVERIFY(checkpointed);
    QCOMPARE(checkpointed->canvasState, draft);

    canvas.stopHostSceneState(false);
    QVERIFY(!canvas.isHostSceneActive());
    QVERIFY(!hiddenImage->isContentVisible());
    QVERIFY(video->isContentVisible());
    QVERIFY(!video->isPlaying());
    QCOMPARE(video->currentPositionMs(), qint64(3'210));
    QVERIFY(!video->isMuted());

    const QJsonObject restored = canvas.serializeProjectState();
    QCOMPARE(mediaStateById(restored, QStringLiteral("hidden-image"))
                 .value(QStringLiteral("visible")).toBool(), false);
    QCOMPARE(mediaStateById(restored, QStringLiteral("draft-video"))
                 .value(QStringLiteral("visible")).toBool(), true);
    QCOMPARE(qRound64(mediaStateById(restored, QStringLiteral("draft-video"))
                          .value(QStringLiteral("startPositionMs")).toDouble()),
             qint64(3'210));
    QCOMPARE(mediaStateById(restored, QStringLiteral("draft-video"))
                 .value(QStringLiteral("muted")).toBool(), false);

    // The strict lease-terminal path must converge to the same immutable
    // draft, not leave the last SceneRun frame in the editor or on disk.
    const QJsonObject draftBeforeLeaseExpiry = canvas.serializeProjectState();
    canvas.startHostSceneState(ScreenCanvas::HostSceneMode::Remote);
    hiddenImage->showImmediateNoFade();
    video->hideImmediateNoFade();
    video->pauseAndSetPosition(9'999);
    video->setMuted(true, true);
    QCOMPARE(canvas.serializeProjectState(), draftBeforeLeaseExpiry);

    canvas.handleRemoteConnectionLost();
    QVERIFY(!canvas.isHostSceneActive());
    QVERIFY(!hiddenImage->isContentVisible());
    QVERIFY(video->isContentVisible());
    QVERIFY(!video->isPlaying());
    QCOMPARE(video->currentPositionMs(), qint64(3'210));
    QVERIFY(!video->isMuted());
    QCOMPARE(canvas.serializeProjectState(), draftBeforeLeaseExpiry);
}

void RemoteSceneLifecycleTest::projectRoundTripPreservesRawSettingsAndPausedVideoPosition()
{
    const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
    QVERIFY2(QFile::exists(fixture),
             qPrintable(QStringLiteral("Missing test video: %1").arg(fixture)));

    ScreenCanvas source;
    source.resize(960, 600);
    source.setScreens({ScreenInfo(1, 1920, 1080, 0, 0, true)});

    auto* video = new ResizableVideoItem(
        fixture, QSize(640, 360), 8, 16, QStringLiteral("settings.mp4"));
    video->restorePersistentMediaId(QStringLiteral("settings-video"));
    video->setFileId(QString(64, QLatin1Char('c')));
    QImage poster(640, 360, QImage::Format_RGB32);
    poster.fill(Qt::black);
    video->setExternalPosterImage(poster, poster.size());
    source.scene()->addItem(video);

    ResizableMediaBase::MediaSettingsState expected;
    expected.displayAutomatically = false;
    expected.displayDelayEnabled = true;
    expected.displayDelayText = QStringLiteral(" 01,250 ");
    expected.unmuteAutomatically = false;
    expected.unmuteDelayEnabled = true;
    expected.unmuteDelayText = QStringLiteral("0,375");
    expected.playAutomatically = true;
    expected.playDelayEnabled = false;
    expected.playDelayText = QStringLiteral("02.500");
    expected.pauseDelayEnabled = true;
    expected.pauseDelayText = QStringLiteral("3,125");
    expected.repeatEnabled = false;
    expected.repeatCountText = QStringLiteral("0007");
    expected.fadeInEnabled = true;
    expected.fadeInText = QStringLiteral("0.125");
    expected.fadeOutEnabled = false;
    expected.fadeOutText = QStringLiteral("4,750");
    expected.audioFadeInEnabled = false;
    expected.audioFadeInText = QStringLiteral("5.0625");
    expected.audioFadeOutEnabled = true;
    expected.audioFadeOutText = QStringLiteral("0,875");
    expected.opacityOverrideEnabled = true;
    expected.opacityText = QStringLiteral("037");
    expected.volumeOverrideEnabled = true;
    expected.volumeText = QStringLiteral("43,75");
    expected.hideDelayEnabled = false;
    expected.hideDelayText = QStringLiteral("6.625");
    expected.hideWhenVideoEnds = true;
    expected.muteDelayEnabled = true;
    expected.muteDelayText = QStringLiteral("7,875");
    expected.muteWhenVideoEnds = false;

    // The effective values intentionally differ from the raw settings text:
    // a checkpoint must preserve both layers without one recomputing the other.
    video->setMediaSettingsState(expected);
    video->setContentOpacity(0.62);
    video->restoreEffectiveVolume(0.71);
    video->setMuted(true, true);
    video->restorePausedPosition(4'321);

    QTRY_VERIFY_WITH_TIMEOUT(
        video->mediaPlayer()->playbackState() != QMediaPlayer::PlayingState, 4000);
    QTRY_COMPARE_WITH_TIMEOUT(video->currentPositionMs(), qint64(4'321), 4000);
    QVERIFY(!video->isPlaying());

    const QJsonObject sceneManifest = source.serializeSceneState();
    QVERIFY(!mediaStateById(sceneManifest, QStringLiteral("settings-video"))
                 .contains(QStringLiteral("projectMediaSettings")));

    const QJsonObject projectSnapshot = source.serializeProjectState();
    const QJsonObject serializedMedia =
        mediaStateById(projectSnapshot, QStringLiteral("settings-video"));
    const QJsonObject serializedSettings =
        serializedMedia.value(QStringLiteral("projectMediaSettings")).toObject();
    QCOMPARE(serializedSettings.value(QStringLiteral("schemaVersion")).toInt(), 1);
    QCOMPARE(serializedSettings.value(QStringLiteral("displayDelayText")).toString(),
             expected.displayDelayText);
    QCOMPARE(serializedSettings.value(QStringLiteral("volumeText")).toString(),
             expected.volumeText);
    QCOMPARE(serializedMedia.value(QStringLiteral("contentOpacity")).toDouble(), 0.62);
    QCOMPARE(serializedMedia.value(QStringLiteral("volume")).toDouble(), 0.71);
    QCOMPARE(qRound64(serializedMedia.value(QStringLiteral("startPositionMs")).toDouble()),
             qint64(4'321));

    // Exercise the exact JSON representation that ProjectStore writes rather
    // than passing an in-memory object directly to restoration.
    QJsonParseError parseError;
    const QJsonObject persistedSnapshot = QJsonDocument::fromJson(
        QJsonDocument(projectSnapshot).toJson(QJsonDocument::Compact),
        &parseError).object();
    QCOMPARE(parseError.error, QJsonParseError::NoError);

    ScreenCanvas restored;
    restored.resize(960, 600);
    QHash<QString, QString> sourcePaths;
    sourcePaths.insert(QStringLiteral("settings-video"), fixture);
    QStringList skippedMediaIds;
    QVERIFY(restored.restoreProjectState(
        persistedSnapshot, sourcePaths, &skippedMediaIds));
    QVERIFY(skippedMediaIds.isEmpty());

    ResizableVideoItem* restoredVideo = nullptr;
    for (QGraphicsItem* item : restored.scene()->items()) {
        auto* candidate = dynamic_cast<ResizableVideoItem*>(item);
        if (candidate && candidate->mediaId() == QLatin1String("settings-video")) {
            restoredVideo = candidate;
            break;
        }
    }
    QVERIFY(restoredVideo);
    compareMediaSettings(restoredVideo->mediaSettingsState(), expected);
    QVERIFY(qAbs(restoredVideo->contentOpacity() - 0.62) < 0.000001);
    QVERIFY(qAbs(restoredVideo->volume() - 0.71) < 0.000001);
    QVERIFY(restoredVideo->isMuted());
    QVERIFY(!restoredVideo->isPlaying());

    QTRY_VERIFY_WITH_TIMEOUT(
        restoredVideo->mediaPlayer()->playbackState() != QMediaPlayer::PlayingState, 4000);
    QTRY_COMPARE_WITH_TIMEOUT(restoredVideo->currentPositionMs(), qint64(4'321), 4000);
    compareMediaSettings(restoredVideo->mediaSettingsState(), expected);
    QVERIFY(qAbs(restoredVideo->contentOpacity() - 0.62) < 0.000001);
    QVERIFY(qAbs(restoredVideo->volume() - 0.71) < 0.000001);
}

QTEST_MAIN(RemoteSceneLifecycleTest)
#include "tst_RemoteSceneLifecycle.moc"
