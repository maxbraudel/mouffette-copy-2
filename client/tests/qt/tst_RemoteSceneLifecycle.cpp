#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"

class RemoteSceneLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void testSceneRestoresImmutableDraftState()
    {
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create());
        QVERIFY(host);
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
        QCOMPARE(copy->position(), QPointF(120, 80));
        QCOMPARE(copy->baseSize(), QSize(640, 220));
        QCOMPARE(copy->scale(), 1.25);
        QCOMPARE(copy->settings().displayDelayText, QStringLiteral("1.375"));
        QCOMPARE(copy->settings().fadeInText, QStringLiteral("0.45"));
        QCOMPARE(copy->settings().opacityText, QStringLiteral("72.5"));
        QCOMPARE(restored->document()->cameraScale(), 1.7);
        QCOMPARE(restored->document()->cameraPanX(), 32.0);
        QCOMPARE(restored->document()->cameraPanY(), -14.0);
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
