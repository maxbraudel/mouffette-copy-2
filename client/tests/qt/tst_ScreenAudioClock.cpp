#include "backend/screensharing/ScreenAudioClock.h"
#include "backend/screensharing/ScreenPresentationQueue.h"
#include "backend/screensharing/CaptureTimestampMapper.h"
#include <QtTest>

class ScreenAudioClockTest final : public QObject {
    Q_OBJECT
private slots:
    void videoWaitIsBoundedAndLateFramesStayImmediate() {
        ScreenAudioClock clock;
        QCOMPARE(clock.videoDelayUs(8000000, 2000000), 0);
        clock.update(8000000, 2000000);
        QCOMPARE(clock.videoDelayUs(8040000, 2010000), 30000);
        QCOMPARE(clock.videoDelayUs(8200000, 2010000), 150000);
        QCOMPARE(clock.videoDelayUs(7990000, 2010000), 0);
        QCOMPARE(clock.videoDelayUs(8060000, 2050000), 10000);
    }
    void staleAndDifferentEpochsNeverHoldVideo() {
        ScreenAudioClock clock;
        clock.update(9000000, 3000000);
        QCOMPARE(clock.videoDelayUs(9400000, 3250001), 0);
        QCOMPARE(clock.videoDelayUs(1000, 3000000), 0);
        QCOMPARE(clock.videoDelayUs(-1, 3000000), 0);
        clock.reset();
        QCOMPARE(clock.videoDelayUs(9040000, 3000000), 0);
    }
    void ipcArrivalDoesNotReanchorTheOutputClock() {
        ScreenAudioClock clock;
        // The worker measured this pair 60 ms before its IPC was handled.
        clock.update(8000000, 2000000);
        QCOMPARE(clock.videoDelayUs(8080000, 2060000), 20000);
        // An older queued report cannot move the clock backwards.
        clock.update(7000000, 1990000);
        QCOMPARE(clock.videoDelayUs(8080000, 2060000), 20000);
        clock.update(-1, 2070000);
        QCOMPARE(clock.videoDelayUs(8080000, 2070000), 10000);
    }
    void submittedFutureAudioSamplesStillProvideAClock() {
        ScreenAudioClock clock;
        clock.update(8000000, 2020000);
        QCOMPARE(clock.videoDelayUs(8000000, 2000000), 20000);
        QCOMPARE(clock.videoDelayUs(8080000, 2000000), 100000);
        QCOMPARE(clock.videoDelayUs(8080000, 1800000), 0);
    }
    void newImagesNeverInheritOlderPresentationDeadlines() {
        ScreenAudioClock clock;
        clock.update(1000000, 2000000);
        ScreenPresentationQueue<int> frames;
        frames.push(1, 1080000, 2000000, 4);
        frames.push(2, 1100000, 2020000, 4);
        frames.push(3, 1120000, 2040000, 4);
        QVERIFY(!frames.takeReady(clock, 2079999));
        QCOMPARE(*frames.takeReady(clock, 2080000), 1);
        QVERIFY(!frames.takeReady(clock, 2099999));
        QCOMPARE(*frames.takeReady(clock, 2100000), 2);
        QCOMPARE(*frames.takeReady(clock, 2120000), 3);
        QVERIFY(frames.empty());
    }
    void sustainedSixtyFpsRemainsSynchronized() {
        ScreenAudioClock clock;
        ScreenPresentationQueue<qint64> frames;
        int presented = 0;
        for (qint64 elapsed = 0; elapsed < 30000000; elapsed += 1000) {
            const qint64 now = 1000000 + elapsed;
            if (elapsed % 20000 == 0) clock.update(5000000 + elapsed, now);
            if (elapsed % 16000 == 0) {
                const qint64 pts = 5000000 + elapsed + 96000;
                frames.push(pts, pts, now, 1920 * 1080 * 4);
            }
            if (const auto frame = frames.takeReady(clock, now)) {
                QVERIFY(*frame <= 5000000 + elapsed);
                QVERIFY(5000000 + elapsed - *frame < 16000);
                ++presented;
            }
            QVERIFY(frames.size() <= 7);
            QVERIFY(frames.bytes() <= ScreenPresentationQueue<qint64>::MaximumBytes);
        }
        QVERIFY(presented > 1800);
    }
    void stallsSkipOnlyImagesWhosePresentationTimeHasPassed() {
        ScreenAudioClock clock;
        clock.update(1000000, 2000000);
        ScreenPresentationQueue<int> frames;
        frames.push(1, 1080000, 2000000, 4);
        frames.push(2, 1100000, 2020000, 4);
        frames.push(3, 1140000, 2040000, 4);
        QCOMPARE(*frames.takeReady(clock, 2110000), 2);
        QCOMPARE(frames.size(), size_t(1));
        QCOMPARE(*frames.takeReady(clock, 2140000), 3);
    }
    void memoryPressureDropsFramesWithoutPresentingFutureImagesEarly() {
        ScreenAudioClock clock;
        clock.update(1000000, 2000000);
        ScreenPresentationQueue<int> frames;
        frames.push(1, 1080000, 2000000, 16, 32);
        frames.push(2, 1100000, 2020000, 16, 32);
        frames.push(3, 1120000, 2040000, 16, 32);
        QCOMPARE(frames.bytes(), 32);
        QCOMPARE(frames.size(), size_t(2));
        QCOMPARE(*frames.takeReady(clock, 2080000), 1);
        QVERIFY(!frames.takeReady(clock, 2100000));
        QCOMPARE(*frames.takeReady(clock, 2120000), 3);
    }
    void sustainedVideoWithOneSurfaceBudgetCannotStarvePresentation() {
        ScreenAudioClock clock;
        ScreenPresentationQueue<qint64> frames;
        int presented = 0;
        constexpr qint64 cost = 3840LL * 2160 * 4;
        for (qint64 elapsed = 0; elapsed < 3000000; elapsed += 1000) {
            const qint64 now = 1000000 + elapsed;
            if (elapsed % 20000 == 0) clock.update(5000000 + elapsed, now);
            if (elapsed % 16000 == 0) {
                const qint64 pts = 5000000 + elapsed + 96000;
                frames.push(pts, pts, now, cost, cost);
            }
            if (const auto frame = frames.takeReady(clock, now)) {
                QVERIFY(*frame <= 5000000 + elapsed);
                QVERIFY(5000000 + elapsed - *frame < 2000);
                ++presented;
            }
            QVERIFY(frames.bytes() <= cost);
        }
        QVERIFY(presented >= 25);
    }
    void fourKPlanarSurfacesFitTheNormalJitterWindow() {
        ScreenAudioClock clock;
        ScreenPresentationQueue<qint64> frames;
        int presented = 0;
        constexpr qint64 cost = 3840LL * 2160 * 3 / 2;
        for (qint64 elapsed = 0; elapsed < 3000000; elapsed += 1000) {
            const qint64 now = 1000000 + elapsed;
            if (elapsed % 20000 == 0) clock.update(5000000 + elapsed, now);
            if (const auto frame = frames.takeReady(clock, now)) {
                QCOMPARE(*frame, 5000000 + elapsed);
                ++presented;
            }
            if (elapsed % 16000 == 0) {
                const qint64 pts = 5000000 + elapsed + 80000;
                frames.push(pts, pts, now, cost);
            }
            QVERIFY(frames.bytes() <= ScreenPresentationQueue<qint64>::MaximumBytes);
        }
        QCOMPARE(presented, 183);
    }
    void inactiveAudioAndRunawayClockCannotFreezeTheScreen() {
        ScreenAudioClock clock;
        ScreenPresentationQueue<int> frames;
        frames.push(1, 1080000, 2000000, 4);
        frames.push(2, 1100000, 2020000, 4);
        QCOMPARE(*frames.takeReady(clock, 2020000), 2);
        clock.update(1000000, 2000000);
        frames.push(3, 1900000, 2020000, 4);
        // Continuous but lagging audio still cannot accumulate video latency.
        clock.update(1020000, 2169999);
        QVERIFY(!frames.takeReady(clock, 2169999));
        QCOMPARE(*frames.takeReady(clock, 2170000), 3);
        frames.push(4, 1950000, 2200000, 4);
        clock.reset();
        QCOMPARE(*frames.takeReady(clock, 2200000), 4);
        frames.push(5, 2000000, 2200000, 4);
        frames.clear();
        QVERIFY(!frames.takeReady(clock, 2400000));
        QCOMPARE(frames.bytes(), 0);
    }
    void nativeCaptureSpacingSurvivesCallbackJitter() {
        CaptureTimestampMapper mapper;
        QCOMPARE(mapper.map(0, 9000000), 9000000);
        QCOMPARE(mapper.map(20000, 9024000), 9020000);
        QCOMPARE(mapper.map(40000, 9050000), 9040000);
        // Recreated capture source resets its relative clock.
        QCOMPARE(mapper.map(0, 9060000), 9060000);
        QCOMPARE(mapper.map(-1, 9100000), 9100000);
    }
    void nativeHostClockDoesNotAcquireAPerMonitorOffset() {
        CaptureTimestampMapper first, second;
        QCOMPARE(first.map(20000000, 20002000), 20000000);
        QCOMPARE(second.map(20000000, 20008000), 20000000);
        QCOMPARE(first.map(22000000, 22001000), 22000000);
    }
    void delayedHostTimestampsNeverBecomeFreshByReanchoring() {
        QCOMPARE(CaptureTimestampMapper::hostTimestamp(20000000, 23000000), 20000000);
        QCOMPARE(CaptureTimestampMapper::hostTimestamp(20000000, 20008000), 20000000);
        QCOMPARE(CaptureTimestampMapper::hostTimestamp(-1, 23000000), 23000000);
    }
};
QTEST_GUILESS_MAIN(ScreenAudioClockTest)
#include "tst_ScreenAudioClock.moc"
