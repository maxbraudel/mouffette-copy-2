#include "backend/screensharing/ScreenPublicationProfiles.h"
#include "backend/screensharing/ScreenFrameAdmissionCeiling.h"
#include <QtTest>

class ScreenPublicationProfilesTest final : public QObject {
    Q_OBJECT
private slots:
    void allocateOnceAndRetainMainOnWeakUplink() {
        ScreenPublicationProfiles::Limits limits;
        const auto weak = ScreenPublicationProfiles::select(128000, 3840, true, false, false, limits);
        QCOMPARE(weak.size(), 1);
        QCOMPARE(weak.value("main").bitrateBps, 128000);
        const auto strong = ScreenPublicationProfiles::select(10000000, 3840, true, false, false, limits);
        QCOMPARE(strong.size(), 2);
        QCOMPARE(strong.value("main").maximumEdge, 3840);
        QCOMPARE(strong.value("main").bitrateBps + strong.value("low").bitrateBps, 10000000);
        QVERIFY(strong.value("low").maximumEdge <= 960);
        QVERIFY(strong.value("low").framesPerSecond <= 20);
        QCOMPARE(strong.value("low").bitrateBps, 750000);
    }
    void demandCpuAndConfigurationGateExtraEncoder() {
        ScreenPublicationProfiles::Limits limits;
        QCOMPARE(ScreenPublicationProfiles::select(10000000, 3840, false, false, false, limits).size(), 1);
        QCOMPARE(ScreenPublicationProfiles::select(10000000, 3840, true, true, true, limits).size(), 1);
        limits.lowEnabled = false;
        const auto disabled = ScreenPublicationProfiles::select(10000000, 3840, true, false, true, limits);
        QCOMPARE(disabled.size(), 1);
        QCOMPARE(disabled.value("main").bitrateBps, 10000000);
    }
    void collapseRedundantLayersAndHoldThreshold() {
        ScreenPublicationProfiles::Limits limits;
        limits.maximumFps = 5;
        QCOMPARE(ScreenPublicationProfiles::select(10000000, 160, true, false, false, limits).size(), 1);
        limits = {};
        limits.lowMinimumTotalBitrateBps = 1000000;
        QCOMPARE(ScreenPublicationProfiles::select(900000, 3840, true, false, false, limits).size(), 1);
        QCOMPARE(ScreenPublicationProfiles::select(900000, 3840, true, false, true, limits).size(), 2);
        QCOMPARE(ScreenPublicationProfiles::select(790000, 3840, true, false, true, limits).size(), 1);
    }
    void boundsAcrossBudgetsAndViewport() {
        ScreenPublicationProfiles::Limits limits;
        limits.maximumFps = 12;
        limits.maximumEdge = 1280;
        limits.lowMinimumTotalBitrateBps = 64000;
        limits.lowMaximumEdge = 160;
        for (int budget = 32000; budget <= 20000000; budget += 32000) {
            const auto profiles = ScreenPublicationProfiles::select(budget, 800, true, false, true, limits);
            int total = 0;
            for (auto profile : profiles) {
                total += profile.bitrateBps;
                QVERIFY(profile.maximumEdge <= 800);
                QVERIFY(profile.framesPerSecond <= 12);
                QVERIFY(profile.maximumEdge % 2 == 0);
            }
            QVERIFY(total <= budget);
            QVERIFY(profiles.size() <= 2);
        }
    }
    void oversizedIdrCanReduceBelowTheOrdinaryQualityLadder() {
        ScreenFrameAdmissionCeiling ceiling;
        ScreenStreamProfile current;
        current.maximumEdge = 320;
        current.framesPerSecond = 5;
        current.bitrateBps = 128000;
        current.idleIntervalMs = 1500;
        current.softwarePreset = QStringLiteral("fast");
        QVERIFY(ceiling.limit(current, 20000, 5000, 0));
        const auto limited = ceiling.apply(current, 0);
        QCOMPARE(limited.maximumEdge, 160);
        QCOMPARE(limited.bitrateBps, 32000);
        QCOMPARE(limited.framesPerSecond, 4);
        QCOMPARE(limited.idleIntervalMs, current.idleIntervalMs);
        QCOMPARE(limited.softwarePreset, current.softwarePreset);
    }
    void admissionReductionsAreCoalescedAndLayersRemainIndependent() {
        QHash<QString, ScreenFrameAdmissionCeiling> ceilings;
        ScreenStreamProfile desired;
        desired.maximumEdge = 1920;
        desired.framesPerSecond = 30;
        desired.bitrateBps = 4000000;
        QVERIFY(ceilings["low"].limit(desired, 40000, 10000, 0));
        const auto first = ceilings["low"].apply(desired, 0);
        QVERIFY(first.maximumEdge < desired.maximumEdge);
        QCOMPARE(first.maximumEdge % 2, 0);
        QVERIFY(ceilings["main"].apply(desired, 100) == desired);
        QVERIFY(!ceilings["low"].limit(first, 40000, 10000, 499));
        QVERIFY(ceilings["low"].apply(desired, 499) == first);
        QVERIFY(ceilings["low"].limit(first, 40000, 10000, 500));
        const auto next = ceilings["low"].apply(desired, 500);
        QVERIFY(next.maximumEdge < first.maximumEdge);
        QVERIFY(next.bitrateBps < first.bitrateBps);
        QVERIFY(next.framesPerSecond < first.framesPerSecond);
    }
    void rejectedFloorDoesNotCauseUnlimitedEncoderRestarts() {
        ScreenFrameAdmissionCeiling ceiling;
        ScreenStreamProfile floor;
        floor.maximumEdge = 160;
        floor.bitrateBps = 32000;
        floor.framesPerSecond = 1;
        QVERIFY(!ceiling.limit(floor, 10000, 1000, 0));
        QVERIFY(ceiling.apply(ScreenStreamProfile{}, 0) == floor);
        for (qint64 now = 500; now <= 10000; now += 500) {
            QVERIFY(!ceiling.limit(floor, 10000, 1000, now));
            QVERIFY(ceiling.apply(ScreenStreamProfile{}, now) == floor);
        }
        QVERIFY(ceiling.apply(ScreenStreamProfile{}, 14999) == floor);
        const auto recovering = ceiling.apply(ScreenStreamProfile{}, 15000);
        QVERIFY(recovering.maximumEdge > floor.maximumEdge);
        QVERIFY(recovering.maximumEdge < ScreenStreamProfile{}.maximumEdge);
    }
    void admissionCeilingRecoversSlowlyWithoutCatchupJumps() {
        ScreenFrameAdmissionCeiling ceiling;
        ScreenStreamProfile desired;
        QVERIFY(ceiling.limit(desired, 40000, 10000, 0));
        const auto initial = ceiling.apply(desired, 0);
        QVERIFY(ceiling.apply(desired, 4999) == initial);
        const auto step = ceiling.apply(desired, 5000);
        QVERIFY(step.maximumEdge > initial.maximumEdge);
        QVERIFY(step.maximumEdge <= initial.maximumEdge * 1.11);
        QVERIFY(step.maximumEdge < desired.maximumEdge);
        QVERIFY(ceiling.apply(desired, 5000) == step);
        QVERIFY(ceiling.apply(desired, 5999) == step);
        const auto afterPause = ceiling.apply(desired, 60000);
        QVERIFY(afterPause.maximumEdge > step.maximumEdge);
        QVERIFY(afterPause.maximumEdge <= step.maximumEdge * 1.11);
        auto smaller = desired;
        smaller.maximumEdge = 160;
        smaller.bitrateBps = 32000;
        smaller.framesPerSecond = 1;
        QVERIFY(ceiling.apply(smaller, 61000) == smaller);
    }
    void invalidAdmissionSamplesDoNotMutateOrExtendTheHold() {
        ScreenFrameAdmissionCeiling ceiling;
        const ScreenStreamProfile desired;
        QVERIFY(!ceiling.limit(desired, 0, 1000, 0));
        QVERIFY(!ceiling.limit(desired, 2000, 0, 0));
        QVERIFY(!ceiling.limit(desired, 2000, 1000, -1));
        QVERIFY(!ceiling.limit(desired, 1000, 1000, 0));
        QVERIFY(ceiling.apply(desired, 0) == desired);
        QVERIFY(ceiling.limit(desired, 40000, 10000, 0));
        const auto initial = ceiling.apply(desired, 0);
        QVERIFY(!ceiling.limit(initial, -1, 1000, 4999));
        QVERIFY(!ceiling.limit(initial, 1000, 1000, 4999));
        QVERIFY(ceiling.apply(desired, 5000).maximumEdge > initial.maximumEdge);
    }
};
QTEST_GUILESS_MAIN(ScreenPublicationProfilesTest)
#include "tst_ScreenPublicationProfiles.moc"
