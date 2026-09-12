#include <QtTest>

#include "backend/managers/system/SystemLifecycleMonitor.h"

class SystemLifecycleMonitorTest final : public QObject
{
    Q_OBJECT

private slots:
    void lockAndUnlockEmitAggregateTransitions();
    void overlappingSleepAndLockStaySuspendedUntilBothClear();
    void duplicateNativeEventsAreIdempotent();
};

void SystemLifecycleMonitorTest::lockAndUnlockEmitAggregateTransitions()
{
    SystemLifecycleMonitor monitor;
    QSignalSpy suspendedSpy(&monitor, &SystemLifecycleMonitor::systemSuspendedChanged);

    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SessionLocked);
    QVERIFY(monitor.isSessionLocked());
    QVERIFY(monitor.isSystemSuspended());
    QCOMPARE(suspendedSpy.count(), 1);
    QCOMPARE(suspendedSpy.takeFirst().at(0).toBool(), true);

    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SessionUnlocked);
    QVERIFY(!monitor.isSessionLocked());
    QVERIFY(!monitor.isSystemSuspended());
    QCOMPARE(suspendedSpy.count(), 1);
    QCOMPARE(suspendedSpy.takeFirst().at(0).toBool(), false);
}

void SystemLifecycleMonitorTest::overlappingSleepAndLockStaySuspendedUntilBothClear()
{
    SystemLifecycleMonitor monitor;
    QSignalSpy suspendedSpy(&monitor, &SystemLifecycleMonitor::systemSuspendedChanged);

    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SessionLocked);
    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SystemWillSleep);
    QCOMPARE(suspendedSpy.count(), 1);

    // Native ordering is not stable across platforms. Waking first must not
    // reveal a project while the login session is still locked.
    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SystemDidWake);
    QVERIFY(monitor.isSystemSuspended());
    QCOMPARE(suspendedSpy.count(), 1);

    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SessionUnlocked);
    QVERIFY(!monitor.isSystemSuspended());
    QCOMPARE(suspendedSpy.count(), 2);
    QCOMPARE(suspendedSpy.at(0).at(0).toBool(), true);
    QCOMPARE(suspendedSpy.at(1).at(0).toBool(), false);
}

void SystemLifecycleMonitorTest::duplicateNativeEventsAreIdempotent()
{
    SystemLifecycleMonitor monitor;
    QSignalSpy suspendedSpy(&monitor, &SystemLifecycleMonitor::systemSuspendedChanged);

    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SystemWillSleep);
    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SystemWillSleep);
    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SystemDidWake);
    monitor.simulateNativeEventForTesting(
        SystemLifecycleMonitor::NativeEvent::SystemDidWake);

    QCOMPARE(suspendedSpy.count(), 2);
    QCOMPARE(suspendedSpy.at(0).at(0).toBool(), true);
    QCOMPARE(suspendedSpy.at(1).at(0).toBool(), false);
}

QTEST_APPLESS_MAIN(SystemLifecycleMonitorTest)
#include "tst_SystemLifecycleMonitor.moc"
