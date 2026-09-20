#include "backend/media/EditingProxyCache.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "backend/media/MediaPreviewStore.h"
#include "backend/media/ResidentVideoPlayer.h"
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QUuid>
#include <QtTest>
#include <algorithm>

class MediaEditingCacheTest : public QObject {
    Q_OBJECT
    QString cacheDirectory;
    std::shared_ptr<ResidentMediaAsset> original(const char* test) {
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.retainOriginalVideo = true;
        QString error;
        auto asset = MediaDecoder::decode(QString::fromUtf8(TEST_VIDEO_FILE), callbacks, &error);
        if (asset) asset->sha256 += QString::fromLatin1(test);
        return asset;
    }
private slots:
    void initTestCase() {
        QCoreApplication::setApplicationName(QStringLiteral("MouffetteEditingCacheTest-") + QUuid::createUuid().toString(QUuid::Id128));
        cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QVERIFY(!cacheDirectory.isEmpty());
    }
    void diskFileLimitAppliesAtStartupAndDuringWrites() {
        QDir disk(cacheDirectory + QStringLiteral("/media-previews-v1/scrub"));
        QVERIFY(disk.mkpath(QStringLiteral(".")));
        // Simulate stale derivative files inherited from an earlier process.
        // File-count limits matter independently of the byte budget.
        for (int i = 0; i < 12001; ++i) {
            QFile file(disk.filePath(QStringLiteral("stale-%1.jpg").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        auto asset = original("file-limit"); QVERIFY(asset);
        QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::blue);
        for (int i = 0; i < std::min(16, int(asset->frameIndex.size())); ++i) {
            QVERIFY(MediaPreviewStore::storeScrubFrame(*asset, i, image));
            QVERIFY(MediaPreviewStore::scrubFrame(*asset, i).isValid());
        }
        QVERIFY(disk.entryList({QStringLiteral("*.jpg")}, QDir::Files).size() <= 12000);
        // External cleanup is allowed; later writes reconcile missing entries.
        for (const auto& name : disk.entryList({QStringLiteral("stale-*.jpg")}, QDir::Files))
            QVERIFY(disk.remove(name));
    }
    void cancelledDecodeCanRecover() {
        auto asset = original("cancel"); QVERIFY(asset);
        IndexedMediaDecoder decoder;
        QString error;
        QVERIFY(!decoder.videoFrame(*asset, asset->frameIndex.size()-1, error, [] { return true; }).isValid());
        QCOMPARE(error, QStringLiteral("cancelled"));
        error.clear();
        const auto frame = decoder.videoFrame(*asset, asset->frameIndex.size()-1, error);
        QVERIFY2(frame.isValid(), qPrintable(error));
        QCOMPARE(frame.startTime(), asset->frameIndex.last().timestampUs);
        int checks = 0;
        QVERIFY(!decoder.videoFrame(*asset, 0, error, [&] { return ++checks > 2; }).isValid());
        error.clear();
        QVERIFY2(decoder.videoFrame(*asset, 0, error).isValid(), qPrintable(error));
    }
    void thumbnailAndProxyPreserveTimingAndIdentity() {
        auto asset = original("roundtrip"); QVERIFY(asset);
        QVERIFY(MediaPreviewStore::supportsScrubProxy(*asset));
        IndexedMediaDecoder decoder;
        QString error;
        const int index = asset->frameIndex.size()/2;
        const auto thumbnail = decoder.thumbnail(*asset, index, error);
        QVERIFY2(!thumbnail.isNull(), qPrintable(error));
        QVERIFY(thumbnail.width() <= MediaThumbnails::Width);
        QVERIFY(thumbnail.height() <= MediaThumbnails::Height);
        MediaPreviewStore::storeThumbnail(*asset, index, thumbnail);
        QCOMPARE(MediaPreviewStore::thumbnail(*asset, index).size(), thumbnail.size());
        const auto image = decoder.previewImage(*asset, index, QSize(960, 960), error);
        QVERIFY2(!image.isNull(), qPrintable(error));
        QVERIFY(MediaPreviewStore::storeScrubFrame(*asset, index, image));
        const auto frame = MediaPreviewStore::scrubFrame(*asset, index);
        QVERIFY(frame.isValid());
        QCOMPARE(frame.startTime(), asset->frameIndex[index].timestampUs);
        QCOMPARE(frame.endTime(), asset->frameIndex[index].timestampUs + asset->frameIndex[index].durationUs);
        QCOMPARE(frame.size(), image.size());
        QVERIFY(mediaFrameAllocationBytes(frame) > 0);
        asset->rotation = (asset->rotation + 90) % 360;
        QVERIFY(MediaPreviewStore::thumbnail(*asset, index).isNull());
        QVERIFY(!MediaPreviewStore::scrubFrame(*asset, index).isValid());
    }
    void damagedDerivativeIsACacheMiss() {
        auto asset = original("damaged"); QVERIFY(asset);
        IndexedMediaDecoder decoder;
        QString error;
        const auto image = decoder.previewImage(*asset, 0, QSize(960, 960), error);
        QVERIFY(MediaPreviewStore::storeScrubFrame(*asset, 0, image));
        // Only this process's disposable cache is used by this test executable.
        QDir cache(cacheDirectory + QStringLiteral("/media-previews-v1/scrub"));
        for (const QString& name : cache.entryList({QStringLiteral("*.jpg")}, QDir::Files)) {
            QFile file(cache.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("incomplete derivative");
        }
        QVERIFY(!MediaPreviewStore::scrubFrame(*asset, 0).isValid());
        QVERIFY(decoder.videoFrame(*asset, 0, error).isValid());
    }
    void generationPausesAndReleasesItsSource() {
        auto asset = original("background"); QVERIFY(asset);
        auto& cache = EditingProxyCache::instance();
        QObject owner;
        cache.setEnabled(false);
        cache.acquire(&owner, asset);
        QCOMPARE(cache.pendingAssets(), 1);
        QTest::qWait(300);
        QVERIFY(!MediaPreviewStore::hasScrubFrame(*asset, asset->frameIndex.size()-1));
        cache.setInteractive(&owner, true);
        cache.setEnabled(true);
        QTest::qWait(300);
        QVERIFY(!MediaPreviewStore::hasScrubFrame(*asset, asset->frameIndex.size()-1));
        cache.setInteractive(&owner, false);
        QTRY_VERIFY_WITH_TIMEOUT(MediaPreviewStore::hasScrubFrame(*asset, asset->frameIndex.size()-1), 10000);
        QTRY_COMPARE(cache.pendingAssets(), 0);
        // A completed scan is not permanent availability: disk cleanup can
        // evict earlier frames. Revisiting a cursor must regenerate its window.
        QDir disk(cacheDirectory + QStringLiteral("/media-previews-v1/scrub"));
        for (const auto& name : disk.entryList({QStringLiteral("*.jpg")}, QDir::Files))
            QVERIFY(disk.remove(name));
        QVERIFY(!MediaPreviewStore::hasScrubFrame(*asset, asset->frameIndex.size()-1));
        cache.prioritize(&owner, asset->frameIndex.size()-1);
        QTRY_VERIFY_WITH_TIMEOUT(MediaPreviewStore::hasScrubFrame(*asset, asset->frameIndex.size()-1), 10000);
        QTRY_COMPARE(cache.pendingAssets(), 0);
        cache.release(&owner);
        std::weak_ptr<const ResidentMediaAsset> weak = asset;
        asset.reset();
        QTRY_VERIFY_WITH_TIMEOUT(weak.expired(), 3000);
    }
    void cleanupTestCase() {
        EditingProxyCache::instance().setEnabled(false);
        QDir(cacheDirectory).removeRecursively();
    }
};
QTEST_GUILESS_MAIN(MediaEditingCacheTest)
#include "tst_MediaEditingCache.moc"
