#include <QtTest>
#include <QCursor>
#include "backend/managers/system/SystemMonitor.h"

namespace {
class FakeVolumeBackend final : public SystemVolumeMonitorBackend {
public:
    using SystemVolumeMonitorBackend::SystemVolumeMonitorBackend;
    void start() override { ++starts; publish(initialVolume); }
    void stop() override { ++stops; }
    void change(int percent) { publish(percent); }
    int starts = 0;
    int stops = 0;
    int initialVolume = 42;
};
}

class SystemMonitorTest : public QObject {
    Q_OBJECT
private slots:
    void nativeVolumeBackendRestartsOnOwningThread()
    {
        QList<int> readings;
        bool correctThread = true;
        const auto ownerThread = QThread::currentThread();
        auto backend = createSystemVolumeMonitorBackend([&](int percent) {
            correctThread = correctThread && QThread::currentThread() == ownerThread;
            readings.append(percent);
        });
        for (int cycle = 0; cycle < 5; ++cycle) {
            const auto before = readings.size();
            backend->start();
            QVERIFY(readings.size() > before); // Read even when no audio output exists.
            QTest::qWait(20);
            backend->stop();
            const auto stopped = readings.size();
            QTest::qWait(20);
            QCOMPARE(readings.size(), stopped);
        }
        backend.reset();
        const auto destroyed = readings.size();
        QTest::qWait(20);
        QCOMPARE(readings.size(), destroyed);
        QVERIFY(correctThread);
        for (int percent : readings) QVERIFY(percent >= -1 && percent <= 100);
    }

    void volumeInitialSampleChangesLossAndRestart()
    {
        FakeVolumeBackend* backend = nullptr;
        SystemMonitor monitor(nullptr, {}, [&](SystemVolumeMonitorBackend::Publish publish) {
            auto result = std::make_unique<FakeVolumeBackend>(std::move(publish));
            backend = result.get();
            return result;
        });
        QSignalSpy changes(&monitor, &SystemMonitor::volumeChanged);
        QCOMPARE(monitor.getSystemVolumePercent(), -1);
        monitor.startVolumeMonitoring();
        QCOMPARE(monitor.getSystemVolumePercent(), 42);
        QCOMPARE(changes.size(), 1); // Initial sample does not wait for a timer.
        monitor.startVolumeMonitoring();
        QCOMPARE(backend->starts, 1);
        backend->change(73);
        QCOMPARE(monitor.getSystemVolumePercent(), 73);
        QCOMPARE(changes.size(), 2);
        backend->change(73);
        QCOMPARE(changes.size(), 2);
        backend->change(-1); // Device removed/API unavailable.
        QCOMPARE(monitor.getSystemVolumePercent(), -1);
        QCOMPARE(changes.last().first().toInt(), -1);
        backend->change(101); // Never advertise an invalid percentage.
        QCOMPARE(changes.size(), 3);
        backend->change(0);
        QCOMPARE(monitor.getSystemVolumePercent(), 0);
        backend->change(100);
        QCOMPARE(monitor.getSystemVolumePercent(), 100);
        monitor.stopVolumeMonitoring();
        QCOMPARE(monitor.getSystemVolumePercent(), -1);
        const int stoppedCount = changes.size();
        backend->change(15);
        QCOMPARE(changes.size(), stoppedCount);
        backend->initialVolume = 28;
        monitor.startVolumeMonitoring();
        QCOMPARE(backend->starts, 2);
        QCOMPARE(monitor.getSystemVolumePercent(), 28);
        QCOMPARE(changes.size(), stoppedCount + 1);
    }

    void missedNotificationRefreshesInventoryAndCursorMapping()
    {
        bool valid = true;
        int captures = 0;
        LocalScreenTopology::Screen left, right;
        left.geometry = QRect(-100, 0, 100, 100);
        left.advertisedGeometry = QRect(-200, 0, 200, 200);
        left.advertisedAvailableGeometry = left.advertisedGeometry;
        right.geometry = QRect(0, 0, 200, 100);
        right.advertisedGeometry = QRect(0, 0, 400, 200);
        right.advertisedAvailableGeometry = QRect(0, 20, 400, 160);
        right.primary = true;
        QList<LocalScreenTopology::Screen> inventory{left, right};
        SystemMonitor monitor(nullptr, [&](bool* success) {
            ++captures;
            *success = valid;
            return inventory;
        });
        QList<ScreenInfo> screens;
        QVERIFY(monitor.captureScreenInfo(&screens));
        QCOMPARE(screens.size(), 2);
        QCOMPARE(screens[1].uiZones.size(), 2);
        QCursor::setPos(10, 12);
        int screenId = -1;
        QPointF position;
        for (int i = 0; i < 60; ++i) {
            QVERIFY(monitor.getLocalCursorPosition(&screenId, &position));
            QCOMPARE(screenId, 1);
            QCOMPARE(position, QPointF(20, 24));
        }
        QCOMPARE(captures, 1); // Cursor ticks never re-enumerate the desktop.
        inventory = {right}; // No screenChanged event at all.
        QVERIFY(monitor.captureScreenInfo(&screens));
        QCOMPARE(screens.size(), 1);
        QVERIFY(monitor.getLocalCursorPosition(&screenId, &position));
        QCOMPARE(screenId, 0);
        valid = false;
        QVERIFY(!monitor.captureScreenInfo(&screens));
        QCOMPARE(monitor.getLocalScreenInfo().size(), 1);
        QVERIFY(!monitor.getLocalCursorPosition(&screenId, &position));
        valid = true;
        inventory.clear();
        QVERIFY(monitor.captureScreenInfo(&screens));
        QVERIFY(screens.isEmpty());
        QVERIFY(!monitor.getLocalCursorPosition(&screenId, &position));
    }
};
QTEST_MAIN(SystemMonitorTest)
#include "tst_SystemMonitor.moc"
