#include <QtTest>

#include "backend/network/RemoteCacheStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

class RemoteCacheStoreTest final : public QObject {
    Q_OBJECT

private slots:
    void strictIdentifiersAndGenerationBinding();
    void teardownIsAtomicAndIdempotent();
    void teardownNeverReplacesDestination();
    void targetedAssetRemovalKeepsSessionOpenAndReplays();
    void targetedAssetRemovalNeverReplacesDestination();
    void startupRecoversAssetRemovalIntent_data();
    void startupRecoversAssetRemovalIntent();
    void startupRecoversDurableIntent();
    void startupRecoversRenameBeforeTombstone();
    void startupQuarantinesAbandonedLiveScope();
    void provisionalTeardownAdoptsOfficialIdentity();
    void uncommittedAssetRemovalIntentBlocksAdvertisement();
    void symlinkIsRefusedAndCleanupErrorPersists();
};

namespace {

const QString kSender = QString(64, QLatin1Char('a'));
const QString kSession = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString kTeardown = QStringLiteral("22222222-2222-4222-8222-222222222222");
const QString kOtherTeardown = QStringLiteral("33333333-3333-4333-8333-333333333333");
const QString kAsset = QString(64, QLatin1Char('b'));
const QString kUpload = QStringLiteral("upload_targeted");
const QString kDigest = QString(64, QLatin1Char('d'));
constexpr qint64 kValidatedImageBytes = 15;

RemoteCacheStore::Scope scope(quint64 generation = 7)
{
    return {kSender, kSession, generation};
}

RemoteCacheStore::AssetRemovalDescriptor assetRemoval(
    const QString& extension = QStringLiteral("png"))
{
    return {
        kTeardown,
        kUpload,
        kAsset,
        kDigest,
        kDigest,
        kValidatedImageBytes,
        kValidatedImageBytes,
        extension
    };
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size()
        && file.flush();
}

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

QString findOnlyJson(const QString& directory)
{
    const QStringList entries = QDir(directory).entryList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    return entries.size() == 1 ? QDir(directory).filePath(entries.constFirst()) : QString();
}

QString assetRemovalMetadataName(const QString& removalId)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        removalId.toUtf8(), QCryptographicHash::Sha256).toHex())
        + QStringLiteral(".json");
}

QString assetQuarantineEntry(
    const RemoteCacheStore::Scope& removalScope,
    const RemoteCacheStore::AssetRemovalDescriptor& descriptor)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto addPart = [&hash](const QByteArray& part) {
        hash.addData(part);
        hash.addData(QByteArrayView("\0", 1));
    };
    addPart(removalScope.senderEndpointId.toUtf8());
    addPart(removalScope.remoteSessionId.toUtf8());
    addPart(QString::number(removalScope.generation).toLatin1());
    addPart(descriptor.assetId.toUtf8());
    hash.addData(descriptor.removalId.toUtf8());
    return QStringLiteral("a-") + QString::fromLatin1(hash.result().toHex());
}

QJsonObject assetRemovalIntent(
    const RemoteCacheStore::Scope& removalScope,
    const RemoteCacheStore::AssetRemovalDescriptor& descriptor,
    const QString& phase,
    const QString& quarantineEntry)
{
    QJsonObject object{
        {QStringLiteral("schemaVersion"),
         RemoteCacheStore::MetadataSchemaVersion},
        {QStringLiteral("senderEndpointId"), removalScope.senderEndpointId},
        {QStringLiteral("remoteSessionId"), removalScope.remoteSessionId},
        {QStringLiteral("generation"),
         QString::number(removalScope.generation)},
        {QStringLiteral("removalId"), descriptor.removalId},
        {QStringLiteral("uploadId"), descriptor.uploadId},
        {QStringLiteral("assetId"), descriptor.assetId},
        {QStringLiteral("fileId"), descriptor.fileId},
        {QStringLiteral("sha256"), descriptor.sha256},
        {QStringLiteral("offset"), QString::number(descriptor.offset)},
        {QStringLiteral("size"), QString::number(descriptor.size)},
        {QStringLiteral("extension"), descriptor.extension},
        {QStringLiteral("quarantineEntry"), quarantineEntry},
        {QStringLiteral("phase"), phase},
        {QStringLiteral("createdAt"), QStringLiteral("2026-01-01T00:00:00.000Z")}
    };
    if (phase == QLatin1String("quarantined")) {
        object.insert(QStringLiteral("quarantinedBytes"),
                      QString::number(descriptor.size));
        object.insert(QStringLiteral("quarantinedAt"),
                      QStringLiteral("2026-01-01T00:00:01.000Z"));
    }
    return object;
}

