#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"

class RemoteSceneLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void testSceneRestoresImmutableDraftState()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
        host->setProjectEditingEnabled(true);
        CanvasMedia* media = host->document()->addText(
            QPointF(40, 60), QStringLiteral("Scene title"));
        QVERIFY(media);
        media->setContentVisible(true);
        MediaSettingsState settings = media->settings();
        settings.displayAutomatically = false;
        settings.displayDelayEnabled = false;
        media->setSettings(settings);
        host->document()->select(media->mediaId());
        host->setOverlayActionsEnabled(true);
        QVERIFY(host->testSceneActionEnabled());

        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
        QVERIFY(host->document()->editsLocked());
        QVERIFY(host->document()->selectedMediaIds().isEmpty());
        QTRY_VERIFY(!media->contentVisible());

        // Scene playback may mutate runtime state, but stop restores the draft.
        media->setAnimatedDisplayOpacity(0.35);
        host->triggerTestSceneAction();
        QVERIFY(!host->testSceneLaunched());
        QVERIFY(!host->document()->editsLocked());
        QVERIFY(media->contentVisible());
        QCOMPARE(media->animatedDisplayOpacity(), 1.0);
    }

    void projectRoundTripPreservesTypedSettingsGeometryAndText()
    {
        std::unique_ptr<QuickCanvasHost> source(QuickCanvasHost::create());
        QVERIFY(source);
        source->setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        CanvasMedia* text = source->document()->addText(
            QPointF(120, 80), QStringLiteral("Round trip"));
        QVERIFY(text);
        text->setBaseSize(QSize(640, 220));
        text->setScale(1.25);
        text->setTextColor(QColor(QStringLiteral("#ff55aa")));
        text->setTextColorOverrideEnabled(true);
        text->setOutlineWidthPercent(8.5);
        text->setOutlineWidthOverrideEnabled(true);
        text->setItalic(true);
        text->setUppercase(true);
        MediaSettingsState settings = text->settings();
        settings.displayDelayEnabled = true;
        settings.displayDelayText = QStringLiteral("1.375");
        settings.fadeInEnabled = true;
        settings.fadeInText = QStringLiteral("0.45");
        settings.opacityOverrideEnabled = true;
        settings.opacityText = QStringLiteral("72.5");
        text->setSettings(settings);
        source->document()->setCamera(1.7, 32, -14);
        const QPointF expectedPosition = text->position();
        const QSize expectedBaseSize = text->baseSize();

        const QJsonObject state = source->serializeProjectState();
        std::unique_ptr<QuickCanvasHost> restored(QuickCanvasHost::create());
        QVERIFY(restored);
        QStringList skipped;
        QVERIFY(restored->restoreProjectState(state, {}, &skipped));
        QVERIFY(skipped.isEmpty());
        QCOMPARE(restored->enumerateMediaItems().size(), 1);
        CanvasMedia* copy = restored->enumerateMediaItems().first();
        QVERIFY(copy && copy->isText());
        QCOMPARE(copy->mediaId(), text->mediaId());
        QCOMPARE(copy->text(), QStringLiteral("Round trip"));
        QCOMPARE(copy->position(), expectedPosition);
        QCOMPARE(copy->baseSize(), expectedBaseSize);
        QCOMPARE(copy->scale(), 1.25);
        QCOMPARE(copy->settings().displayDelayText, QStringLiteral("1.375"));
        QCOMPARE(copy->settings().fadeInText, QStringLiteral("0.45"));
        QCOMPARE(copy->settings().opacityText, QStringLiteral("72.5"));
        QVERIFY(copy->textColorOverrideEnabled());
        QVERIFY(copy->outlineWidthOverrideEnabled());
        QCOMPARE(copy->textColor(), QColor(QStringLiteral("#ff55aa")));
        QCOMPARE(copy->outlineWidthPercent(), 8.5);
        QCOMPARE(restored->document()->cameraScale(), 1.7);
        QCOMPARE(restored->document()->cameraPanX(), 32.0);
        QCOMPARE(restored->document()->cameraPanY(), -14.0);
    }

    void disabledOverridesKeepRawValuesAndPublishNeutralRendering()
    {
        std::unique_ptr<QuickCanvasHost> source(QuickCanvasHost::create());
        QVERIFY(source);
        CanvasMedia* text = source->document()->addText(
            QPointF(100, 100), QStringLiteral("Overrides"));
        QVERIFY(text);
        text->setFitToTextEnabled(false);
        text->setFontWeight(900);
        text->setTextColor(QColor(QStringLiteral("#8044aa22")));
        text->setOutlineWidthPercent(100.0);
        text->setOutlineColor(QColor(QStringLiteral("#ff22ccdd")));

        MediaSettingsState settings = text->settings();
        settings.opacityOverrideEnabled = false;
        settings.opacityText = QStringLiteral("23.5");
        text->setSettings(settings);

        QCOMPARE(text->fontWeight(), 900);
        QCOMPARE(text->textColor(), QColor(QStringLiteral("#8044aa22")));
        QCOMPARE(text->outlineWidthPercent(), 100.0);
        QCOMPARE(text->outlineColor(), QColor(QStringLiteral("#ff22ccdd")));
        QCOMPARE(text->contentOpacity(), 1.0);

        const QVariantMap disabledProjection = text->toModelMap();
        QCOMPARE(disabledProjection.value(
                     QStringLiteral("textFontWeight")).toInt(), 400);
        QCOMPARE(QColor(disabledProjection.value(
                     QStringLiteral("textColor")).toString()),
                 QColor(Qt::white));
        QCOMPARE(disabledProjection.value(
                     QStringLiteral("textOutlineWidthPercent")).toDouble(),
                 0.0);
        QCOMPARE(QColor(disabledProjection.value(
                     QStringLiteral("textOutlineColor")).toString()),
                 QColor(Qt::black));

        const QJsonObject project = source->serializeProjectState();
        const QJsonObject serializedText =
            project.value(QStringLiteral("media")).toArray().first().toObject();
        QCOMPARE(serializedText.value(QStringLiteral("contentOpacity")).toDouble(),
                 1.0);
        QCOMPARE(serializedText.value(QStringLiteral("fontWeight")).toInt(), 400);
        QCOMPARE(serializedText.value(
                     QStringLiteral("textBorderWidthPercent")).toDouble(),
                 0.0);
        const QJsonObject rawTextSettings = serializedText.value(
            QStringLiteral("projectTextSettings")).toObject();
        QCOMPARE(rawTextSettings.value(QStringLiteral("fontWeight")).toInt(), 900);
        QCOMPARE(rawTextSettings.value(
                     QStringLiteral("textBorderWidthPercent")).toDouble(),
                 100.0);
        QVERIFY(!rawTextSettings.value(
                     QStringLiteral("fontWeightOverrideEnabled")).toBool(true));
        QVERIFY(!rawTextSettings.value(
                     QStringLiteral("textBorderWidthOverrideEnabled")).toBool(true));

        std::unique_ptr<QuickCanvasHost> restored(QuickCanvasHost::create());
        QVERIFY(restored);
        QVERIFY(restored->restoreProjectState(project, {}));
        CanvasMedia* copy = restored->enumerateMediaItems().first();
        QVERIFY(copy);
        QVERIFY(!copy->fontWeightOverrideEnabled());
        QVERIFY(!copy->textColorOverrideEnabled());
        QVERIFY(!copy->outlineWidthOverrideEnabled());
        QVERIFY(!copy->outlineColorOverrideEnabled());
        QCOMPARE(copy->fontWeight(), 900);
        QCOMPARE(copy->textColor(), QColor(QStringLiteral("#8044aa22")));
        QCOMPARE(copy->outlineWidthPercent(), 100.0);
        QCOMPARE(copy->outlineColor(), QColor(QStringLiteral("#ff22ccdd")));
        QCOMPARE(copy->settings().opacityText, QStringLiteral("23.5"));
        QCOMPARE(copy->contentOpacity(), 1.0);

        copy->setFontWeightOverrideEnabled(true);
        copy->setTextColorOverrideEnabled(true);
        copy->setOutlineWidthOverrideEnabled(true);
        copy->setOutlineColorOverrideEnabled(true);
        settings = copy->settings();
        settings.opacityOverrideEnabled = true;
        copy->setSettings(settings);
        const QVariantMap enabledProjection = copy->toModelMap();
        QCOMPARE(enabledProjection.value(
                     QStringLiteral("textFontWeight")).toInt(), 900);
        QCOMPARE(QColor(enabledProjection.value(
                     QStringLiteral("textColor")).toString()),
                 QColor(QStringLiteral("#8044aa22")));
        QCOMPARE(enabledProjection.value(
                     QStringLiteral("textOutlineWidthPercent")).toDouble(),
                 100.0);
        QCOMPARE(QColor(enabledProjection.value(
                     QStringLiteral("textOutlineColor")).toString()),
                 QColor(QStringLiteral("#ff22ccdd")));
        QVERIFY(qAbs(copy->contentOpacity() - 0.235) < 0.0001);

        CanvasMedia video(CanvasMedia::Type::Video, QSize(320, 180));
        video.initializeVideoRuntime();
        MediaSettingsState videoSettings = video.settings();
        videoSettings.volumeOverrideEnabled = true;
        videoSettings.volumeText = QStringLiteral("37");
        video.setSettings(videoSettings);
        QVERIFY(qAbs(video.volume() - 0.37) < 0.0001);
        videoSettings.volumeOverrideEnabled = false;
        video.setSettings(videoSettings);
        QCOMPARE(video.settings().volumeText, QStringLiteral("37"));
        QVERIFY(qAbs(video.volume() - 1.0) < 0.0001);
        videoSettings.volumeOverrideEnabled = true;
        video.setSettings(videoSettings);
        QVERIFY(qAbs(video.volume() - 0.37) < 0.0001);

        QTemporaryDir volumeDirectory;
        QVERIFY(volumeDirectory.isValid());
        std::unique_ptr<QuickCanvasHost> volumeHost(QuickCanvasHost::create());
        QVERIFY(volumeHost);
        volumeHost->setProjectEditingEnabled(true);
        CanvasMedia* controlledVideo = volumeHost->document()->addPreparedFile(
            volumeDirectory.filePath(QStringLiteral("volume-setting-test.mp4")),
            QSize(320, 180), true, QPointF());
        QVERIFY(controlledVideo);
        volumeHost->controller()->handleOverlayVolumeChange(
            controlledVideo->mediaId(), 0.42);
        QVERIFY(controlledVideo->settings().volumeOverrideEnabled);
        QCOMPARE(controlledVideo->settings().volumeText, QStringLiteral("42"));
        QVERIFY(qAbs(controlledVideo->volume() - 0.42) < 0.0001);
    }

    void missingFileIsRejectedDuringRestore()
    {
        std::unique_ptr<QuickCanvasHost> source(QuickCanvasHost::create());
        QVERIFY(source);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("asset.png"));
        QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::blue);
        QVERIFY(image.save(path));
        CanvasMedia* media = source->document()->addPreparedFile(
            path, image.size(), false, QPointF(4, 5));
        QVERIFY(media);
        const QString id = media->mediaId();
        const QJsonObject state = source->serializeProjectState();

        std::unique_ptr<QuickCanvasHost> restored(QuickCanvasHost::create());
        QStringList skipped;
        QVERIFY(restored->restoreProjectState(state, {}, &skipped));
        QCOMPARE(restored->enumerateMediaItems().size(), 0);
        QCOMPARE(skipped, QStringList{id});
    }
};

QTEST_MAIN(RemoteSceneLifecycleTest)
#include "tst_RemoteSceneLifecycle.moc"
