#include "backend/screensharing/ScreenAudioClock.h"
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
        QCOMPARE(clock.videoDelayUs(8200000, 2010000), 40000);
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
};
QTEST_GUILESS_MAIN(ScreenAudioClockTest)
#include "tst_ScreenAudioClock.moc"
