#include <QtTest>
#include "backend/network/RetryPolicy.h"
#include "backend/network/RetryScheduler.h"
#include "backend/network/SessionRecoveryController.h"
#include "backend/config/AppConfig.h"

class RetryPolicyTest : public QObject {
    Q_OBJECT
private slots:
    void sessionActionsAndPresenceKeepFailureHistory() {
        qint64 clock = 100;
        SessionRecoveryController recovery([&] { return clock; }, [] { return quint64(0); });
        int attempts = 0;
        recovery.ready = [&](const QString& endpoint) {
            ++attempts;
            recovery.running(endpoint);
        };
        recovery.retry("peer", "target_unavailable");
        QCOMPARE(recovery.diagnostic("peer").action, RetryAction::Scheduled);
        QCOMPARE(recovery.nextAttemptAtMs("peer"), 900);
        recovery.retry("peer", "duplicate_result");
        QCOMPARE(recovery.diagnostic("peer").attempts, 1);
        recovery.expedite("peer");
        QCOMPARE(recovery.diagnostic("peer").attempts, 1);
        recovery.retry("peer", "target_unavailable");
        QCOMPARE(recovery.nextAttemptAtMs("peer"), 1700);
        clock = 1700;
        recovery.processDue();
        QCOMPARE(attempts, 1);
        QCOMPARE(recovery.diagnostic("peer").action, RetryAction::Running);
        recovery.pause("peer", "waiting_for_presence");
        QCOMPARE(recovery.diagnostic("peer").action, RetryAction::Suspended);
        recovery.block("peer", "invalid_capability");
        recovery.suspend();
        QCOMPARE(recovery.diagnostic("peer").action, RetryAction::Blocked);
        recovery.reset("peer");
        QCOMPARE(recovery.diagnostic("peer").action, RetryAction::Idle);
        QCOMPARE(recovery.diagnostic("peer").attempts, 0);
        clock += 100000;
        recovery.processDue();
        QCOMPARE(attempts, 1);
    }
    void hardCeilingsAndSaturation() {
        const auto low = [] { return quint64(0); };
        for (auto growth : {RetryPolicy::Growth::Exponential, RetryPolicy::Growth::Linear, RetryPolicy::Growth::Fixed}) {
            const RetryPolicy policy{1000, 5000, 20, growth};
            for (int attempt : {-1, 0, 1, 2, 3, 30, INT_MAX}) {
                for (quint64 random : {quint64(0), quint64(2000), std::numeric_limits<quint64>::max()}) {
                    const int delay = policy.delay(attempt, [random] { return random; });
                    QVERIFY(delay >= 1);
                    QVERIFY(delay <= 5000);
                }
            }
        }
        const RetryPolicy background{1000, 5000, 20};
        QCOMPARE(background.delay(0, low), 800);
        QCOMPARE(background.delay(1, low), 1600);
        QCOMPARE(background.delay(2, low), 3200);
        QCOMPARE(background.delay(INT_MAX, low), 4000);
        QCOMPARE(background.delay(INT_MAX, [] { return quint64(1000); }), 5000);
        const RetryPolicy fast{250, 750, 20, RetryPolicy::Growth::Linear, true};
        QCOMPARE(fast.delay(0, low), 0);
        QCOMPARE(fast.delay(0, [] { return quint64(250); }), 250);
        QCOMPARE(fast.delay(30, low), 600);
        QCOMPARE(RetryPolicy::increment(INT_MAX), 30);
    }
    void cancellationReplacementAndSleep() {
        qint64 clock = 100;
        RetryScheduler scheduler(nullptr, [&] { return clock; });
        int calls = 0;
        scheduler.schedule("connection", 100, [&] { calls += 100; });
        scheduler.schedule("connection", 200, [&] { ++calls; });
        QCOMPARE(scheduler.size(), 1);
        clock = 200;
        scheduler.processDue();
        QCOMPARE(calls, 0);
        clock = 300;
        scheduler.processDue();
        QCOMPARE(calls, 1);
        scheduler.schedule("connection", 100, [&] { ++calls; });
        scheduler.cancelAll();
        clock += 3600000; // suspended-inclusive clock after a long sleep
        scheduler.processDue();
        QCOMPARE(calls, 1);
        scheduler.schedule("operation", 100, [&] {
            ++calls;
            scheduler.schedule("operation", 100, [&] { ++calls; });
        });
        clock += 3600000;
        scheduler.processDue();
        QCOMPARE(calls, 2); // no catch-up burst
        scheduler.processDue();
        QCOMPARE(calls, 2);
        clock += 100;
        scheduler.processDue();
        QCOMPARE(calls, 3);
        QCOMPARE(scheduler.size(), 0);
    }
    void repeatedDisturbancesStayBounded() {
        qint64 clock = 0;
        RetryScheduler scheduler(nullptr, [&] { return clock; });
        int calls = 0;
        for (int i = 0; i < 100000; ++i) {
            scheduler.schedule("same-operation", 1, [&] { ++calls; });
            QCOMPARE(scheduler.size(), 1);
            if (i % 2) scheduler.cancelAll();
            ++clock;
            scheduler.processDue();
            QCOMPARE(scheduler.size(), 0);
        }
        QCOMPARE(calls, 50000);
    }
    void onlyContinuousReadinessResetsFailures() {
        StableConnectionWindow stable;
        QVERIFY(!stable.transition(false, 0, 30000));
        QVERIFY(!stable.transition(false, 60000, 30000)); // authentication/sync age does not count
        QVERIFY(!stable.transition(true, 60000, 30000));
        QVERIFY(!stable.transition(false, 89999, 30000)); // degraded just before boundary
        QVERIFY(!stable.transition(true, 90000, 30000));
        QVERIFY(!stable.transition(true, 119999, 30000));
        QVERIFY(stable.transition(false, 120000, 30000)); // exact healthy boundary
        QVERIFY(!stable.transition(false, 200000, 30000));
        QVERIFY(!stable.transition(true, 200000, 30000));
        stable.reset(); // Disable cancels the window
        QVERIFY(!stable.transition(false, 400000, 30000));
        QVERIFY(!stable.transition(true, 400000, 30000, 1500));
        // Sleep may advance a monotonic clock, but cannot prove healthy service.
        QVERIFY(!stable.transition(true, 460000, 30000, 1500));
        for (qint64 now = 460750; now < 490000; now += 750)
            QVERIFY(!stable.transition(true, now, 30000, 1500));
        QVERIFY(stable.transition(true, 490000, 30000, 1500));
    }
    void configurationSeparatesFamilies() {
        AppConfig config;
        AppConfig::LoadOptions options;
        options.defaultEnvFilePath.clear();
        QString error;
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.reconnectMaxMs(), 5000);
        QCOMPARE(config.sessionRetryMaxMs(), 5000);
        QCOMPARE(config.connectionSyncTimeoutMs(), 10000);
        QCOMPARE(config.controlRequestRetryMs(), 1000);
        QCOMPARE(config.uploadChannelRetryMaxMs(), 5000);
        QCOMPARE(config.uploadChannelAttemptTimeoutMs(), 10000);
        QCOMPARE(config.deferredCleanupRetryMaxMs(), 30000);
        options.processEnvironment.insert("MOUFFETTE_SESSION_RETRY_BASE_MS", "2000");
        QVERIFY(config.load(options, &error));
        QCOMPARE(config.sessionRetryBaseMs(), 2000);
        QCOMPARE(config.reconnectBaseMs(), 1000);
        options.processEnvironment.insert("MOUFFETTE_SESSION_RETRY_MAX_MS", "1000");
        QVERIFY(!config.load(options, &error));
        options.processEnvironment.remove("MOUFFETTE_SESSION_RETRY_MAX_MS");
        options.processEnvironment.insert("MOUFFETTE_UPLOAD_CHANNEL_RETRY_MAX_MS", "500");
        QVERIFY(!config.load(options, &error));
        options.processEnvironment.remove("MOUFFETTE_UPLOAD_CHANNEL_RETRY_MAX_MS");
        options.processEnvironment.insert("MOUFFETTE_DEFERRED_CLEANUP_RETRY_MAX_MS", "500");
        QVERIFY(!config.load(options, &error));
    }
};

QTEST_GUILESS_MAIN(RetryPolicyTest)
#include "tst_RetryPolicy.moc"
