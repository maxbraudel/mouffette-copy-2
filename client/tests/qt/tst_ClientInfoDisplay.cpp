#include <QtTest>

#include "backend/domain/models/ClientInfo.h"

class ClientInfoDisplayTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesEveryPublicAvailabilityBadge_data();
    void normalizesEveryPublicAvailabilityBadge();
    void disconnectedPresenceOverridesStaleSessionState();
    void formatsProjectCountdowns();
    void formatsCountdownBoundaries();
    void appendsInstanceNumberOnlyForSecondaryProfiles();
};

void ClientInfoDisplayTest::normalizesEveryPublicAvailabilityBadge_data()
{
    QTest::addColumn<QString>("wireStatus");
    QTest::addColumn<QString>("displayStatus");

    QTest::newRow("available") << QStringLiteral("Available") << QStringLiteral("Available");
    QTest::newRow("connecting") << QStringLiteral("opening") << QStringLiteral("Connecting");
    QTest::newRow("connected") << QStringLiteral("active") << QStringLiteral("Connected");
    QTest::newRow("reconnecting") << QStringLiteral("grace") << QStringLiteral("Reconnecting");
    QTest::newRow("disconnecting") << QStringLiteral("cleanup_pending") << QStringLiteral("Disconnecting");
    QTest::newRow("disconnected") << QStringLiteral("Disconnected") << QStringLiteral("Disconnected");
    QTest::newRow("unreachable") << QStringLiteral("Unreachable") << QStringLiteral("Unreachable");
}

void ClientInfoDisplayTest::normalizesEveryPublicAvailabilityBadge()
{
    QFETCH(QString, wireStatus);
    QFETCH(QString, displayStatus);

    ClientInfo client(QStringLiteral("device-a"), QStringLiteral("Studio A"),
                      QStringLiteral("Linux"));
    client.setAvailabilityStatus(wireStatus);
    QCOMPARE(client.availabilityBadgeText(), displayStatus);
}

void ClientInfoDisplayTest::disconnectedPresenceOverridesStaleSessionState()
{
    ClientInfo client(QStringLiteral("device-b"), QStringLiteral("Studio B"),
                      QStringLiteral("macOS"));
    client.setAvailabilityStatus(QStringLiteral("Connected"));
    client.setOnline(false);
    client.setHasProject(true);

    QCOMPARE(client.availabilityBadgeText(), QStringLiteral("Disconnected"));
    QCOMPARE(client.getProjectSummaryText(1'000), QStringLiteral("Project"));
    QVERIFY(client.getDisplayText().endsWith(QStringLiteral("— Disconnected")));
}

void ClientInfoDisplayTest::formatsProjectCountdowns()
{
    ClientInfo client(QStringLiteral("device-c"), QStringLiteral("Studio C"),
                      QStringLiteral("Windows"));
    client.setHasProject(true);
    client.setRemoteSessionCloseAtMs(60'000);
    client.setProjectDeleteAtMs(300'000);

    QCOMPARE(client.getProjectSummaryText(1'000),
             QStringLiteral("Project · Disconnect in 0:59 · Delete project in 4:59"));
    QCOMPARE(client.getProjectSummaryText(60'001),
             QStringLiteral("Project · Delete project in 4:00"));

    client.setHasProject(false);
    QVERIFY(client.getProjectSummaryText(1'000).isEmpty());
}

void ClientInfoDisplayTest::formatsCountdownBoundaries()
{
    QCOMPARE(ClientInfo::formatRemainingTime(-1), QStringLiteral("0:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(0), QStringLiteral("0:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(1), QStringLiteral("0:01"));
    QCOMPARE(ClientInfo::formatRemainingTime(59'001), QStringLiteral("1:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(300'000), QStringLiteral("5:00"));
}

void ClientInfoDisplayTest::appendsInstanceNumberOnlyForSecondaryProfiles()
{
    ClientInfo primary(QStringLiteral("endpoint-primary"),
                       QStringLiteral("Studio"), QStringLiteral("macOS"));
    primary.setInstanceOrdinal(1);
    QVERIFY(!primary.getIdentityDisplayText().contains(QStringLiteral("Instance")));

    ClientInfo secondary(QStringLiteral("endpoint-secondary"),
                         QStringLiteral("Studio"), QStringLiteral("macOS"));
    secondary.setInstanceOrdinal(3);
    QVERIFY(secondary.getIdentityDisplayText().contains(
        QStringLiteral("Studio — Instance 3")));
}

QTEST_APPLESS_MAIN(ClientInfoDisplayTest)
#include "tst_ClientInfoDisplay.moc"
