#include <QtTest>
#include <QCursor>
#include "backend/managers/system/SystemMonitor.h"

class SystemMonitorTest : public QObject {
    Q_OBJECT
private slots:
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