bool writeObject(const QString& path, const QJsonObject& object)
{
    return writeBytes(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

void RemoteCacheStoreTest::strictIdentifiersAndGenerationBinding()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    RemoteCacheStore store(QDir(temporary.path()).filePath(QStringLiteral("Uploads")));
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));

    // SHA-256 base64url device IDs may legitimately begin with '-' or '_'.
    RemoteCacheStore::Scope base64UrlScope = scope();
    base64UrlScope.senderEndpointId = QStringLiteral("-base64urlEndpointId");
    base64UrlScope.remoteSessionId = QStringLiteral("session_base64url");
    QVERIFY2(store.ensureSession(base64UrlScope, &error), qPrintable(error));

    RemoteCacheStore::Scope invalid = scope();
    invalid.senderEndpointId = QStringLiteral("../device");
    QVERIFY(!store.ensureSession(invalid, &error));
    QCOMPARE(error, QStringLiteral("invalid_sender_endpoint_id"));

    invalid = scope();
    invalid.remoteSessionId = QStringLiteral("../../escape");
    QVERIFY(!store.ensureSession(invalid, &error));
    QCOMPARE(error, QStringLiteral("invalid_remote_session_id"));

    invalid = scope(0);
    QVERIFY(!store.ensureSession(invalid, &error));
    QCOMPARE(error, QStringLiteral("invalid_remote_session_generation"));

    QVERIFY2(store.ensureSession(scope(), &error), qPrintable(error));
    const QString assetPath = store.assetPath(scope(), kAsset,
                                              RemoteCacheStore::AssetArea::Staging,
                                              QStringLiteral("MP4"), &error);
    QVERIFY2(!assetPath.isEmpty(), qPrintable(error));
    QVERIFY(QFileInfo(assetPath).absoluteFilePath().startsWith(
        QFileInfo(store.rootPath()).absoluteFilePath() + QDir::separator()));
    QVERIFY(assetPath.endsWith(QStringLiteral(".mp4")));
    QVERIFY(store.acceptsCommands(scope()));

    const QString firstStagingPath = store.stagingAssetPath(
        scope(), QStringLiteral("upload_first"), kAsset,
        QStringLiteral("mp4"), &error);
    const QString retryStagingPath = store.stagingAssetPath(
        scope(), QStringLiteral("upload_retry"), kAsset,
        QStringLiteral("mp4"), &error);
    QVERIFY2(!firstStagingPath.isEmpty(), qPrintable(error));
    QVERIFY2(!retryStagingPath.isEmpty(), qPrintable(error));
    QVERIFY(firstStagingPath != retryStagingPath);
    const QString stagingRoot = QDir(store.rootPath()).filePath(
        kSender + QLatin1Char('/') + kSession + QStringLiteral("/staging"));
    QCOMPARE(QFileInfo(QFileInfo(firstStagingPath).absolutePath()).absolutePath(),
             stagingRoot);
    QVERIFY(QFileInfo(firstStagingPath).dir().dirName().size() <= 24);
    QVERIFY(QFileInfo(firstStagingPath).completeBaseName().size() <= 24);
    QVERIFY(!firstStagingPath.contains(QStringLiteral("upload_first")));
    QVERIFY(!firstStagingPath.contains(kAsset));
    QVERIFY(store.stagingAssetPath(
        scope(), QStringLiteral("../upload"), kAsset,
        QStringLiteral("mp4"), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("invalid_upload_id"));

    const QString validatedPath = store.assetPath(
        scope(), kAsset, RemoteCacheStore::AssetArea::Validated,
        QStringLiteral("mp4"), &error);
    QVERIFY2(!validatedPath.isEmpty(), qPrintable(error));
    QVERIFY(QFileInfo(validatedPath).completeBaseName().size() <= 24);
    QVERIFY(!validatedPath.contains(kAsset));

    QVERIFY(store.assetPath(scope(), QStringLiteral("../asset"),
                            RemoteCacheStore::AssetArea::Validated).isEmpty());
    QVERIFY(store.assetPath(scope(), kAsset,
                            RemoteCacheStore::AssetArea::Validated,
                            QStringLiteral("../png")).isEmpty());

    QVERIFY(!store.ensureSession(scope(6), &error));
    QCOMPARE(error, QStringLiteral("remote_session_generation_conflict"));

    QVERIFY2(store.ensureSession(scope(8), &error), qPrintable(error));
    QVERIFY(store.acceptsCommands(scope(8)));
    QVERIFY(!store.acceptsCommands(scope()));
    QVERIFY2(store.rebindSessionGeneration(scope(8), 9, &error), qPrintable(error));
    QVERIFY(store.acceptsCommands(scope(9)));
    QVERIFY(!store.rebindSessionGeneration(scope(9), 9, &error));
    QCOMPARE(error, QStringLiteral("invalid_generation_transition"));

    QVERIFY(!store.beginTeardown(scope(9), QStringLiteral("not-a-uuid"), &error));
    QCOMPARE(error, QStringLiteral("invalid_teardown_id"));

#ifndef Q_OS_WIN
    const QFileDevice::Permissions permissions = QFileInfo(store.rootPath()).permissions();
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
    QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
    QVERIFY(permissions.testFlag(QFileDevice::ExeOwner));
    QVERIFY(!permissions.testFlag(QFileDevice::ReadGroup));
    QVERIFY(!permissions.testFlag(QFileDevice::ReadOther));
#endif
}

