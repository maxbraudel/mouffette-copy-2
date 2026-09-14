#include <QApplication>
#include <QAudioOutput>
#include <QFile>
#include <QMediaPlayer>
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
        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(video->isPlaying(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(video->positionMs() > 100, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !controller.dropPreviewModel()
                 .value(QStringLiteral("visible")).toBool(),
            5000);
        video->togglePlayPause();
        QTRY_VERIFY_WITH_TIMEOUT(!video->isPlaying(), 3000);
    }
};

QTEST_MAIN(VideoPlaybackBackendTest)
#include "tst_VideoPlaybackBackend.moc"
