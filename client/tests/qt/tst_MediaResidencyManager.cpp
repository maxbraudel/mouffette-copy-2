#include <QtTest>
#include <QImage>
#include <QTemporaryDir>
#include <QSignalSpy>
#include "backend/media/MediaResidencyManager.h"
#include "backend/media/ResidentVideoPlayer.h"

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
private slots:
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
        QTRY_COMPARE_WITH_TIMEOUT(manager.state("video"), QStringLiteral("error"), 10000);
        QVERIFY(!manager.ready("video"));
        QVERIFY(!manager.asset("video"));
        QVERIFY(!manager.errorString("video").isEmpty());
        QVERIFY(!errors.isEmpty());
        // No stuck decoder reservation or stalled queue after native failure.
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
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
            // Software validation has succeeded. Make only the native player's
            // private source fail, leaving the verified shared asset intact.
            QTimer::singleShot(0, player, [player] {
                if (!player || !player->asset()) return;
                auto broken = std::make_shared<ResidentMediaAsset>(*player->asset());
                broken->compressedVideo = QByteArrayLiteral("invalid native MP4 stream");
                player->setAsset(broken);
            });
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
        QCOMPARE(summary.value("videoBytes").toULongLong(), quint64(video->compressedVideo.capacity()));
        QCOMPARE(summary.value("posterBytes").toULongLong(), video->posterBytes);
        quint64 total = 0;
        for (const auto* category : {"videoBytes", "imageBytes", "posterBytes", "thumbnailBytes"}) {
            quint64 rows = 0;
            for (const auto& value : manager.assets()) rows += value.toMap().value(category).toULongLong();
            QCOMPARE(rows, summary.value(category).toULongLong());
            total += rows;
        }
        QCOMPARE(summary.value("mediaBytes").toULongLong(), total);
        manager.release("duplicate");
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), total);
        manager.setMemorySnapshotForTesting(memory(0));
        QTRY_VERIFY(!manager.ready("image") && !manager.ready("video"));
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), quint64(0));
        for (const auto& value : manager.assets()) {
            for (const auto* category : {"videoBytes", "imageBytes", "posterBytes", "thumbnailBytes"})
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
                    + row.value("posterBytes").toULongLong() + row.value("thumbnailBytes").toULongLong());
            }
        });
        manager.acquire("first", QString::fromUtf8(TEST_VIDEO_FILE));
        manager.acquire("second", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("first") && manager.ready("second"), 10000);
        QVERIFY(sawPreparation);
        const auto asset = manager.asset("first");
        QCOMPARE(asset, manager.asset("second"));
        QVERIFY(!asset->compressedVideo.isEmpty());
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), asset->residentBytes);
        QCOMPARE(manager.summary().value("videoBytes").toULongLong(), quint64(asset->compressedVideo.capacity()));
        QCOMPARE(manager.summary().value("imageBytes").toULongLong(), quint64(0));
        QCOMPARE(manager.summary().value("thumbnailBytes").toULongLong(), asset->thumbnailBytes);
        QCOMPARE(manager.summary().value("posterBytes").toULongLong(), asset->posterBytes);
        QCOMPARE(manager.summary().value("reservedBytes").toULongLong(), quint64(0));
        const quint64 budget = asset->playbackBudgetBytes;
        manager.setMemorySnapshotForTesting(memory(2 * GiB + budget + 1));
        QVERIFY(!manager.pinOwners({"first", "second"}, "too-many-players"));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
        QVERIFY(!manager.pinOwners({"first", "first"}, "duplicate-remote-occurrences"));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
        QVERIFY(manager.pinOwners({"first"}, "one-player"));
        // A rejected replacement must preserve the original scene reservation.
        QVERIFY(!manager.pinOwners({"first", "first"}, "one-player"));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), budget);
        QVERIFY(asset->reservePlayback()); // consumes the pre-admitted slot
        QVERIFY(!asset->reservePlayback());
        QCOMPARE(manager.summary().value("mediaBytes").toULongLong(), asset->residentBytes);
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), budget);
        asset->releasePlayback(false);
        manager.unpinGroup("one-player");
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
        manager.setMemorySnapshotForTesting(memory());
        QVERIFY(manager.pinOwners({"first", "first"}, "duplicate-remote-occurrences"));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), 2 * budget);
        manager.release("first");
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
    }
    void preparedPlaybackDoesNotReserveAlreadyAllocatedMemory() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(0, 548);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        const auto asset = manager.asset("video");
        const quint64 budget = asset->playbackBudgetBytes;
        QVERIFY(budget > 0);
        manager.setMemorySnapshotForTesting(memory(548 * MiB + 2 * budget + 1));
        QVERIFY(manager.pinOwners({"video", "video"}, "pair"));
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), 2 * budget);
        QCOMPARE(manager.assets().first().toMap().value("playbackBudgetBytes").toULongLong(), 2 * budget);
        QCOMPARE(manager.assets().first().toMap().value("pendingPlaybackBudgetBytes").toULongLong(), 2 * budget);
        QVERIFY(asset->reservePlayback());
        QVERIFY(asset->reservePlayback());
        QVERIFY(!asset->reservePlayback());

        asset->playbackPrepared();
        // Available RAM now reflects the first player's real allocations.
        manager.setMemorySnapshotForTesting(memory(548 * MiB + budget + 1));
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), budget);
        QCOMPARE(manager.assets().first().toMap().value("pendingPlaybackBudgetBytes").toULongLong(), budget);
        QVERIFY(!asset->reservePlayback()); // The second player is still pending.
        asset->playbackPrepared();
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), 2 * budget);

        // Prepared players are already reflected in available RAM. The new
        // scene reserves only its additional player, atomically.
        QVERIFY(manager.pinOwners({"video"}, "third"));
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), budget);
        QVERIFY(asset->reservePlayback());
        QVERIFY(!asset->reservePlayback());
        asset->releasePlayback(false);
        // The scene still owns its slot even though its player failed early.
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), budget);
        manager.unpinGroup("third");
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        manager.unpinGroup("pair");

        QVERIFY(asset->reservePlayback()); // An unpinned player uses the same accounting.
        QVERIFY(!asset->reservePlayback());
        asset->releasePlayback(false);
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        asset->releasePlayback(true);
        asset->releasePlayback(true);
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
        manager.setSafetyReserve(0, 548);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        const auto asset = manager.asset("video");
        const quint64 budget = asset->playbackBudgetBytes;
        const quint64 reserve = 548 * MiB;
        QVERIFY(budget > 0);
        manager.setMemorySnapshotForTesting(memory(reserve + 2 * budget));
        QVERIFY(manager.pinOwners({"video", "video"}, "pair"));
        QVERIFY(asset->reservePlayback());

        // The first decoder has allocated some buffers, but has not produced
        // its first frame yet. Its existing commitment must not be charged
        // again when the second player consumes its already admitted slot.
        manager.setMemorySnapshotForTesting(memory(reserve + budget + budget / 2));
        QVERIFY(asset->reservePlayback());
        QVERIFY(!asset->reservePlayback()); // a third player is a new commitment
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), 2 * budget);
        asset->playbackPrepared();
        asset->playbackPrepared();
        manager.unpinGroup("pair");
        asset->releasePlayback(true);
        asset->releasePlayback(true);
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));

        // A pre-admitted slot still needs its own full future buffer budget
        // above the configured system reserve when its decoder starts.
        manager.setMemorySnapshotForTesting(memory(reserve + 2 * budget));
        QVERIFY(manager.pinOwners({"video", "video"}, "pair"));
        QVERIFY(asset->reservePlayback());
        manager.setMemorySnapshotForTesting(memory(reserve + budget - 1));
        QVERIFY(!asset->reservePlayback());
        manager.setMemorySnapshotForTesting(memory(reserve - 1));
        QVERIFY(!asset->reservePlayback());
        manager.setMemorySnapshotForTesting(memory());
        manager.unpinGroup("pair");
        asset->releasePlayback(false);
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), quint64(0));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
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
        manager.setMemorySnapshotForTesting(memory(2 * GiB - 1));
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
    void evictsLargestAndWaitsForHealthySamples() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        manager.setSafetyReserve(20, 2048);
        manager.acquire("small", image(dir, "small.png", 8, qRgb(1, 2, 3)));
        manager.acquire("large", image(dir, "large.png", 32, qRgb(3, 2, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("small") && manager.ready("large"), 10000);
        manager.setMemorySnapshotForTesting(memory(2 * GiB - 1024));
        QVERIFY(manager.ready("small"));
        QVERIFY(!manager.ready("large"));
        QCOMPARE(manager.state("large"), QStringLiteral("waiting_for_memory"));
        manager.setMemorySnapshotForTesting(memory(2 * GiB + 128 * 1024 * 1024));
        manager.sampleNow();
        QVERIFY(!manager.ready("large"));
        QVERIFY(manager.ready("small"));
        // Time-based hysteresis must recover here without an extra hidden
        // 512 MiB (or percentage) added to the configured safety reserve.
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("large"), 10000);
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
        manager.setMemorySnapshotForTesting(memory(GiB));
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
        auto warning = memory();
        warning.pressure = 1;
        manager.setMemorySnapshotForTesting(warning);
        manager.setSafetyReserve(0, 548);
        manager.acquire("video", QString::fromUtf8(TEST_VIDEO_FILE));
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("video"), 10000);
        const auto asset = manager.asset("video");
        const quint64 budget = asset->playbackBudgetBytes;
        QVERIFY(budget > 0);
        warning.availableBytes = 548 * MiB + 2 * budget;
        manager.setMemorySnapshotForTesting(warning);
        QVERIFY(!manager.pinOwners({"video", "video", "video"}, "too-many"));
        QVERIFY(manager.pinOwners({"video", "video"}, "pair"));
        QVERIFY(asset->reservePlayback());
        QVERIFY(asset->reservePlayback());
        QVERIFY(!asset->reservePlayback());
        QCOMPARE(manager.summary().value("pendingPlaybackBudgetBytes").toULongLong(), 2 * budget);
        QCOMPARE(manager.summary().value("loadableBytes").toULongLong(), quint64(0));
        manager.unpinGroup("pair");
        asset->releasePlayback(false);
        asset->releasePlayback(false);
        QVERIFY(asset->reservePlayback());
        asset->releasePlayback(false);
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
        QCOMPARE(summary.value("totalBytes").toULongLong(),
            summary.value("availableBytes").toULongLong() + summary.value("processBytes").toULongLong()
                + summary.value("otherBytes").toULongLong());
    }
};
QTEST_MAIN(MediaResidencyManagerTest)
#include "tst_MediaResidencyManager.moc"
