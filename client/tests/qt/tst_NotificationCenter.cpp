#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend/notifications/HistoryStore.h"
#include "backend/notifications/NotificationCenter.h"

namespace {
NotificationRequest request(const QString& message, qint64 timestamp)
{
    NotificationRequest result;
    result.message = message;
    result.timestampMs = timestamp;
    result.category = QStringLiteral("Test");
    return result;
}
}

class NotificationCenterTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<NotificationEntry>();
        qRegisterMetaType<NotificationSeverity>();
    }

    void persistsUnreadStateAndMarksNewVisibleEntriesRead()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.filePath(QStringLiteral("history.json")));
        NotificationCenter center(&store);

        QSignalSpy unreadSpy(&center, &NotificationCenter::unreadCountChanged);
        QVERIFY(!center.publish(request(QStringLiteral("First"), 1)).isEmpty());
        QVERIFY(!center.publish(request(QStringLiteral("Second"), 2)).isEmpty());
        QCOMPARE(center.unreadCount(), 2);
        QVERIFY(center.setHistoryVisible(true));
        QCOMPARE(center.unreadCount(), 0);

        QVERIFY(!center.publish(request(QStringLiteral("While open"), 3)).isEmpty());
        QCOMPARE(center.unreadCount(), 0);
        QVERIFY(center.entries().first().read);
        QVERIFY(center.setHistoryVisible(false));
        QVERIFY(!center.publish(request(QStringLiteral("After close"), 4)).isEmpty());
        QCOMPARE(center.unreadCount(), 1);
        QVERIFY(unreadSpy.count() >= 3);

        NotificationCenter restored(&store);
        QCOMPARE(restored.entries().size(), 4);
        QCOMPARE(restored.entries().first().message, QStringLiteral("After close"));
        QCOMPARE(restored.unreadCount(), 1);
    }

    void keepsNewestOneHundredEntries()
    {
        QTemporaryDir temporary;
        HistoryStore store(temporary.filePath(QStringLiteral("history.json")));
        NotificationCenter center(&store);
        for (int index = 0; index < 105; ++index) {
            QVERIFY(!center.publish(request(QStringLiteral("Message %1").arg(index), index)).isEmpty());
        }
        QCOMPARE(center.entries().size(), HistoryStore::MaximumEntries);
        QCOMPARE(center.entries().first().message, QStringLiteral("Message 104"));
        QCOMPARE(center.entries().last().message, QStringLiteral("Message 5"));

        NotificationHistoryData persisted;
        QVERIFY(store.load(&persisted));
        QCOMPARE(persisted.entries.size(), HistoryStore::MaximumEntries);
    }

    void terminalDeduplicationSurvivesEvictionAndRestart()
    {
        QTemporaryDir temporary;
        HistoryStore store(temporary.filePath(QStringLiteral("history.json")));
        {
            NotificationCenter center(&store);
            NotificationRequest terminal = request(QStringLiteral("Session closed"), 1);
            terminal.terminal = true;
            terminal.correlationId = QStringLiteral("teardown-42");
            terminal.remoteSessionId = QStringLiteral("remote-session");
            QVERIFY(!center.publish(terminal).isEmpty());
            QVERIFY(center.publish(terminal).isEmpty());

            // Evict the visible terminal entry; its bounded replay tombstone remains.
            for (int index = 0; index < 100; ++index) {
                center.publish(request(QStringLiteral("Filler %1").arg(index), index + 2));
            }
            QCOMPARE(center.entries().size(), 100);
        }

        NotificationCenter restored(&store);
        NotificationRequest replay = request(QStringLiteral("Session closed replay"), 500);
        replay.terminal = true;
        replay.correlationId = QStringLiteral("teardown-42");
        QVERIFY(restored.publish(replay).isEmpty());
        QCOMPARE(restored.entries().size(), 100);

        QVERIFY(restored.clearHistory());
        QVERIFY(restored.entries().isEmpty());
        // Clearing visible history cannot weaken terminal replay protection.
        QVERIFY(restored.publish(replay).isEmpty());
        QCOMPARE(restored.entries().size(), 0);

        NotificationCenter afterClearRestart(&store);
        QVERIFY(afterClearRestart.publish(replay).isEmpty());
        QCOMPARE(afterClearRestart.entries().size(), 0);
    }

    void protocolTerminalCorrelationsAreScopedAndDeduplicated()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        HistoryStore store(temporary.filePath(QStringLiteral("history.json")));
        NotificationCenter center(&store);
        QSignalSpy toastSpy(&center, &NotificationCenter::toastRequested);

        const QString sharedId =
            QStringLiteral("11111111-2222-4333-8444-555566667788");
        NotificationRequest upload = request(QStringLiteral("Upload failed"), 1);
        upload.category = QStringLiteral("Upload");
        upload.correlationId = NotificationCorrelation::upload(sharedId);
        upload.terminal = true;

        NotificationRequest scene = request(QStringLiteral("Scene stopped"), 2);
        scene.category = QStringLiteral("Scene");
        scene.sceneRunId = sharedId;
        scene.correlationId = NotificationCorrelation::sceneRun(sharedId);
        scene.terminal = true;

        NotificationRequest teardown = request(QStringLiteral("Cleanup failed"), 3);
        teardown.category = QStringLiteral("Remote session cleanup");
        teardown.remoteSessionId = QStringLiteral("remote-session");
        teardown.correlationId = NotificationCorrelation::teardown(sharedId);
        teardown.terminal = true;

        QVERIFY(!center.publish(upload).isEmpty());
        QVERIFY(!center.publish(scene).isEmpty());
        QVERIFY(!center.publish(teardown).isEmpty());
        QCOMPARE(center.entries().size(), 3);
        QCOMPARE(toastSpy.count(), 3);

        // Retries may carry a different human-readable message, but the
        // protocol object's terminal identity remains the idempotency key.
        upload.message = QStringLiteral("Late upload replay");
        scene.message = QStringLiteral("Late stopped acknowledgement");
        teardown.message = QStringLiteral("Late teardown acknowledgement");
        QVERIFY(center.publish(upload).isEmpty());
        QVERIFY(center.publish(scene).isEmpty());
        QVERIFY(center.publish(teardown).isEmpty());
        QCOMPARE(center.entries().size(), 3);
        QCOMPARE(toastSpy.count(), 3);

        QCOMPARE(NotificationCorrelation::upload(QStringLiteral("  ")), QString());
        QCOMPARE(NotificationCorrelation::sceneRun(QStringLiteral("  ")), QString());
        QCOMPARE(NotificationCorrelation::teardown(QStringLiteral("  ")), QString());
    }

    void rejectsMalformedTerminalAndCorruptStore()
    {
        QTemporaryDir temporary;
        const QString path = temporary.filePath(QStringLiteral("history.json"));
        HistoryStore store(path);
        NotificationCenter center(&store);
        NotificationRequest malformed = request(QStringLiteral("Missing correlation"), 1);
        malformed.terminal = true;
        QVERIFY(center.publish(malformed).isEmpty());
        QVERIFY(center.entries().isEmpty());

        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("{truncated"), qint64(10));
        file.close();
        NotificationHistoryData data;
        QVERIFY(!store.load(&data));

        QVERIFY(!store.lastError().isEmpty());

        NotificationEntry fractional;
        fractional.id = QStringLiteral("d71b6a73-6ae9-41a2-9ca0-f4c159c48cbc");
        fractional.timestampMs = 1;
        fractional.message = QStringLiteral("Fractional timestamp");
        QJsonObject encodedEntry = fractional.toJson();
        encodedEntry.insert(QStringLiteral("timestampMs"), 1.5);
        const QJsonObject root{
            {QStringLiteral("schemaVersion"), HistoryStore::SchemaVersion},
            {QStringLiteral("entries"), QJsonArray{encodedEntry}},
            {QStringLiteral("terminalCorrelationIds"), QJsonArray{}}
        };
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray encoded = QJsonDocument(root).toJson(QJsonDocument::Compact);
        QCOMPARE(file.write(encoded), qint64(encoded.size()));
        file.close();
        QVERIFY(!store.load(&data));

        NotificationEntry terminalWithoutTombstone = fractional;
        terminalWithoutTombstone.id =
            QStringLiteral("4f98b156-1d6e-427a-ae1c-690eb357e456");
        terminalWithoutTombstone.terminal = true;
        terminalWithoutTombstone.correlationId = QStringLiteral("terminal-1");
        const QJsonObject missingTombstoneRoot{
            {QStringLiteral("schemaVersion"), HistoryStore::SchemaVersion},
            {QStringLiteral("entries"),
             QJsonArray{terminalWithoutTombstone.toJson()}},
            {QStringLiteral("terminalCorrelationIds"), QJsonArray{}}
        };
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray missingTombstone =
            QJsonDocument(missingTombstoneRoot).toJson(QJsonDocument::Compact);
        QCOMPARE(file.write(missingTombstone), qint64(missingTombstone.size()));
        file.close();
        QVERIFY(!store.load(&data));

        encodedEntry = fractional.toJson();
        encodedEntry.insert(QStringLiteral("id"), QStringLiteral("not-a-uuid"));
        encodedEntry.insert(QStringLiteral("read"), QStringLiteral("false"));
        const QJsonObject wrongTypesRoot{
            {QStringLiteral("schemaVersion"), HistoryStore::SchemaVersion},
            {QStringLiteral("entries"), QJsonArray{encodedEntry}},
            {QStringLiteral("terminalCorrelationIds"), QJsonArray{}}
        };
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray wrongTypes =
            QJsonDocument(wrongTypesRoot).toJson(QJsonDocument::Compact);
        QCOMPARE(file.write(wrongTypes), qint64(wrongTypes.size()));
        file.close();
        QVERIFY(!store.load(&data));
    }

    void peersSurvivePersistenceAndToastWithoutStoringProfiles()
    {
        QTemporaryDir temporary;
        HistoryStore store(temporary.filePath(QStringLiteral("history.json")));
        NotificationCenter center(&store);
        QSignalSpy toasts(&center, &NotificationCenter::toastRequested);
        auto notification = request(QStringLiteral("Media uploaded and loaded into RAM"), 1);
        notification.peers = {{QStringLiteral("endpoint-42"), QStringLiteral("Studio"),
                               2, QStringLiteral("To")}};
        notification.toastDurationMs = 4567;
        QVERIFY(!center.publish(notification).isEmpty());
        QCOMPARE(toasts.size(), 1);
        const NotificationEntry toasted = toasts.first().first().value<NotificationEntry>();
        QCOMPARE(toasted.peers.size(), 1);
        QCOMPARE(toasted.peers.first().endpointId, QStringLiteral("endpoint-42"));
        QCOMPARE(toasts.first().at(1).toInt(), 4567);

        NotificationCenter restored(&store);
        QCOMPARE(restored.entries().size(), 1);
        const auto entry = restored.entries().first();
        QCOMPARE(entry.message, notification.message);
        QCOMPARE(entry.peers.first().machineName, QStringLiteral("Studio"));
        QCOMPARE(entry.peers.first().instanceOrdinal, 2);
        QCOMPARE(entry.peers.first().role, QStringLiteral("To"));
        const QVariantMap peer = notificationPeersToVariant(entry.peers).first().toMap();
        QCOMPARE(peer.size(), 4);
        QVERIFY(!peer.contains(QStringLiteral("username")));
        QVERIFY(!peer.contains(QStringLiteral("profilePictureJpeg")));

        // Even unexpected optional fields in an input document cannot flow
        // through the durable peer codec into a later saved history file.
        QJsonObject input = entry.toJson();
        QJsonObject unsafePeer = input.value("peers").toArray().first().toObject();
        unsafePeer.insert("username", "Private username");
        unsafePeer.insert("profilePictureJpeg", "Private photo");
        unsafePeer.insert("profilePictureHash", "Private photo hash");
        input.insert("peers", QJsonArray{unsafePeer});
        NotificationEntry sanitized;
        QVERIFY(NotificationEntry::fromJson(input, &sanitized));
        const QByteArray serialized = QJsonDocument(sanitized.toJson()).toJson();
        QVERIFY(!serialized.contains("Private"));
        QVERIFY(!serialized.contains("username"));
        QVERIFY(!serialized.contains("profilePicture"));
    }

    void rejectsInvalidPeerReferencesBeforeCommit()
    {
        QTemporaryDir temporary;
        HistoryStore store(temporary.filePath(QStringLiteral("history.json")));
        NotificationCenter center(&store);
        auto notification = request(QStringLiteral("Rejected reference"), 1);
        notification.peers = {{QString(), QStringLiteral("Studio"), 1, QStringLiteral("To")}};
        QVERIFY(center.publish(notification).isEmpty());
        notification.peers.first().endpointId = QStringLiteral("endpoint");
        notification.peers.first().instanceOrdinal = 0;
        QVERIFY(center.publish(notification).isEmpty());
        notification.peers.first().instanceOrdinal = 1;
        notification.peers.first().role = QStringLiteral("Unsupported role");
        QVERIFY(center.publish(notification).isEmpty());
        QVERIFY(center.entries().isEmpty());
        QVERIFY(!QFile::exists(store.filePath()));
    }

    void neverDisplaysToastBeforeAtomicHistoryCommit()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        // A directory cannot be replaced by QSaveFile as a history file.
        HistoryStore store(temporary.path());
        NotificationCenter center(&store);
        QSignalSpy toastSpy(&center, &NotificationCenter::toastRequested);

        QVERIFY(center.publish(request(QStringLiteral("Must stay hidden"), 1)).isEmpty());
        QCOMPARE(toastSpy.count(), 0);
        QVERIFY(center.entries().isEmpty());
        QVERIFY(!center.lastError().isEmpty());
    }
};

QTEST_GUILESS_MAIN(NotificationCenterTest)
#include "tst_NotificationCenter.moc"