void RemoteCacheStoreTest::teardownIsAtomicAndIdempotent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    RemoteCacheStore store(QDir(temporary.path()).filePath(QStringLiteral("Uploads")));
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));
    QVERIFY(store.ensureSession(scope(), &error));
    const QString assetPath = store.assetPath(scope(), kAsset,
                                              RemoteCacheStore::AssetArea::Validated,
                                              QStringLiteral("mp4"), &error);
    QVERIFY(writeBytes(assetPath, QByteArrayLiteral("media")));
    const RemoteCacheStore::Scope teardownScope = scope(8);

    // A session resume can advance the generation without performing another
    // upload; the authenticated terminal envelope must still purge this scope.
    QVERIFY(store.beginTeardown(teardownScope, kTeardown, &error));
    QVERIFY(store.beginTeardown(teardownScope, kTeardown, &error));
    QVERIFY(!store.beginTeardown(teardownScope, kOtherTeardown, &error));
    QCOMPARE(error, QStringLiteral("terminal_teardown_conflict"));
    QVERIFY(!store.acceptsCommands(teardownScope));
    QVERIFY(store.assetPath(teardownScope, kAsset,
                            RemoteCacheStore::AssetArea::Validated).isEmpty());

    QSignalSpy logicalSpy(&store, &RemoteCacheStore::logicalCommitCompleted);
    QSignalSpy physicalSpy(&store, &RemoteCacheStore::physicalCleanupCompleted);
    const RemoteCacheStore::CommitResult committed = store.commitTeardown(teardownScope, kTeardown);
    QCOMPARE(committed.outcome, RemoteCacheStore::CommitOutcome::Committed);
    QVERIFY(committed.acknowledgementSafe());
    QVERIFY(committed.quarantinedBytes >= 5);
    QCOMPARE(logicalSpy.size(), 1);
    QVERIFY(!QFileInfo::exists(assetPath));

    QTRY_COMPARE_WITH_TIMEOUT(store.state(teardownScope), RemoteCacheStore::SessionState::Closed, 5000);
    QCOMPARE(physicalSpy.size(), 1);
    const RemoteCacheStore::CommitResult replay = store.commitTeardown(teardownScope, kTeardown);
    QCOMPARE(replay.outcome, RemoteCacheStore::CommitOutcome::AlreadyCommitted);
    QVERIFY(replay.acknowledgementSafe());
    QCOMPARE(replay.teardownId, kTeardown);

    const RemoteCacheStore::CommitResult conflict = store.commitTeardown(teardownScope, kOtherTeardown);
    QCOMPARE(conflict.outcome, RemoteCacheStore::CommitOutcome::Conflict);
    QVERIFY(!conflict.acknowledgementSafe());
    QCOMPARE(conflict.teardownId, kTeardown);
}

void RemoteCacheStoreTest::teardownNeverReplacesDestination()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root =
        QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    RemoteCacheStore store(root);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));
    QVERIFY2(store.ensureSession(scope(), &error), qPrintable(error));
    const QString assetPath = store.assetPath(
        scope(), kAsset, RemoteCacheStore::AssetArea::Validated,
        QStringLiteral("mp4"), &error);
    QVERIFY2(!assetPath.isEmpty(), qPrintable(error));
    QVERIFY(writeBytes(assetPath, QByteArrayLiteral("live-session-asset")));
    QVERIFY2(store.beginTeardown(scope(), kTeardown, &error),
             qPrintable(error));

    const QString intentPath = findOnlyJson(
        QDir(root).filePath(QStringLiteral(".remote-cache-state/intents")));
    QVERIFY(!intentPath.isEmpty());
    const QString quarantineEntry =
        readObject(intentPath).value(QStringLiteral("quarantineEntry")).toString();
    QVERIFY(!quarantineEntry.isEmpty());
    const QString destination = QDir(root).filePath(
        QStringLiteral(".quarantine/") + quarantineEntry);
    QVERIFY(QDir().mkpath(destination));
    const QString sentinel =
        QDir(destination).filePath(QStringLiteral("must-survive.bin"));
    QVERIFY(writeBytes(sentinel, QByteArrayLiteral("must-survive")));

    const RemoteCacheStore::CommitResult rejected =
        store.commitTeardown(scope(), kTeardown);
    QCOMPARE(rejected.outcome, RemoteCacheStore::CommitOutcome::CleanupError);
    QCOMPARE(rejected.errorCode, QStringLiteral("ambiguous_cache_state"));
    QVERIFY(QFileInfo::exists(assetPath));
    QFile live(assetPath);
    QVERIFY(live.open(QIODevice::ReadOnly));
    QCOMPARE(live.readAll(), QByteArrayLiteral("live-session-asset"));
    QFile destinationFile(sentinel);
    QVERIFY(destinationFile.open(QIODevice::ReadOnly));
    QCOMPARE(destinationFile.readAll(), QByteArrayLiteral("must-survive"));
    QCOMPARE(store.state(scope()), RemoteCacheStore::SessionState::CleanupError);
}

