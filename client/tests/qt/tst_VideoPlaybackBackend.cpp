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
#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacVideoThumbnailer.h"
#elif defined(Q_OS_WIN)
#include "backend/platform/windows/WindowsVideoThumbnailer.h"
#endif

class VideoPlaybackBackendTest final : public QObject
{
    Q_OBJECT

private slots:
    void nativePreparationReadsDisplaySizeAndFirstFrame()
    {
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFile::exists(fixture)) QSKIP("Optional real-video fixture is missing");
#ifdef Q_OS_MACOS
        QCOMPARE(MacVideoThumbnailer::videoDimensions(fixture), QSize(1920, 1080));
        QCOMPARE(MacVideoThumbnailer::firstFrame(fixture).size(), QSize(1920, 1080));
#elif defined(Q_OS_WIN)
        QCOMPARE(WindowsVideoThumbnailer::videoDimensions(fixture), QSize(1920, 1080));
        QCOMPARE(WindowsVideoThumbnailer::firstFrame(fixture).size(), QSize(1920, 1080));
#else
        QSKIP("Native first-frame preparation is implemented on macOS and Windows");
#endif
    }

    void qmlVideoOutputUsesDocumentRuntimeAndPreservesAudioState()
    {
        const QString fixture = QString::fromUtf8(TEST_VIDEO_FILE);
        if (!QFile::exists(fixture)) QSKIP("Optional real-video fixture is missing");

        CanvasDocument document;
        QuickCanvasController controller(&document);
        QString error;
        QVERIFY2(controller.initialize(&error), qPrintable(error));
        CanvasMedia* video = document.addPreparedFile(
            fixture, QSize(1920, 1080), true, QPointF(0, 0));
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
        QCOMPARE(projection.value(QStringLiteral("width")).toInt(), 1920);
        QVERIFY(projection.value(QStringLiteral("videoPlayerPtr")).value<QObject*>()
                == player);
    }
};

QTEST_MAIN(VideoPlaybackBackendTest)
#include "tst_VideoPlaybackBackend.moc"
