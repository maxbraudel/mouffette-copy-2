#include <QtTest>
#include <QImage>
#include <QTemporaryDir>
#include <QSignalSpy>
#include "backend/media/MediaResidencyManager.h"

class MediaResidencyManagerTest : public QObject {
    Q_OBJECT
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
        manager.release("blocked");
        manager.setSafetyReserve(0, 0);
        QCOMPARE(manager.summary().value("reserveBytes").toULongLong(), quint64(0));
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
    void compressedVideoAccountingAndAtomicPlaybackAdmission() {
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
        bool sawPreparation = false;
        connect(&manager, &MediaResidencyManager::changed, &manager, [&] {
            for (const auto& value : manager.assets()) {
                const auto row = value.toMap();
                if (row.value("state").toString() != QLatin1String("decoding")) continue;
                sawPreparation = true;
                // Actual retained bytes must not include the decoder scratch.
                QVERIFY(row.value("residentBytes").toULongLong()
                    <= row.value("estimatedBytes").toULongLong() + 65536);
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
        asset->releasePlayback();
        manager.unpinGroup("one-player");
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), quint64(0));
        manager.setMemorySnapshotForTesting(memory());
        QVERIFY(manager.pinOwners({"first", "first"}, "duplicate-remote-occurrences"));
        QCOMPARE(manager.summary().value("playbackBudgetBytes").toULongLong(), 2 * budget);
        manager.release("first");
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
    void evictsLargestAndWaitsForHeadroom() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
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
        manager.setMemorySnapshotForTesting(memory());
        manager.sampleNow();
        QTRY_VERIFY_WITH_TIMEOUT(manager.ready("large"), 10000);
    }
    void atomicPinsAndControlledStop() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
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
    void waitingImportNeverEvictsReadyMediaToRetry() {
        QTemporaryDir dir;
        MediaResidencyManager manager;
        manager.setMemorySnapshotForTesting(memory());
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
QTEST_GUILESS_MAIN(MediaResidencyManagerTest)
#include "tst_MediaResidencyManager.moc"