void RemoteCacheStoreTest::targetedAssetRemovalKeepsSessionOpenAndReplays()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    RemoteCacheStore store(QDir(temporary.path()).filePath(QStringLiteral("Uploads")));
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));
    QVERIFY2(store.ensureSession(scope(), &error), qPrintable(error));

    const QString assetPath = store.assetPath(
        scope(), kAsset, RemoteCacheStore::AssetArea::Validated,
        QStringLiteral("png"), &error);
    QVERIFY2(!assetPath.isEmpty(), qPrintable(error));
    QVERIFY(writeBytes(assetPath, QByteArrayLiteral("validated-image")));

    const RemoteCacheStore::AssetRemovalDescriptor removal = assetRemoval();
    const RemoteCacheStore::AssetRemovalResult committed =
        store.removeValidatedAsset(scope(), removal);
    QCOMPARE(committed.outcome, RemoteCacheStore::CommitOutcome::Committed);
    QVERIFY(committed.acknowledgementSafe());
    QCOMPARE(committed.quarantinedBytes, kValidatedImageBytes);
    QVERIFY(!QFileInfo::exists(assetPath));
    QVERIFY(store.acceptsCommands(scope()));

    const QString removalTombstone = findOnlyJson(
        QDir(store.rootPath()).filePath(
            QStringLiteral(".remote-cache-state/asset-removals")));
    QVERIFY(!removalTombstone.isEmpty());
    const QJsonObject persisted = readObject(removalTombstone);
    QCOMPARE(persisted.value(QStringLiteral("senderEndpointId")).toString(),
             scope().senderEndpointId);
    QCOMPARE(persisted.value(QStringLiteral("remoteSessionId")).toString(),
             scope().remoteSessionId);
    QCOMPARE(persisted.value(QStringLiteral("generation")).toString(),
             QString::number(scope().generation));
    QCOMPARE(persisted.value(QStringLiteral("removalId")).toString(),
             removal.removalId);
    QCOMPARE(persisted.value(QStringLiteral("uploadId")).toString(),
             removal.uploadId);
    QCOMPARE(persisted.value(QStringLiteral("assetId")).toString(),
             removal.assetId);
    QCOMPARE(persisted.value(QStringLiteral("fileId")).toString(),
             removal.fileId);
    QCOMPARE(persisted.value(QStringLiteral("sha256")).toString(),
             removal.sha256);
    QCOMPARE(persisted.value(QStringLiteral("offset")).toString(),
             QString::number(removal.offset));
    QCOMPARE(persisted.value(QStringLiteral("size")).toString(),
             QString::number(removal.size));
    QCOMPARE(persisted.value(QStringLiteral("extension")).toString(),
             QStringLiteral("png"));

    RemoteCacheStore::AssetRemovalDescriptor uppercaseReplay = removal;
    uppercaseReplay.extension = QStringLiteral("PNG");
    const RemoteCacheStore::AssetRemovalResult replay =
        store.removeValidatedAsset(scope(), uppercaseReplay);
    QCOMPARE(replay.outcome,
             RemoteCacheStore::CommitOutcome::AlreadyCommitted);
    QVERIFY(replay.acknowledgementSafe());
    QCOMPARE(replay.quarantinedBytes, committed.quarantinedBytes);
    QVERIFY(store.acceptsCommands(scope()));

    RemoteCacheStore::Scope resumedScope = scope();
    resumedScope.generation = scope().generation + 1;
    QVERIFY2(store.rebindSessionGeneration(scope(), resumedScope.generation,
                                           &error), qPrintable(error));
    const RemoteCacheStore::AssetRemovalResult resumedReplay =
        store.removeValidatedAsset(resumedScope, removal);
    QCOMPARE(resumedReplay.outcome,
             RemoteCacheStore::CommitOutcome::AlreadyCommitted);
    const RemoteCacheStore::AssetRemovalResult staleReplay =
        store.removeValidatedAsset(scope(), removal);
    QCOMPARE(staleReplay.outcome, RemoteCacheStore::CommitOutcome::Conflict);

    RemoteCacheStore::AssetRemovalDescriptor conflictingUpload = removal;
    conflictingUpload.uploadId = QStringLiteral("another_upload");
    const RemoteCacheStore::AssetRemovalResult uploadConflict =
        store.removeValidatedAsset(resumedScope, conflictingUpload);
    QCOMPARE(uploadConflict.outcome, RemoteCacheStore::CommitOutcome::Conflict);

    RemoteCacheStore::AssetRemovalDescriptor conflictingDigest = removal;
    conflictingDigest.fileId = QString(64, QLatin1Char('e'));
    conflictingDigest.sha256 = conflictingDigest.fileId;
    const RemoteCacheStore::AssetRemovalResult digestConflict =
        store.removeValidatedAsset(resumedScope, conflictingDigest);
    QCOMPARE(digestConflict.outcome, RemoteCacheStore::CommitOutcome::Conflict);

    RemoteCacheStore::AssetRemovalDescriptor conflictingRange = removal;
    ++conflictingRange.offset;
    ++conflictingRange.size;
    const RemoteCacheStore::AssetRemovalResult rangeConflict =
        store.removeValidatedAsset(resumedScope, conflictingRange);
    QCOMPARE(rangeConflict.outcome, RemoteCacheStore::CommitOutcome::Conflict);

    RemoteCacheStore::AssetRemovalDescriptor conflictingExtension = removal;
    conflictingExtension.extension = QStringLiteral("jpg");
    const RemoteCacheStore::AssetRemovalResult extensionConflict =
        store.removeValidatedAsset(resumedScope, conflictingExtension);
    QCOMPARE(extensionConflict.outcome,
             RemoteCacheStore::CommitOutcome::Conflict);

    RemoteCacheStore::AssetRemovalDescriptor conflictingAsset = removal;
    conflictingAsset.assetId = QString(64, QLatin1Char('c'));
    const RemoteCacheStore::AssetRemovalResult conflict =
        store.removeValidatedAsset(resumedScope, conflictingAsset);
    QCOMPARE(conflict.outcome, RemoteCacheStore::CommitOutcome::Conflict);
    QVERIFY(!conflict.acknowledgementSafe());

    RemoteCacheStore::Scope conflictingScope = resumedScope;
    conflictingScope.senderEndpointId = QString(64, QLatin1Char('f'));
    const RemoteCacheStore::AssetRemovalResult scopeConflict =
        store.removeValidatedAsset(conflictingScope, removal);
    QCOMPARE(scopeConflict.outcome, RemoteCacheStore::CommitOutcome::Conflict);

    // Targeted cleanup must not poison the surrounding session; it can still
    // be closed later by the normal all-assets quarantine transaction.
    QVERIFY2(store.beginTeardown(resumedScope, kOtherTeardown, &error),
             qPrintable(error));
    const RemoteCacheStore::CommitResult closed =
        store.commitTeardown(resumedScope, kOtherTeardown);
    QVERIFY(closed.acknowledgementSafe());
}

