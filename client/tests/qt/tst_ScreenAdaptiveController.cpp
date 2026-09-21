#include "backend/screensharing/ScreenAdaptiveController.h"

#include <QtTest>

class ScreenAdaptiveControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void healthyHighLatencyUsesItsOwnBaseline() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("uplink"), 850, false, false, 0);
        QCOMPARE(controller.budget(0, false), limits.initialBps);
        controller.observe(QStringLiteral("uplink"), 940, false, false, 500);
        QCOMPARE(controller.budget(500, false), limits.initialBps);
        controller.observe(QStringLiteral("uplink"), 875, false, false, 1000);
        QCOMPARE(controller.budget(1000, false), limits.initialBps);
        // The viewer's much shorter propagation time must not become the
        // publisher leg's reference delay.
        controller.observe(QStringLiteral("viewer"), 20, false, false, 1000);
        controller.observe(QStringLiteral("uplink"), 960, false, false, 1500);
        QCOMPARE(controller.budget(1500, false), limits.initialBps);
    }

    void growingQueueReducesBudgetImmediately() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("uplink"), 100, false, false, 0);
        QCOMPARE(controller.budget(0, false), limits.initialBps);
        controller.observe(QStringLiteral("uplink"), 100 + limits.queueTargetMs + 1, false, false, 500);
        QCOMPARE(controller.budget(500, false), int(limits.initialBps * .65));
        for (qint64 at = 1000; at <= 10000; at += 500)
            controller.observe(QStringLiteral("uplink"), 500, true, false, at);
        QCOMPARE(controller.budget(10000, false), limits.minimumBps);
    }

    void congestionSignalsWithinFeedbackIntervalAreCoalesced() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.penalize(0);
        const int once = controller.budget(0, false);
        QCOMPARE(once, int(limits.initialBps * .65));
        for (qint64 at = 1; at < limits.feedbackIntervalMs; ++at) {
            controller.observe(QStringLiteral("uplink"), 100, true, false, at);
            controller.observe(QStringLiteral("viewer"), 30, true, false, at);
        }
        QCOMPARE(controller.budget(limits.feedbackIntervalMs - 1, false), once);
        controller.penalize(limits.feedbackIntervalMs);
        QCOMPARE(controller.budget(limits.feedbackIntervalMs, false), int(once * .65));
    }

    void recoveryIsGradualAndRequiresFreshFeedback() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("uplink"), 50, true, false, 0);
        const int reduced = controller.budget(0, false);
        controller.observe(QStringLiteral("uplink"), 50, false, false, 1000);
        QCOMPARE(controller.budget(limits.recoveryHoldMs - 1, false), reduced);
        // Silence alone is not evidence that a congested path recovered.
        QCOMPARE(controller.budget(limits.recoveryHoldMs + 1000, false), reduced);
        controller.observe(QStringLiteral("uplink"), 50, false, false, limits.recoveryHoldMs + 1001);
        const int firstRecovery = controller.budget(limits.recoveryHoldMs + 1001, false);
        QCOMPARE(firstRecovery, reduced + std::max(32000, reduced / 5));
        QCOMPARE(controller.budget(limits.recoveryHoldMs + 1002, false), firstRecovery);
        controller.observe(QStringLiteral("uplink"), 50, false, false, limits.recoveryHoldMs * 2 + 1001);
        const int secondRecovery = controller.budget(limits.recoveryHoldMs * 2 + 1001, false);
        QVERIFY(secondRecovery > firstRecovery);
        QVERIFY(secondRecovery < limits.initialBps);
    }

    void eachLegCanHoldRecoveryAndHintsExpire() {
        ScreenAdaptiveController::Limits limits;
        limits.recoveryHoldMs = 1000;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("slow-viewer"), 800, true, false, 0);
        const int reduced = controller.budget(0, false);
        controller.observe(QStringLiteral("healthy-uplink"), 10, false, false, 1200);
        QCOMPARE(controller.budget(1200, false), reduced);
        controller.observe(QStringLiteral("healthy-uplink"), 10, false, false, 2100);
        QVERIFY(controller.budget(2100, false) > reduced);
    }

    void transfersCapTheGlobalBudgetWithoutRepeatedlyDestroyingEstimate() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("uplink"), 20, false, false, 0);
        QCOMPARE(controller.budget(0, true), limits.uploadBps);
        QCOMPARE(controller.budget(1, false), limits.initialBps);
        controller.observe(QStringLiteral("viewer-a"), 30, false, true, 500);
        controller.observe(QStringLiteral("viewer-b"), 80, false, true, 500);
        QCOMPARE(controller.budget(500, false), limits.uploadBps);
        QCOMPARE(controller.budget(1000, true), limits.uploadBps);
        controller.observe(QStringLiteral("viewer-a"), 30, false, false, 1500);
        QCOMPARE(controller.budget(1500, false), limits.uploadBps);
        controller.observe(QStringLiteral("viewer-b"), 80, false, false, 1500);
        QCOMPARE(controller.budget(1500, false), limits.initialBps);
        // Stale remote activity expires, so a departed viewer cannot reserve
        // transfer capacity indefinitely.
        controller.observe(QStringLiteral("departed-viewer"), 10, false, true, 2000);
        QCOMPARE(controller.budget(2000, false), limits.uploadBps);
        QCOMPARE(controller.budget(4001, false), limits.initialBps);
    }

    void transferDoesNotIncreaseAnAlreadyLowerBudget() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.penalize(0);
        const int reduced = controller.budget(0, false);
        QVERIFY(reduced < limits.uploadBps);
        QCOMPARE(controller.budget(0, true), reduced);
    }

    void transportResetDropsLatencyHistoryButKeepsConservativeBudget() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("uplink"), 10, false, false, 0);
        controller.observe(QStringLiteral("uplink"), 400, false, false, 500);
        const int reduced = controller.budget(500, false);
        controller.transportReset(1000);
        QCOMPARE(controller.budget(1000, false), reduced);
        // The new route's intrinsic delay must establish a fresh baseline.
        controller.observe(QStringLiteral("uplink"), 900, false, false, 1100);
        QCOMPARE(controller.budget(1100, false), reduced);
        QCOMPARE(controller.budget(20000, false), reduced);
        controller.observe(QStringLiteral("uplink"), 910, false, false, 20000);
        QVERIFY(controller.budget(20000, false) > reduced);
    }

    void transportResetCapsAnEstimateRaisedOnPreviousTransport() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("uplink"), 10, false, false, 0);
        controller.budget(0, false);
        controller.observe(QStringLiteral("uplink"), 10, false, false, limits.recoveryHoldMs);
        QVERIFY(controller.budget(limits.recoveryHoldMs, false) > limits.initialBps);
        controller.transportReset(limits.recoveryHoldMs + 1);
        QCOMPARE(controller.budget(limits.recoveryHoldMs + 1, false), limits.initialBps);
    }

    void inactiveLegEventuallyGetsAFreshBaseline() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("viewer"), 10, false, false, 0);
        QCOMPARE(controller.budget(0, false), limits.initialBps);
        controller.observe(QStringLiteral("viewer"), 900, false, false, 31000);
        QVERIFY(controller.budget(31000, false) >= limits.initialBps);
    }

    void persistentRouteChangeAgesOutOldLatencyBaseline() {
        ScreenAdaptiveController::Limits limits;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("viewer"), 10, false, false, 0);
        for (int now = 1000; now < 30000; now += 1000) {
            controller.observe(QStringLiteral("viewer"), 500, false, false, now);
            controller.budget(now, false);
        }
        const int low = controller.budget(29000, false);
        // Active traffic on the new route must not pin its obsolete 10 ms
        // baseline forever; no reconnection is needed to recover quality.
        for (int now = 30000; now <= 40000; now += 1000) {
            controller.observe(QStringLiteral("viewer"), 500, false, false, now);
            controller.budget(now, false);
        }
        QVERIFY(controller.budget(40000, false) > low);
    }

    void nonAdaptiveModeStillProtectsTransfers() {
        ScreenAdaptiveController::Limits limits;
        limits.adaptive = false;
        ScreenAdaptiveController controller(limits);
        controller.observe(QStringLiteral("uplink"), 800, true, false, 0);
        QCOMPARE(controller.budget(0, false), limits.maximumBps);
        QCOMPARE(controller.budget(0, true), limits.uploadBps);
        controller.observe(QStringLiteral("viewer"), 800, true, true, 500);
        QCOMPARE(controller.budget(500, false), limits.uploadBps);
        controller.transportReset(1000);
        QCOMPARE(controller.budget(1000, false), limits.maximumBps);
    }
};

QTEST_GUILESS_MAIN(ScreenAdaptiveControllerTest)
#include "tst_ScreenAdaptiveController.moc"
