#include <QtTest>
#include <QImage>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QScopeGuard>
#include "backend/media/MediaResidencyManager.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/media/DecodeScheduler.h"
#include "backend/media/MediaDecoder.h"

extern "C" {
#include <libavformat/avformat.h>
}

class MediaResidencyManagerTest : public QObject {
    Q_OBJECT
    static constexpr quint64 MiB = 1024ULL * 1024;
    static constexpr quint64 GiB = 1024ULL * 1024 * 1024;
    static MediaResidencyManager::MemorySnapshot memory(quint64 available = 4 * GiB) {
        return {8 * GiB, available, 128 * 1024 * 1024, false, 0};
    }
    static QString image(const QTemporaryDir& dir, const QString& name, int size, QRgb color) {
        QImage value(size, size, QImage::Format_ARGB32);
        value.fill(color);
        const auto path = dir.filePath(name);
        return value.save(path) ? path : QString();
    }
    static bool corruptLastVideoPacket(const QString& path) {
        AVFormatContext* format = nullptr;
        const auto name = QFile::encodeName(path);
        if (avformat_open_input(&format, name.constData(), nullptr, nullptr) < 0) return false;
        auto close = qScopeGuard([&] { avformat_close_input(&format); });
        if (avformat_find_stream_info(format, nullptr) < 0) return false;
        const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        AVPacket* packet = av_packet_alloc();
        if (!packet) return false;
        auto free = qScopeGuard([&] { av_packet_free(&packet); });
        qint64 offset = -1;
        int length = 0;
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == video) { offset = packet->pos; length = packet->size; }
            av_packet_unref(packet);
        }
        avformat_close_input(&format);
        QFile file(path);
        return offset >= 0 && length > 0 && file.open(QIODevice::ReadWrite)
            && file.seek(offset) && file.write(QByteArray(length, '\0')) == length;
    }