void RemoteCacheStoreTest::targetedAssetRemovalNeverReplacesDestination()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root =
        QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    RemoteCacheStore store(root);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));
    QVERIFY2(store.ensureSession(scope(), &error), qPrintable(error));

    const RemoteCacheStore::AssetRemovalDescriptor removal = assetRemoval();
    const QString assetPath = store.assetPath(
        scope(), removal.assetId, RemoteCacheStore::AssetArea::Validated,
        removal.extension, &error);
    QVERIFY2(!assetPath.isEmpty(), qPrintable(error));
    QVERIFY(writeBytes(assetPath, QByteArrayLiteral("validated-image")));

    const QString quarantinePath = QDir(root).filePath(
        QStringLiteral(".quarantine/")
        + assetQuarantineEntry(scope(), removal));
    QVERIFY(writeBytes(quarantinePath, QByteArrayLiteral("must-survive")));

    const RemoteCacheStore::AssetRemovalResult rejected =
        store.removeValidatedAsset(scope(), removal);
    QCOMPARE(rejected.outcome, RemoteCacheStore::CommitOutcome::CleanupError);
    QCOMPARE(rejected.errorCode,
             QStringLiteral("asset_quarantine_destination_exists"));
    QVERIFY(QFileInfo::exists(assetPath));
    QFile live(assetPath);
    QVERIFY(live.open(QIODevice::ReadOnly));
    QCOMPARE(live.readAll(), QByteArrayLiteral("validated-image"));
    QFile destination(quarantinePath);
    QVERIFY(destination.open(QIODevice::ReadOnly));
    QCOMPARE(destination.readAll(), QByteArrayLiteral("must-survive"));
    QVERIFY(QDir(QDir(root).filePath(
                       QStringLiteral(".remote-cache-state/asset-removal-intents")))
                .entryList({QStringLiteral("*.json")}, QDir::Files)
                .isEmpty());
}

void RemoteCacheStoreTest::startupRecoversAssetRemovalIntent_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<bool>("moveBeforeRestart");

    QTest::newRow("intent-before-rename") << QStringLiteral("prepared")
                                           << false;
    QTest::newRow("rename-before-phase") << QStringLiteral("prepared")
                                         << true;
    QTest::newRow("phase-before-tombstone") << QStringLiteral("quarantined")
                                            << true;
}

void RemoteCacheStoreTest::startupRecoversAssetRemovalIntent()
{
    QFETCH(QString, phase);
    QFETCH(bool, moveBeforeRestart);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root =
        QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    const RemoteCacheStore::AssetRemovalDescriptor removal = assetRemoval();
    const QString quarantineEntry = assetQuarantineEntry(scope(), removal);
    const QString quarantinePath = QDir(root).filePath(
        QStringLiteral(".quarantine/") + quarantineEntry);
    const QString intentPath = QDir(root).filePath(
        QStringLiteral(".remote-cache-state/asset-removal-intents/")
        + assetRemovalMetadataName(removal.removalId));
    const QString tombstonePath = QDir(root).filePath(
        QStringLiteral(".remote-cache-state/asset-removals/")
        + assetRemovalMetadataName(removal.removalId));
    QString liveAssetPath;

    {
        RemoteCacheStore first(root);
        QString error;
        QVERIFY2(first.initialize(&error), qPrintable(error));
        QVERIFY2(first.ensureSession(scope(), &error), qPrintable(error));
        liveAssetPath = first.assetPath(
            scope(), removal.assetId, RemoteCacheStore::AssetArea::Validated,
            removal.extension, &error);
        QVERIFY2(!liveAssetPath.isEmpty(), qPrintable(error));
        QVERIFY(writeBytes(liveAssetPath,
                           QByteArrayLiteral("validated-image")));
        QVERIFY(writeObject(intentPath,
                            assetRemovalIntent(scope(), removal, phase,
                                               quarantineEntry)));
        if (moveBeforeRestart) {
            QVERIFY(QFile::rename(liveAssetPath, quarantinePath));
        }
    }

    RemoteCacheStore recovered(root);
    QString error;
    QVERIFY2(recovered.initialize(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(liveAssetPath));
    QVERIFY(!QFileInfo::exists(intentPath));
    QVERIFY(QFileInfo::exists(tombstonePath));

    const QJsonObject tombstone = readObject(tombstonePath);
    QCOMPARE(tombstone.value(QStringLiteral("senderEndpointId")).toString(),
             scope().senderEndpointId);
    QCOMPARE(tombstone.value(QStringLiteral("remoteSessionId")).toString(),
             scope().remoteSessionId);
    QCOMPARE(tombstone.value(QStringLiteral("generation")).toString(),
             QString::number(scope().generation));
    QCOMPARE(tombstone.value(QStringLiteral("removalId")).toString(),
             removal.removalId);
    QCOMPARE(tombstone.value(QStringLiteral("uploadId")).toString(),
             removal.uploadId);
    QCOMPARE(tombstone.value(QStringLiteral("assetId")).toString(),
             removal.assetId);
    QCOMPARE(tombstone.value(QStringLiteral("fileId")).toString(),
             removal.fileId);
    QCOMPARE(tombstone.value(QStringLiteral("sha256")).toString(),
             removal.sha256);
    QCOMPARE(tombstone.value(QStringLiteral("offset")).toString(),
             QString::number(removal.offset));
    QCOMPARE(tombstone.value(QStringLiteral("size")).toString(),
             QString::number(removal.size));
    QCOMPARE(tombstone.value(QStringLiteral("extension")).toString(),
             removal.extension);
    QCOMPARE(tombstone.value(QStringLiteral("quarantineEntry")).toString(),
             quarantineEntry);
    QCOMPARE(tombstone.value(QStringLiteral("quarantinedBytes")).toString(),
             QString::number(removal.size));

    const RemoteCacheStore::AssetRemovalResult replay =
        recovered.removeValidatedAsset(scope(), removal);
    QCOMPARE(replay.outcome,
             RemoteCacheStore::CommitOutcome::AlreadyCommitted);
    QCOMPARE(replay.quarantinedBytes, removal.size);
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(quarantinePath), 5000);
}