private slots:
    void firstPreviewPrecedesCompleteValidation() {
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.retainOriginalVideo = true;
        bool cancelled = false;
        bool completed = false;
        std::shared_ptr<const ResidentMediaPreview> first;
        ResidentMediaMemory allocation;
        callbacks.cancelled = [&] { return cancelled; };
        callbacks.allocatedBreakdown = [&](const auto& memory) { allocation = memory; };
        callbacks.progress = [&](double progress) { completed = progress >= 1; };
        callbacks.preview = [&](auto snapshot) {
            first = std::move(snapshot);
            cancelled = true; // stop at the first image, before any EOF/ready
        };
        QString error;
        QVERIFY(!MediaDecoder::decode(QString::fromUtf8(TEST_VIDEO_FILE), callbacks, &error));
        QCOMPARE(error, QStringLiteral("cancelled"));
        QVERIFY(!completed);
        QVERIFY(first);
        QCOMPARE(first->thumbnails.size(), 1);
        QVERIFY(!first->sha256.isEmpty());
        QVERIFY(first->poster.isValid());
        QCOMPARE(first->poster.size(), first->displaySize);
        QVERIFY(!first->thumbnails.first().image.isNull());
        QCOMPARE(allocation.posterBytes + allocation.thumbnailBytes, first->residentBytes);
    }
    void previewsAreImmutableBoundedAndShareResidentPixels() {
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.retainOriginalVideo = true;
        std::shared_ptr<const ResidentMediaPreview> first, latest;
        callbacks.preview = [&](auto snapshot) {
            if (!first) first = snapshot;
            if (latest) QVERIFY(snapshot->thumbnails.size() > latest->thumbnails.size());
            latest = std::move(snapshot);
        };
        QString error;
        const auto asset = MediaDecoder::decode(QString::fromUtf8(TEST_VIDEO_FILE), callbacks, &error);
        QVERIFY2(asset, qPrintable(error));
        QVERIFY(first && latest);
        QCOMPARE(first->thumbnails.size(), 1);
        QVERIFY(latest->thumbnails.size() > 1);
        QVERIFY(latest->thumbnails.size() <= MediaThumbnails::PreviewCount);
        QCOMPARE(latest->thumbnails.size(), asset->thumbnails.size());
        QCOMPARE(first->sha256, asset->sha256);
        QVERIFY(first->poster.isValid());
        QCOMPARE(first->poster.videoBuffer(), asset->firstFrame.frame.videoBuffer());
        QCOMPARE(latest->poster.videoBuffer(), first->poster.videoBuffer());
        quint64 pixels = 0;
        qint64 previous = -1;
        for (int i = 0; i < asset->thumbnails.size(); ++i) {
            const auto& thumb = latest->thumbnails.at(i);
            QVERIFY(thumb.timestampUs > previous);
            previous = thumb.timestampUs;
            QVERIFY(thumb.image.width() <= MediaThumbnails::Width);
            QVERIFY(thumb.image.height() <= MediaThumbnails::Height);
            QCOMPARE(thumb.image.cacheKey(), asset->thumbnails.at(i).image.cacheKey());
            pixels += thumb.image.sizeInBytes();
        }
        QCOMPARE(asset->thumbnailBytes, pixels + quint64(asset->thumbnails.capacity()) * sizeof(ResidentThumbnail));
        QVERIFY(pixels <= quint64(MediaThumbnails::PreviewCount * MediaThumbnails::Width * MediaThumbnails::Height * 4));
    }
    void previewDoesNotAuthorizeReadyPlaybackOrMismatchedOwners() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        bool observed = false;
        connect(&manager, &MediaResidencyManager::ownerChanged, &manager, [&](const QString& owner) {
            if (owner != QLatin1String("video") || manager.ready(owner)) return;
            const auto preview = manager.preview(owner);
            if (!preview) return;
            observed = true;
            QCOMPARE(manager.state(owner), QStringLiteral("decoding"));
            QVERIFY(!manager.asset(owner));
            QVERIFY(!manager.pinOwners({owner}, "not-ready"));
            QVERIFY(!manager.preview("wrong"));
            QVERIFY(preview->poster.isValid());
            const auto usage = manager.summary();
            QVERIFY(usage.value("thumbnailBytes").toULongLong() + usage.value("posterBytes").toULongLong()
                >= preview->residentBytes);
        });
        const auto path = QString::fromUtf8(TEST_VIDEO_FILE);
        manager.acquire("video", path);
        manager.acquire("wrong", path, QString(64, '0'));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        QVERIFY(observed);
        QVERIFY(!manager.ready("wrong"));
        QVERIFY(!manager.preview("wrong"));
        const auto preview = manager.preview("video");
        QVERIFY(preview);
        const auto asset = manager.asset("video");
        QCOMPARE(preview->thumbnails.first().image.cacheKey(), asset->thumbnails.first().image.cacheKey());
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), asset->residentBytes);
    }
    void corruptTailClearsPublishedPreviewWithoutBecomingReady() {
        QTemporaryDir directory;
        const auto path = directory.filePath("corrupt-tail.mp4");
        QVERIFY(QFile::copy(QString::fromUtf8(TEST_VIDEO_FILE), path));
        QVERIFY(corruptLastVideoPacket(path));
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.retainOriginalVideo = true;
        bool decodedPreview = false;
        callbacks.preview = [&](auto preview) { decodedPreview |= !preview->thumbnails.isEmpty(); };
        QString error;
        QVERIFY(!MediaDecoder::decode(path, callbacks, &error));
        QVERIFY(decodedPreview);
        QVERIFY(!error.isEmpty());
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        bool announcedReady = false;
        connect(&manager, &MediaResidencyManager::ownerChanged, &manager,
            [&](const auto& id) { announcedReady |= manager.ready(id); });
        manager.acquire("video", path);
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("video"), QStringLiteral("error"), 10000);
        QVERIFY(!announcedReady);
        QVERIFY(!manager.preview("video"));
        QVERIFY(!manager.asset("video"));
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), quint64(0));
    }
    void sourceChangeAfterPreviewRejectsImport() {
        QTemporaryDir directory;
        const auto path = directory.filePath("changed.mp4");
        QVERIFY(QFile::copy(QString::fromUtf8(TEST_VIDEO_FILE), path));
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        bool changed = false;
        connect(&manager, &MediaResidencyManager::ownerChanged, &manager, [&](const QString& owner) {
            if (changed || !manager.preview(owner) || manager.ready(owner)) return;
            changed = true;
            QFile file(path);
            QVERIFY(file.open(QIODevice::Append));
            QCOMPARE(file.write("changed"), qint64(7));
            file.close();
            QVERIFY(!manager.preview(owner));
        });
        manager.acquire("video", path);
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("video"), QStringLiteral("error"), 10000);
        QVERIFY(changed);
        QVERIFY(!manager.preview("video"));
        QVERIFY(!manager.asset("video"));
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), quint64(0));
    }
    void cancelledPreviewCannotReplaceReboundOwner() {
        QTemporaryDir directory;
        const auto replacement = image(directory, "replacement.png", 8, qRgb(17, 29, 41));
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        bool rebound = false;
        std::weak_ptr<const ResidentMediaPreview> retired;
        connect(&manager, &MediaResidencyManager::ownerChanged, &manager, [&](const QString& owner) {
            if (rebound || !manager.preview(owner) || manager.ready(owner)) return;
            rebound = true;
            retired = manager.preview(owner);
            manager.acquire(owner, replacement);
            QVERIFY(!manager.preview(owner));
        });
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(rebound && manager.ready("video"), 10000);
        const auto asset = manager.asset("video");
        QVERIFY(asset && !asset->video);
        QCOMPARE(asset->image.pixelColor(0, 0), QColor(17, 29, 41));
        QTRY_VERIFY(retired.expired());
        manager.release("video");
        QVERIFY(!manager.preview("video"));
        QTRY_COMPARE(manager.summary().value("mediaBytes").toULongLong(), quint64(0));
    }
    void pressureCancellationReleasesPreviewPixels() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        bool cancelled = false;
        std::weak_ptr<const ResidentMediaPreview> retired;
        connect(&manager, &MediaResidencyManager::ownerChanged, &manager, [&](const QString& owner) {
            if (cancelled || !manager.preview(owner) || manager.ready(owner)) return;
            cancelled = true;
            retired = manager.preview(owner);
            auto critical = memory(); critical.pressure = 2;
            manager.setMemorySnapshotForTesting(critical);
            QVERIFY(!manager.preview(owner));
        });
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("video"), QStringLiteral("waiting_for_memory"), 10000);
        QVERIFY(cancelled);
        QVERIFY(!manager.ready("video"));
        QVERIFY(!manager.preview("video"));
        QTRY_VERIFY(retired.expired());
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), quint64(0));
    }
    void originalImportDoesNotReserveTranscodedCopies() {
        QTemporaryDir directory;
        const auto path = directory.filePath("large-original.mp4");
        QVERIFY(QFile::copy(QString::fromUtf8(TEST_VIDEO_FILE), path));
        QFile source(path);
        QVERIFY(source.open(QIODevice::Append));
        // A valid MP4 free box makes source storage dominate the tiny fixture's
        // frames without introducing a long-running decode into this test.
        QCOMPARE(source.write(QByteArray::fromHex("0100000066726565")), qint64(8));
        const QByteArray padding(16 * MiB - 8, '\0');
        QCOMPARE(source.write(padding), qint64(padding.size()));
        source.close();
        const auto probe = MediaDecoder::probe(path);
        QVERIFY2(probe.accepted(), qPrintable(probe.error));
        MediaResidencyManager manager;
        manager.setSafetyReserve(0, 0);
        manager.setMemorySnapshotForTesting(memory(probe.scratchBytes + QFileInfo(path).size() + 4 * MiB));
        manager.acquire("source", path);
        QTRY_VERIFY2_WITH_TIMEOUT(manager.ready("source"), qPrintable(manager.errorString("source")), 10000);
        QVERIFY(!manager.asset("source")->allIntra);
        QCOMPARE(manager.asset("source")->compressedVideo.size(), QFileInfo(path).size());
    }
    void suppliedSourceFinishesPreparation() {
        const auto path = qEnvironmentVariable("MOUFFETTE_IMPORT_TEST_FILE");
        if (path.isEmpty()) QSKIP("Set MOUFFETTE_IMPORT_TEST_FILE to qualify an external source");
        MediaResidencyManager manager;
        QElapsedTimer elapsed;
        elapsed.start();
        QString lastState;
        int lastProgress = -1;
        qint64 firstPreviewMs = -1;
        connect(&manager, &MediaResidencyManager::ownerChanged, &manager, [&](const QString& owner) {
            if (owner != QLatin1String("source")) return;
            if (firstPreviewMs < 0 && manager.preview(owner)) {
                firstPreviewMs = elapsed.elapsed();
                qInfo() << firstPreviewMs << "first-preview" << manager.preview(owner)->thumbnails.size()
                        << "samples" << "ready:" << manager.ready(owner);
            }
            const auto state = manager.state(owner);
            const int progress = int(manager.progress(owner) * 10);
            if (state != lastState || progress != lastProgress) {
                qInfo() << elapsed.elapsed() << state << manager.progress(owner) << manager.errorString(owner);
                lastState = state;
                lastProgress = progress;
            }
        });
        manager.acquire("source", path);
        QTRY_VERIFY2_WITH_TIMEOUT(manager.ready("source"), qPrintable(manager.state("source") + ": " + manager.errorString("source")), 180000);
        QVERIFY(firstPreviewMs >= 0);
        const auto asset = manager.asset("source");
        QVERIFY(asset && asset->video);
        ResidentVideoPlayer player;
        player.setAsset(asset);
        for (qint64 position : {qint64(0), qint64(500), qint64(1000), asset->durationUs / 1000 - 1}) {
            player.prepare(position);
            QTRY_VERIFY2_WITH_TIMEOUT(player.preparedAt(position), qPrintable(player.errorString()), 5000);
        }
    }
    void unavailablePlayerFailsBeforeResidency() {
        if (qEnvironmentVariable("QT_MEDIA_BACKEND") != QLatin1String("unavailable"))
            QSKIP("Run in the isolated unavailable-backend CTest process");
        // Qt falls back to a discovered backend for an unknown backend name.
        // Hide multimedia plugins in this isolated process to exercise early
        // rejection on a computer with no usable playback backend.
        QTemporaryDir plugins;
        QVERIFY(plugins.isValid());
        QCoreApplication::setLibraryPaths({plugins.path()});
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        QSignalSpy errors(&manager, &MediaResidencyManager::errorOccurred);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        QVERIFY(!manager.asset("video")->allIntra);
        QVERIFY(errors.isEmpty());
        QTemporaryDir dir;
        manager.acquire("image", image(dir, "valid.png", 32, qRgb(1, 2, 3)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("image"), 3000);
    }
    void nativeFailureDoesNotPublishReadyAndCanBeRetried() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        bool injected = false;
        bool announcedReady = false;
        connect(&manager, &MediaResidencyManager::ownerChanged, &manager, [&](const QString& owner) {
            if (owner == QLatin1String("video") && manager.ready(owner)) announcedReady = true;
        });
        connect(&manager, &MediaResidencyManager::changed, &manager, [&] {
            if (injected || manager.summary().value("playbackBudgetBytes").toULongLong() == 0) return;
            injected = true;
            const QPointer<ResidentVideoPlayer> player = manager.findChild<ResidentVideoPlayer*>();
            QVERIFY(player);
            // Inject a preparation error at admission, before readiness is
            // published. The decoder's corrupt-input path is tested separately.
            player->errorOccurred(QMediaPlayer::FormatError, QStringLiteral("Injected preparation failure"));
        });
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("video"), QStringLiteral("error"), 10000);
        QVERIFY(injected);
        QVERIFY(!announcedReady);
        QVERIFY(!manager.asset("video"));
        QVERIFY(!manager.errorString("video").isEmpty());
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        manager.retry("video");
        QTRY_VERIFY2_WITH_TIMEOUT(manager.ready("video"), qPrintable(manager.errorString("video")), 10000);
        QVERIFY(announcedReady);
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
    }
    void cancellingNativeValidationKeepsTheQueueUsable() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        bool cancelled = false;
        connect(&manager, &MediaResidencyManager::changed, &manager, [&] {
            if (cancelled || manager.summary().value("playbackBudgetBytes").toULongLong() == 0) return;
            cancelled = true;
            QVERIFY(!manager.ready("video"));
            manager.release("video");
        });
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(cancelled, 10000);
        QVERIFY(!manager.ready("video"));
        QTRY_VERIFY(manager.assets().isEmpty());
        manager.acquire("replacement", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY2_WITH_TIMEOUT(manager.ready("replacement"), qPrintable(manager.errorString("replacement")), 10000);
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
    }
    void smallConfiguredReserveAllowsFourGiBAvailable_data() {
        QTest::addColumn<int>("percent");
        QTest::addColumn<quint64>("total");
        QTest::addColumn<quint64>("reserve");
        QTest::newRow("fixed-minimum") << 0 << 8 * GiB << 548 * MiB;
        QTest::newRow("five-percent-minimum-wins") << 5 << 8 * GiB << 548 * MiB;
        QTest::newRow("five-percent-total-ram-wins") << 5 << 32 * GiB << 32 * GiB / 20;
    }
    void smallConfiguredReserveAllowsFourGiBAvailable() {
        QFETCH(int, percent);
        QFETCH(quint64, total);
        QFETCH(quint64, reserve);
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting({total, 4 * GiB, 128 * MiB, false, 0});
        manager.setSafetyReserve(percent, 548);
        QCOMPARE(manager.summary().value("reserveBytes").toULongLong(), reserve);
        manager.acquire("image", image(dir, "image.png", 128, qRgb(1, 2, 3)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("image"), 5000);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        QVERIFY(manager.pinOwners({"video"}, "scene"));
        manager.unpinGroup("scene");
    }
    void configuredReserveControlsAdmission() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory(7 * GiB));
        manager.setSafetyReserve(50, 1024);
        QCOMPARE(manager.summary().value("reserveBytes").toULongLong(), 4 * GiB);
        manager.setSafetyReserve(25, 8192);
        QCOMPARE(manager.summary().value("reserveBytes").toULongLong(), 8 * GiB);
        manager.acquire("blocked", image(dir, "blocked.png", 8, qRgb(1, 2, 3)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("blocked"), QStringLiteral("capacity_insufficient"), 5000);
        manager.setSafetyReserve(0, 0);
        QCOMPARE(manager.summary().value("reserveBytes").toULongLong(), quint64(0));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("blocked"), 5000);
        manager.acquire("fits", image(dir, "fits.png", 8, qRgb(4, 5, 6)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("fits"), 5000);
    }
    void asynchronousCompleteAndDeduplicated() {
        QTemporaryDir dir;
        const auto a = image(dir, "a.png", 32, qRgb(10, 20, 30));
        const auto b = dir.filePath("b.png");
        QVERIFY(QFile::copy(a, b));
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.acquire("first", a);
        manager.acquire("second", b);
        QVERIFY(!manager.ready("first"));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("first") && manager.ready("second"), 10000);
        QCOMPARE(manager.asset("first"), manager.asset("second"));
        QCOMPARE(manager.sha256("first").size(), 64);
        QCOMPARE(manager.assets().size(), 1);
        QCOMPARE(manager.assets().first().toMap().value("occurrences").toInt(), 2);
        QCOMPARE(manager.asset("first")->image.size(), QSize(32, 32));
        manager.release("first");
        QVERIFY(manager.ready("second"));
        manager.release("second");
        QVERIFY(manager.assets().isEmpty());
    }
    void memoryCategoriesSumWithoutDoubleCountingAndClearOnEviction() {
        QTemporaryDir directory;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        const auto path = image(directory, "large.png", 512, qRgb(10, 20, 30));
        manager.acquire("image", path);
        manager.acquire("duplicate", path);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("image") && manager.ready("duplicate") && manager.ready("video"), 10000);
        QCOMPARE(manager.assets().size(), 2);
        const auto picture = manager.asset("image");
        const auto video = manager.asset("video");
        const quint64 imagePixels = picture->image.sizeInBytes();
        const quint64 imageThumbnails = picture->thumbnails.first().image.sizeInBytes() + sizeof(ResidentThumbnail);
        const auto summary = manager.summary();
        QCOMPARE(summary.value("imageBytes").toULongLong(), imagePixels);
        QCOMPARE(summary.value("thumbnailBytes").toULongLong(), imageThumbnails + video->thumbnailBytes);
        QCOMPARE(summary.value("videoBytes").toULongLong(), video->memoryBreakdown().videoBytes);
        QCOMPARE(summary.value("posterBytes").toULongLong(), video->posterBytes);
        quint64 total = 0;
        for (const auto* category : {"videoBytes", "imageBytes", "posterBytes", "thumbnailBytes", "audioPreviewBytes"}) {
            quint64 rows = 0;
            for (const auto& value : manager.assets()) rows += value.toMap().value(category).toULongLong();
            QCOMPARE(rows, summary.value(category).toULongLong());
            total += rows;
        }
        QCOMPARE(summary.value("mediaBytes").toULongLong(), total);
        manager.release("duplicate");
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), total);
        { auto critical = memory(0); critical.pressure = 2; manager.setMemorySnapshotForTesting(critical); }
        QTRY_VERIFY(!manager.ready("image") && !manager.ready("video"));
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), quint64(0));
        for (const auto& value : manager.assets()) {
            for (const auto* category : {"videoBytes", "imageBytes", "posterBytes", "thumbnailBytes", "audioPreviewBytes"})
                QCOMPARE(value.toMap().value(category).toULongLong(), quint64(0));
        }
    }
    void compressedVideoAccountingAndAtomicPlaybackAdmission() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(20, 2048);
        bool sawPreparation = false;
        connect(&manager, &MediaResidencyManager::changed, &manager, [&] {
            for (const auto& value : manager.assets()) {
                const auto row = value.toMap();
                if (row.value("state").toString() != QLatin1String("decoding")) continue;
                sawPreparation = true;
                // Actual retained bytes must not include the decoder scratch.
                QVERIFY(row.value("residentBytes").toULongLong()
                    <= row.value("estimatedBytes").toULongLong() + 65536);
                QCOMPARE(row.value("residentBytes").toULongLong(),
                    row.value("videoBytes").toULongLong() + row.value("imageBytes").toULongLong()
                    + row.value("posterBytes").toULongLong() + row.value("thumbnailBytes").toULongLong()
                    + row.value("audioPreviewBytes").toULongLong());
            }
        });
        manager.acquire("first", QString::fromUtf8(TEST_VIDEO_FILE));
        manager.acquire("second", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("first") && manager.ready("second"), 10000);
        QVERIFY(sawPreparation);
        const auto asset = manager.asset("first");
        QCOMPARE(asset, manager.asset("second"));
        QVERIFY(!asset->compressedVideo.isEmpty());
        QVERIFY(!asset->allIntra);
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), asset->residentBytes);
        QCOMPARE(manager.summary().value("videoBytes").toULongLong(), asset->memoryBreakdown().videoBytes);
        QCOMPARE(manager.summary().value("imageBytes").toULongLong(), quint64(0));
        QCOMPARE(manager.summary().value("thumbnailBytes").toULongLong(), asset->thumbnailBytes);
        QCOMPARE(manager.summary().value("posterBytes").toULongLong(), asset->posterBytes);
        QCOMPARE(manager.summary().value("reservedBytes").toULongLong(), quint64(0));
        QVERIFY(manager.pinOwners({"first"}, "scene"));
        const quint64 oneDecoderPool = manager.summary().value("playbackBudgetBytes").toULongLong();
        QStringList ten; for (int i = 0; i < 10; ++i) ten.append("first");
        QVERIFY(manager.pinOwners(ten, "scene"));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), oneDecoderPool);
        const quint64 reservation = manager.summary().value("pendingPlaybackBudgetBytes").toULongLong();
        QVERIFY(reservation > 0);
        manager.unpinGroup("scene");
        const quint64 reserve = manager.summary().value("reserveBytes").toULongLong();
        manager.setMemorySnapshotForTesting(memory(reserve + reservation - 1));
        QVERIFY(!manager.pinOwners(ten, "scene"));
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        QVERIFY(manager.ready("first"));
        manager.setMemorySnapshotForTesting(memory());
        QVERIFY(manager.pinOwners(ten, "scene"));
        for (int i = 0; i < 10; ++i) { QVERIFY(asset->reservePlayback()); asset->playbackPrepared(); }
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), oneDecoderPool);
        for (int i = 0; i < 10; ++i) asset->releasePlayback(true);
        manager.unpinGroup("scene");
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
    }
    void existingScenePinsCanShrinkBelowMemoryReserve() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(0, 512);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        QVERIFY(manager.pinOwners({"video", "video", "video"}, "scene"));
        const auto three = manager.summary().value("pendingPlaybackBudgetBytes").toULongLong();
        manager.setMemorySnapshotForTesting(memory(256 * MiB));
        QVERIFY(manager.ready("video"));

        QVERIFY(manager.pinOwners({"video", "video"}, "scene"));
        const auto two = manager.summary().value("pendingPlaybackBudgetBytes").toULongLong();
        QVERIFY(two < three);
        QVERIFY(manager.pinOwners({"video", "video"}, "scene"));
        QVERIFY(!manager.pinOwners({"video", "video", "video"}, "scene"));
        QVERIFY(!manager.pinOwners({"video"}, "new-scene"));
        QVERIFY(!manager.pinOwners({"missing"}, "scene"));
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), two);
        QVERIFY(manager.assets().first().toMap().value("protected").toBool());

        QVERIFY(manager.pinOwners({}, "scene"));
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        QVERIFY(!manager.assets().first().toMap().value("protected").toBool());
    }
    void preparedPlaybackDoesNotReserveAlreadyAllocatedMemory() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(0, 512);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        const auto asset = manager.asset("video");
        QVERIFY(manager.pinOwners({"video", "video"}, "pair"));
        const auto initial = manager.summary().value("pendingPlaybackBudgetBytes").toULongLong();
        QVERIFY(initial > 0);
        QVERIFY(asset->reservePlayback()); QVERIFY(asset->reservePlayback());
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), initial);
        asset->playbackPrepared();
        QVERIFY(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong() < initial);
        asset->playbackPrepared();
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        manager.setMemorySnapshotForTesting(memory(512 * MiB + 1));
        QVERIFY(manager.ready("video"));
        QVERIFY(!asset->reservePlayback());
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        manager.unpinGroup("pair");
        asset->releasePlayback(true); asset->releasePlayback(true);
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
    }


    void preparedPlaybackAllowsMediaImport() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(0, 548);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        const auto asset = manager.asset("video");
        QVERIFY(asset->reservePlayback());
        asset->playbackPrepared();
        // Enough for this image and its preparation buffers after the OS has
        // accounted for playback, but not another estimated playback budget.
        manager.setMemorySnapshotForTesting(memory(548 * MiB + 64 * MiB + MiB));
        manager.acquire("image", image(dir, "image.png", 32, qRgb(5, 6, 7)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("image"), 5000);
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        asset->releasePlayback(true);
    }
    void preAdmittedSceneSlotsSurvivePartialNativeAllocations() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(0, 512);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        const auto asset = manager.asset("video");
        QVERIFY(manager.pinOwners({"video", "video"}, "pair"));
        QVERIFY(asset->reservePlayback());
        // Existing pool commitments are not charged twice as worker allocations
        // appear in the OS measurement. Only the small cursor audio buffer grows.
        manager.setMemorySnapshotForTesting(memory(512 * MiB + MiB));
        QVERIFY(asset->reservePlayback());
        QVERIFY(!asset->reservePlayback());
        asset->playbackPrepared(); asset->playbackPrepared();
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        manager.setMemorySnapshotForTesting(memory(512 * MiB - 1));
        QVERIFY(!asset->reservePlayback());
        QVERIFY(manager.ready("video"));
        manager.unpinGroup("pair");
        asset->releasePlayback(true); asset->releasePlayback(true);
    }


    void survivingDuplicateReloadsItsOwnSource()
    {
        QTemporaryDir dir;
        const auto first = image(dir, "original.png", 16, qRgb(2, 4, 6));
        const auto second = dir.filePath("copy.png");
        QVERIFY(QFile::copy(first, second));
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(20, 2048);
        manager.acquire("a", first);
        manager.acquire("b", second);
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("a") && manager.ready("b"), 10000);
        manager.release("a");
        QVERIFY(QFile::remove(first));
        { auto critical = memory(2 * GiB - 1); critical.pressure = 2; manager.setMemorySnapshotForTesting(critical); }
        QVERIFY(!manager.ready("b"));
        manager.setMemorySnapshotForTesting(memory());
        for (int i = 0; i < 10; ++i) manager.retry("b");
        QVERIFY(!manager.ready("b")); // repeated calls cannot manufacture two healthy seconds
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("b"), 10000);
        QCOMPARE(manager.asset("b")->image.size(), QSize(16, 16));
    }
    void impossibleAssetStaysBlocked()
    {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting({2 * GiB, 2 * GiB, 0, false, 0});
        manager.setSafetyReserve(20, 2048);
        manager.acquire("too-large", image(dir, "image.png", 8, qRgb(2, 3, 4)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("too-large"), QStringLiteral("capacity_insufficient"), 10000);
        manager.retry("too-large");
        manager.sampleNow();
        QVERIFY(!manager.ready("too-large"));
        QVERIFY(!manager.pinOwners({"too-large"}, "scene"));
        QCOMPARE(manager.state("too-large"), QStringLiteral("capacity_insufficient"));
        // A text-only canvas creates no media allocations or decoder budgets.
        manager.setMemorySnapshotForTesting({2 * GiB, GiB, 0, false, 0});
        QVERIFY(manager.pinOwners({}, "text-only"));
        manager.unpinGroup("text-only");
    }
    void cancelledOwnerCannotReappear() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        const auto path = image(dir, "cancel.png", 64, qRgb(3, 4, 5));
        QSignalSpy idle(&manager, &MediaResidencyManager::backgroundWorkFinished);
        manager.acquire("gone", path);
        QVERIFY(manager.hasBackgroundWorkForPath(path));
        manager.release("gone");
        // A removed owner still has a cancellation job until its file closes.
        QVERIFY(manager.hasBackgroundWorkForPath(path));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.hasBackgroundWorkForPath(path), 10000);
        QTRY_COMPARE(idle.size(), 1);
        QVERIFY(QFile::remove(path));
        QVERIFY(!manager.ready("gone"));
        QVERIFY(manager.assets().isEmpty());
    }
    void hashMismatchNeverReady() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.acquire("wrong", image(dir, "wrong.png", 16, qRgb(4, 5, 6)), QString(64, '0'));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("wrong"), QStringLiteral("error"), 10000);
        QVERIFY(!manager.ready("wrong"));
        QVERIFY(!manager.asset("wrong"));
        QVERIFY(manager.errorString("wrong").contains("SHA-256"));
        QVERIFY(!manager.pinOwners({"wrong"}, "scene"));
    }
    void retryRestartsSharedErroredSource() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        const auto path = dir.filePath("unavailable.png");
        manager.acquire("first", path);
        manager.acquire("second", path);
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("first"), QStringLiteral("error"), 10000);
        QTRY_VERIFY(!manager.hasBackgroundWorkForPath(path));
        manager.retry("first");
        QCOMPARE(manager.state("first"), QStringLiteral("analysing"));
        QCOMPARE(manager.state("second"), QStringLiteral("analysing"));
        QCOMPARE(manager.assets().size(), 1);
        QVERIFY(manager.hasBackgroundWorkForPath(path));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("second"), QStringLiteral("error"), 10000);
    }
    void headroomShortagePreservesReadyAndCriticalPressureRecovers() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(20, 2048);
        manager.acquire("small", image(dir, "small.png", 8, qRgb(1, 2, 3)));
        manager.acquire("large", image(dir, "large.png", 32, qRgb(3, 2, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("small") && manager.ready("large"), 10000);
        auto shortage = memory(2 * GiB - 1024);
        manager.setMemorySnapshotForTesting(shortage);
        QVERIFY(manager.ready("small") && manager.ready("large"));
        QCOMPARE(manager.summary().value("loadableBytes").toULongLong(), quint64(0));
        shortage.pressure = 2; manager.setMemorySnapshotForTesting(shortage);
        QVERIFY(!manager.ready("small") && !manager.ready("large"));
        manager.setMemorySnapshotForTesting(memory(2 * GiB + 128 * MiB));
        manager.sampleNow();
        QVERIFY(!manager.ready("large"));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("small") && manager.ready("large"), 10000);
    }

    void atomicPinsAndControlledStop() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(20, 2048);
        manager.acquire("protected", image(dir, "protected.png", 16, qRgb(5, 6, 7)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("protected"), 10000);
        QVERIFY(!manager.pinOwners({"protected", "missing"}, "failed"));
        QVERIFY(manager.pinOwners({"protected"}, "scene"));
        QSignalSpy stop(&manager, &MediaResidencyManager::sceneStopRequested);
        { auto critical = memory(GiB); critical.pressure = 2; manager.setMemorySnapshotForTesting(critical); }
        QVERIFY(manager.ready("protected"));
        QTRY_COMPARE_WITH_TIMEOUT(stop.size(), 1, 4500);
        QCOMPARE(stop.first().first().toString(), QStringLiteral("scene"));
        QVERIFY(manager.ready("protected"));
        manager.unpinGroup("scene");
        manager.sampleNow();
        QVERIFY(!manager.ready("protected"));
    }
    void warningAdmitsImageWithScreenshotMemoryBudget_data() {
        QTest::addColumn<int>("reserveMiB");
        QTest::newRow("zero-reserve") << 0;
        QTest::newRow("default-reserve") << 548;
    }
    void warningAdmitsImageWithScreenshotMemoryBudget() {
        QFETCH(int, reserveMiB);
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting({16 * GiB, 3960 * MiB, 197 * MiB, true, 1, true});
        manager.setSafetyReserve(0, reserveMiB);
        // A small compressed JPEG still needs 46.5 MiB of pixels and about
        // 233 MiB including preparation. Both fit easily in the reported RAM.
        const auto path = image(dir, "screenshot.jpg", 3492, qRgb(31, 42, 53));
        QVERIFY(!path.isEmpty());
        QVERIFY(QFileInfo(path).size() < MiB);
        manager.acquire("image", path);
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("image"), 5000);
        QVERIFY(manager.asset("image")->residentBytes > 46 * MiB);
        QVERIFY(manager.asset("image")->residentBytes < 47 * MiB);
        QCOMPARE(manager.summary().value("pressure").toString(), QStringLiteral("warning"));
        QCOMPARE(manager.summary().value("reserveBytes").toULongLong(), quint64(reserveMiB) * MiB);
        QVERIFY(manager.errorString("image").isEmpty());
    }
    void warningStillEnforcesBudgetAndRecoversWithoutNormalPressure_data() {
        QTest::addColumn<int>("reserveMiB");
        QTest::newRow("zero-reserve") << 0;
        QTest::newRow("default-reserve") << 548;
    }
    void warningStillEnforcesBudgetAndRecoversWithoutNormalPressure() {
        QFETCH(int, reserveMiB);
        QTemporaryDir dir;
        MediaResidencyManager manager;
        const quint64 reserve = quint64(reserveMiB) * MiB;
        auto warning = memory(reserve + 32 * MiB);
        warning.pressure = 1;
        manager.setMemorySnapshotForTesting(warning);
        manager.setSafetyReserve(0, reserveMiB);
        manager.acquire("image", image(dir, "image.png", 128, qRgb(4, 5, 6)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("image"), QStringLiteral("waiting_for_memory"), 5000);
        QVERIFY(manager.errorString("image").contains("preparation needs"));
        QCOMPARE(manager.summary().value("loadableBytes").toULongLong(), 32 * MiB);
        const quint64 preparation = manager.assets().first().toMap().value("preparationBudgetBytes").toULongLong();
        QVERIFY(preparation > 32 * MiB);
        warning.availableBytes = reserve + preparation;
        manager.setMemorySnapshotForTesting(warning);
        for (int i = 0; i < 10; ++i) manager.retry("image");
        QVERIFY(!manager.ready("image")); // retries cannot bypass recovery hysteresis
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("image"), 5000);
        QCOMPARE(manager.summary().value("pressure").toString(), QStringLiteral("warning"));
    }
    void warningPlaybackAdmissionStillAccountsForPendingPlayers() {
        MediaResidencyManager manager;
        auto warning = memory(); warning.pressure = 1;
        manager.setMemorySnapshotForTesting(warning);
        manager.setSafetyReserve(0, 512);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        const auto asset = manager.asset("video");
        QVERIFY(manager.pinOwners({"video", "video"}, "pair"));
        const auto pending = manager.summary().value("pendingPlaybackBudgetBytes").toULongLong();
        manager.unpinGroup("pair");
        warning.availableBytes = 512 * MiB + pending;
        manager.setMemorySnapshotForTesting(warning);
        QVERIFY(!manager.pinOwners({"video", "video", "video"}, "too-many"));
        QVERIFY(manager.pinOwners({"video", "video"}, "pair"));
        QVERIFY(asset->reservePlayback()); QVERIFY(asset->reservePlayback());
        QVERIFY(!asset->reservePlayback());
        QCOMPARE(manager.summary().value("loadableBytes").toULongLong(), quint64(0));
        manager.unpinGroup("pair"); asset->releasePlayback(false); asset->releasePlayback(false);
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
    }


    void criticalPressureStillBlocksZeroReserveAndRecoversToWarning() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        auto snapshot = memory();
        snapshot.pressure = 2;
        manager.setMemorySnapshotForTesting(snapshot);
        manager.setSafetyReserve(0, 0);
        QCOMPARE(manager.summary().value("loadableBytes").toULongLong(), quint64(0));
        manager.acquire("image", image(dir, "image.png", 128, qRgb(4, 5, 6)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("image"), QStringLiteral("waiting_for_memory"), 5000);
        QVERIFY(manager.errorString("image").contains("critical"));
        QVERIFY(!manager.ready("image"));
        snapshot.pressure = 1;
        manager.setMemorySnapshotForTesting(snapshot);
        QCOMPARE(manager.summary().value("loadableBytes").toULongLong(), snapshot.availableBytes);
        for (int i = 0; i < 10; ++i) manager.retry("image");
        QVERIFY(!manager.ready("image"));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("image"), 5000);
        QCOMPARE(manager.summary().value("pressure").toString(), QStringLiteral("warning"));
    }
    void pressureWarningPreservesResidentMediaAndAdmitsNewImports() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(0, 548);
        manager.acquire("resident", image(dir, "resident.png", 32, qRgb(3, 4, 5)));
        manager.acquire("protected", image(dir, "protected.png", 16, qRgb(5, 4, 3)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("resident") && manager.ready("protected"), 5000);
        const auto resident = manager.asset("resident");
        const auto protectedAsset = manager.asset("protected");
        QVERIFY(manager.pinOwners({"protected"}, "scene"));
        QSignalSpy stop(&manager, &MediaResidencyManager::sceneStopRequested);
        auto warning = memory();
        warning.pressure = 1;
        manager.setMemorySnapshotForTesting(warning);
        const auto pendingPath = image(dir, "pending.png", 64, qRgb(6, 7, 8));
        manager.acquire("pending", pendingPath);
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("pending"), 5000);
        // A persistent warning with ample available RAM must not trigger the
        // two-second controlled stop or evict already prepared media.
        QTest::qWait(2200);
        manager.sampleNow();
        QCOMPARE(stop.size(), 0);
        QVERIFY(manager.ready("resident"));
        QVERIFY(manager.ready("protected"));
        QCOMPARE(manager.asset("resident"), resident);
        QCOMPARE(manager.asset("protected"), protectedAsset);
        QVERIFY(manager.ready("pending"));
        QCOMPARE(manager.summary().value("pressure").toString(), QStringLiteral("warning"));
        manager.unpinGroup("scene");
    }
    void criticalPressureEvictsAndRequestsControlledStop() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(0, 548);
        manager.acquire("resident", image(dir, "resident.png", 32, qRgb(3, 4, 5)));
        manager.acquire("protected", image(dir, "protected.png", 16, qRgb(5, 4, 3)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("resident") && manager.ready("protected"), 5000);
        QVERIFY(manager.pinOwners({"protected"}, "scene"));
        QSignalSpy stop(&manager, &MediaResidencyManager::sceneStopRequested);
        auto critical = memory();
        critical.pressure = 2;
        manager.setMemorySnapshotForTesting(critical);
        QVERIFY(!manager.ready("resident"));
        QVERIFY(manager.ready("protected"));
        QTRY_COMPARE_WITH_TIMEOUT(stop.size(), 1, 4500);
        QCOMPARE(stop.first().first().toString(), QStringLiteral("scene"));
        QVERIFY(manager.ready("protected"));
        manager.unpinGroup("scene");
        manager.sampleNow();
        QVERIFY(!manager.ready("protected"));
    }
    void waitingImportNeverEvictsReadyMediaToRetry() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(20, 2048);
        manager.acquire("resident", image(dir, "resident.png", 64, qRgb(20, 30, 40)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("resident"), 10000);
        const auto resident = manager.asset("resident");
        // Enough to preserve the system reserve, but not a decoder's scratch
        // allocation. A new import must wait without reclaiming ready data.
        manager.setMemorySnapshotForTesting(memory(2 * GiB + 32 * 1024 * 1024));
        manager.acquire("pending", image(dir, "pending.png", 128, qRgb(40, 30, 20)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("pending"), QStringLiteral("waiting_for_memory"), 10000);
        for (int i = 0; i < 5; ++i) manager.retry("pending");
        QVERIFY(manager.ready("resident"));
        QCOMPARE(manager.asset("resident"), resident);
        QVERIFY(!manager.ready("pending"));
        manager.setMemorySnapshotForTesting(memory());
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("pending"), 10000);
        QCOMPARE(manager.asset("resident"), resident);
    }
    void waitingImportRecoversWithOnlyItsPreparationBudget() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory(548 * MiB + 32 * MiB));
        manager.setSafetyReserve(0, 548);
        manager.acquire("pending", image(dir, "pending.png", 128, qRgb(7, 8, 9)));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("pending"), QStringLiteral("waiting_for_memory"), 5000);
        const quint64 preparation = manager.assets().first().toMap().value("preparationBudgetBytes").toULongLong();
        QVERIFY(preparation > 32 * MiB);
        QVERIFY(preparation < 128 * MiB);
        manager.setMemorySnapshotForTesting(memory(548 * MiB + preparation + MiB));
        for (int i = 0; i < 10; ++i) manager.retry("pending");
        QVERIFY(!manager.ready("pending"));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("pending"), 5000);
    }
    void errorsAndRemoteStates() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.acquire("missing", dir.filePath("missing.png"));
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("missing"), QStringLiteral("error"), 10000);
        QVERIFY(!manager.errorString("missing").isEmpty());
        manager.release("missing");
        manager.acquire("image", image(dir, "image.png", 8, qRgb(1, 1, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("image"), 10000);
        manager.setRemoteState(manager.sha256("image"), "target", "decoding", .5);
        QCOMPARE(manager.assets().first().toMap().value("remoteStates").toList().size(), 1);
        manager.clearRemoteStates("target");
        QVERIFY(manager.assets().first().toMap().value("remoteStates").toList().isEmpty());
        const auto summary = manager.summary();
        QCOMPARE(summary.value("processBytes").toULongLong(), memory().processBytes);
        QCOMPARE(summary.value("availableBytes").toULongLong(), memory().availableBytes);
        QVERIFY(!summary.contains("otherBytes"));
    }
};
QTEST_MAIN(MediaResidencyManagerTest)
#include "tst_MediaResidencyManager.moc"