void RemoteCacheStoreTest::startupRecoversDurableIntent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    {
        RemoteCacheStore first(root);
        QString error;
        QVERIFY(first.initialize(&error));
        QVERIFY(first.ensureSession(scope(), &error));
        const QString asset = first.assetPath(scope(), kAsset,
                                              RemoteCacheStore::AssetArea::Staging,
                                              {}, &error);
        QVERIFY(writeBytes(asset, QByteArrayLiteral("partial")));
        QVERIFY(first.beginTeardown(scope(), kTeardown, &error));
        QCOMPARE(first.state(scope()), RemoteCacheStore::SessionState::Terminating);
    }

    RemoteCacheStore recovered(root);
    QString error;
    QVERIFY2(recovered.initialize(&error), qPrintable(error));
    const auto tombstone = recovered.tombstone(scope());
    QVERIFY(tombstone.has_value());
    QCOMPARE(tombstone->teardownId, kTeardown);
    QVERIFY(!recovered.acceptsCommands(scope()));
    QTRY_COMPARE_WITH_TIMEOUT(recovered.state(scope()), RemoteCacheStore::SessionState::Closed, 5000);
}

void RemoteCacheStoreTest::startupRecoversRenameBeforeTombstone()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    QString liveScope;
    QString quarantineEntry;
    {
        RemoteCacheStore first(root);
        QString error;
        QVERIFY(first.initialize(&error));
        QVERIFY(first.ensureSession(scope(), &error));
        const QString asset = first.assetPath(scope(), kAsset,
                                              RemoteCacheStore::AssetArea::Validated,
                                              {}, &error);
        QVERIFY(writeBytes(asset, QByteArrayLiteral("validated")));
        QVERIFY(first.beginTeardown(scope(), kTeardown, &error));

        const QString intentFile = findOnlyJson(
            QDir(root).filePath(QStringLiteral(".remote-cache-state/intents")));
        QVERIFY(!intentFile.isEmpty());
        quarantineEntry = readObject(intentFile)
                              .value(QStringLiteral("quarantineEntry"))
                              .toString();
        QVERIFY(!quarantineEntry.isEmpty());
        liveScope = QDir(root).filePath(kSender + QLatin1Char('/') + kSession);
        const QString destination = QDir(root).filePath(
            QStringLiteral(".quarantine/") + quarantineEntry);
        QVERIFY(QDir().rename(liveScope, destination));
    }

    RemoteCacheStore recovered(root);
    QString error;
    QVERIFY2(recovered.initialize(&error), qPrintable(error));
    const auto tombstone = recovered.tombstone(scope());
    QVERIFY(tombstone.has_value());
    QCOMPARE(tombstone->teardownId, kTeardown);
    QVERIFY(!QFileInfo::exists(liveScope));
    QTRY_COMPARE_WITH_TIMEOUT(recovered.state(scope()), RemoteCacheStore::SessionState::Closed, 5000);
}

void RemoteCacheStoreTest::startupQuarantinesAbandonedLiveScope()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    QString assetPath;
    {
        RemoteCacheStore first(root);
        QString error;
        QVERIFY(first.initialize(&error));
        QVERIFY(first.ensureSession(scope(), &error));
        assetPath = first.assetPath(scope(), kAsset,
                                    RemoteCacheStore::AssetArea::Staging,
                                    {}, &error);
        QVERIFY(writeBytes(assetPath, QByteArrayLiteral("orphan")));
    }

    RemoteCacheStore restarted(root);
    QString error;
    QVERIFY2(restarted.initialize(&error), qPrintable(error));
    const auto recovered = restarted.tombstone(scope());
    QVERIFY(recovered.has_value());
    QVERIFY(recovered->provisional);
    QVERIFY(!QFileInfo::exists(assetPath));
    QVERIFY(!restarted.acceptsCommands(scope()));
    QTRY_COMPARE_WITH_TIMEOUT(restarted.state(scope()), RemoteCacheStore::SessionState::Closed, 5000);
}

void RemoteCacheStoreTest::provisionalTeardownAdoptsOfficialIdentity()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    RemoteCacheStore store(QDir(temporary.path()).filePath(QStringLiteral("Uploads")));
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));
    QVERIFY2(store.ensureSession(scope(), &error), qPrintable(error));

    QList<RemoteCacheStore::Scope> live = store.liveScopes(&error);
    QCOMPARE(live.size(), 1);
    QVERIFY(live.constFirst() == scope());
    QVERIFY(!store.beginProvisionalTeardown(scope(), kTeardown,
                                            QStringLiteral("Not safe"), &error));
    QCOMPARE(error, QStringLiteral("invalid_teardown_reason"));

    QVERIFY2(store.beginProvisionalTeardown(scope(), kTeardown,
                                            QStringLiteral("lease_expired"),
                                            &error), qPrintable(error));
    const RemoteCacheStore::CommitResult local =
        store.commitTeardown(scope(), kTeardown);
    QCOMPARE(local.outcome, RemoteCacheStore::CommitOutcome::Committed);
    const auto provisional = store.tombstone(scope());
    QVERIFY(provisional.has_value());
    QVERIFY(provisional->provisional);
    QCOMPARE(provisional->teardownId, kTeardown);
    QCOMPARE(provisional->reasonCode, QStringLiteral("lease_expired"));
    QVERIFY(store.liveScopes(&error).isEmpty());

    // The authenticated server transaction reconciles the locally generated
    // id without restoring the already-terminal namespace.
    QVERIFY2(store.beginTeardown(scope(), kOtherTeardown, &error),
             qPrintable(error));
    const auto adopted = store.tombstone(scope());
    QVERIFY(adopted.has_value());
    QVERIFY(!adopted->provisional);
    QCOMPARE(adopted->teardownId, kOtherTeardown);

    const RemoteCacheStore::CommitResult replay =
        store.commitTeardown(scope(), kOtherTeardown);
    QCOMPARE(replay.outcome, RemoteCacheStore::CommitOutcome::AlreadyCommitted);
    QVERIFY(replay.acknowledgementSafe());
    const RemoteCacheStore::CommitResult oldLocalReplay =
        store.commitTeardown(scope(), kTeardown);
    QCOMPARE(oldLocalReplay.outcome, RemoteCacheStore::CommitOutcome::Conflict);
    QCOMPARE(oldLocalReplay.teardownId, kOtherTeardown);
}

void RemoteCacheStoreTest::uncommittedAssetRemovalIntentBlocksAdvertisement()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root =
        QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    RemoteCacheStore store(root);
    QString error;
    QVERIFY2(store.initialize(&error), qPrintable(error));

    const RemoteCacheStore::AssetRemovalDescriptor removal = assetRemoval();
    const QString intentPath = QDir(root).filePath(
        QStringLiteral(".remote-cache-state/asset-removal-intents/")
        + assetRemovalMetadataName(removal.removalId));
    QVERIFY(writeObject(
        intentPath,
        assetRemovalIntent(scope(), removal, QStringLiteral("quarantined"),
                           assetQuarantineEntry(scope(), removal))));

    QVERIFY(!store.receiverAdvertisementSafe(&error));
    QCOMPARE(error, QStringLiteral("logical_cleanup_not_committed"));

    // A malformed object cannot be mistaken for an already committed intent
    // merely because its hashed filename has the expected shape.
    QJsonObject malformed = readObject(intentPath);
    malformed.insert(QStringLiteral("phase"), QStringLiteral("deleted"));
    QVERIFY(writeObject(intentPath, malformed));
    QVERIFY(!store.receiverAdvertisementSafe(&error));
    QCOMPARE(error, QStringLiteral("asset_removal_intent_invalid"));
}

void RemoteCacheStoreTest::symlinkIsRefusedAndCleanupErrorPersists()
{
#ifdef Q_OS_WIN
    QSKIP("Creating a symlink requires privileges not guaranteed on Windows CI");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    const QString outside = QDir(temporary.path()).filePath(QStringLiteral("outside.bin"));
    QVERIFY(writeBytes(outside, QByteArrayLiteral("must-survive")));

    {
        RemoteCacheStore store(root);
        QString error;
        QVERIFY(store.initialize(&error));
        QVERIFY(store.ensureSession(scope(), &error));
        const QString linkPath = store.assetPath(scope(), kAsset,
                                                 RemoteCacheStore::AssetArea::Staging,
                                                 {}, &error);
        QVERIFY(QFile::link(outside, linkPath));
        QVERIFY(QFileInfo(linkPath).isSymLink());
        QVERIFY(store.beginTeardown(scope(), kTeardown, &error));
        const RemoteCacheStore::CommitResult result = store.commitTeardown(scope(), kTeardown);
        QCOMPARE(result.outcome, RemoteCacheStore::CommitOutcome::CleanupError);
        QCOMPARE(result.errorCode, QStringLiteral("symlink_refused"));
        QCOMPARE(store.state(scope()), RemoteCacheStore::SessionState::CleanupError);
        QVERIFY(!store.acceptsCommands(scope()));
        QVERIFY(store.hasCleanupErrors());
        QVERIFY(!store.receiverAdvertisementSafe(&error));
        QCOMPARE(error, QStringLiteral("logical_cleanup_not_committed"));
    }

    RemoteCacheStore recovered(root);
    QString error;
    QVERIFY2(recovered.initialize(&error), qPrintable(error));
    QCOMPARE(recovered.state(scope()), RemoteCacheStore::SessionState::CleanupError);
    QVERIFY(recovered.hasCleanupErrors());
    QVERIFY(!recovered.receiverAdvertisementSafe(&error));
    QCOMPARE(error, QStringLiteral("logical_cleanup_not_committed"));
    QVERIFY(QFileInfo::exists(outside));
    QFile outsideFile(outside);
    QVERIFY(outsideFile.open(QIODevice::ReadOnly));
    QCOMPARE(outsideFile.readAll(), QByteArrayLiteral("must-survive"));
#endif
}

QTEST_GUILESS_MAIN(RemoteCacheStoreTest)
#include "tst_RemoteCacheStore.moc"
