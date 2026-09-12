#include <QtTest>

#include "backend/files/FileManager.h"
#include "backend/files/LocalFileRepository.h"
#include "backend/network/RemoteFileTracker.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"

#include <QDir>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QUuid>
#include <algorithm>

namespace {

QByteArray oneFrameH264Mp4()
{
    // 16x16, one-frame H.264/MP4 generated with FFmpeg.  Keeping this tiny
    // fixture inline lets the receiver-path test exercise the real decoder
    // without requiring an external process or a multi-megabyte upload.
    return QByteArray::fromBase64(QByteArrayLiteral(
        "AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAMObW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAD6AAAA+gAAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAjl0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAABAAAAAAAAA+gAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAABAAAAAQAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAPoAAAAAAABAAAAAAGxbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAABAAAAAQABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRlb0hhbmRsZXIAAAABXG1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAARxzdGJsAAAAuHN0c2QAAAAAAAAAAQAAAKhhdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAABAAEABIAAAASAAAAAAAAAABFUxhdmM2Mi4xMS4xMDAgbGlieDI2NAAAAAAAAAAAAAAAGP//AAAALmF2Y0MBQsAK/+EAFmdCwArZHsBEAAADAAQAAAMACDxImSABAAVoy4PLIAAAABBwYXNwAAAAAQAAAAEAAAAUYnRydAAAAAAAABRgAAAAAAAAABhzdHRzAAAAAAAAAAEAAAABAABAAAAAABxzdHNjAAAAAAAAAAEAAAABAAAAAQAAAAEAAAAUc3RzegAAAAAAAAKMAAAAAQAAABRzdGNvAAAAAAAAAAEAAAM+AAAAYXVkdGEAAABZbWV0YQAAAAAAAAAhaGRscgAAAAAAAAAAbWRpcmFwcGwAAAAAAAAAAAAAAAAsaWxzdAAAACSpdG9vAAAAHGRhdGEAAAABAAAAAExhdmY2Mi4zLjEwMAAAAAhmcmVlAAAClG1kYXQAAAJwBgX//2zcRem95tlIt5Ys2CDZI+7veDI2NCAtIGNvcmUgMTY1IHIzMjIyIGIzNTYwNWEgLSBILjI2NC9NUEVHLTQgQVZDIGNvZGVjIC0gQ29weWxlZnQgMjAwMy0yMDI1IC0gaHR0cDovL3d3dy52aWRlb2xhbi5vcmcveDI2NC5odG1sIC0gb3B0aW9uczogY2FiYWM9MCByZWY9MyBkZWJsb2NrPTE6MDowIGFuYWx5c2U9MHgxOjB4MTExIG1lPWhleCBzdWJtZT03IHBzeT0xIHBzeV9yZD0xLjAwOjAuMDAgbWl4ZWRfcmVmPTEgbWVfcmFuZ2U9MTYgY2hyb21hX21lPTEgdHJlbGxpcz0xIDh4OGRjdD0wIGNxbT0wIGRlYWR6b25lPTIxLDExIGZhc3RfcHNraXA9MSBjaHJvbWFfcXBfb2Zmc2V0PS0yIHRocmVhZHM9MSBsb29rYWhlYWRfdGhyZWFkcz0xIHNsaWNlZF90aHJlYWRzPTAgbnI9MCBkZWNpbWF0ZT0xIGludGVybGFjZWQ9MCBibHVyYXlfY29tcGF0PTAgY29uc3RyYWluZWRfaW50cmE9MCBiZnJhbWVzPTAgd2VpZ2h0cD0wIGtleWludD0yNTAga2V5aW50X21pbj0xIHNjZW5lY3V0PTQwIGludHJhX3JlZnJlc2g9MCByY19sb29rYWhlYWQ9NDAgcmM9Y3JmIG1idHJlZT0xIGNyZj0yMy4wIHFjb21wPTAuNjAgcXBtaW49MCBxcG1heD02OSBxcHN0ZXA9NCBpcF9yYXRpbz0xLjQwIGFxPTE6MS4wMACAAAAAFGWIhAV8RigAC4zHAAE6GOAANg2A"));
}

} // namespace

class UploadRemovalSecurityTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void removeFileIsBoundToSenderAndCanvas();
    void removeAllIsBoundToSenderRoot();
    void removeAllFailureKeepsMappings();
    void scopedRemovalFailureRollsBackTheWholeBatch();
    void idempotentRecoveryIsBoundToPersistentSender();
    void interruptedUploadRemovesOnlyPartialStagingAndCanRetry();
    void v2UploadTeardownQuarantinesAndDropsMappings();
    void completedUploadAckIsReplayableAndInventoryBound();
    void receiverRejectsMp4WhoseMediaSamplesCannotDecode();
    void leaseExpiryBulkTeardownIncludesValidatedScopes();
    void terminalSignalsWaitForRendererBarrierBeforeQuarantine();
    void startupSweepPurgesOrphansAndRecoversInterruptedRemoval();
    void receiverAdvertisementFailsClosedWhenCacheCannotInitialize();
    void receiverAdvertisementRetriesUncommittedLogicalQuarantine();
    void duplicateUploadClickIsIgnoredBeforeExplicitCancellation();
    void protocolV2RunsTwoOutgoingSessionsConcurrently();
    void protocolV2TargetedRemovalIsExactAndIdempotent();

private:
    QString uploadRoot() const;
    QString createReceivedFile(const QString& senderId, const QString& fileId);
};

QString UploadRemovalSecurityTest::uploadRoot() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    return QFileInfo(QDir(base).filePath(QStringLiteral("Mouffette/Uploads")))
        .absoluteFilePath();
}

void UploadRemovalSecurityTest::init() {
    QStandardPaths::setTestModeEnabled(true);
    QDir(uploadRoot()).removeRecursively();
    QVERIFY(QDir().mkpath(uploadRoot()));
    LocalFileRepository::instance().clear();
    RemoteFileTracker::instance().clear();
}

void UploadRemovalSecurityTest::cleanup() {
    LocalFileRepository::instance().clear();
    RemoteFileTracker::instance().clear();
    QDir(uploadRoot()).removeRecursively();
}

void UploadRemovalSecurityTest::receiverAdvertisementFailsClosedWhenCacheCannotInitialize()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString invalidRoot =
        QDir(temporary.path()).filePath(QStringLiteral("not-a-directory"));
    QFile blocker(invalidRoot);
    QVERIFY(blocker.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(blocker.write("x", 1), qint64(1));
    blocker.close();

    FileManager files;
    UploadManager uploads(&files, nullptr, invalidRoot);
    QVERIFY(!uploads.receiverReadyForAdvertisement());
    QVERIFY(!uploads.retryReceiverAdvertisementCleanup());
    QVERIFY(!uploads.receiverCleanupError().isEmpty());
}

void UploadRemovalSecurityTest::receiverAdvertisementRetriesUncommittedLogicalQuarantine()
{
#ifdef Q_OS_WIN
    QSKIP("Creating a symlink requires privileges not guaranteed on Windows CI");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cacheRoot = QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    const QString outsidePath =
        QDir(temporary.path()).filePath(QStringLiteral("outside.bin"));
    QFile outside(outsidePath);
    QVERIFY(outside.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(outside.write("keep", 4), qint64(4));
    outside.close();

    const RemoteCacheStore::Scope failedScope{
        QString(64, QLatin1Char('a')),
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        1
    };
    const QString teardownId =
        QStringLiteral("22222222-2222-4222-8222-222222222222");
    QString linkPath;
    {
        RemoteCacheStore store(cacheRoot);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));
        QVERIFY2(store.ensureSession(failedScope, &error), qPrintable(error));
        linkPath = store.assetPath(
            failedScope, QString(64, QLatin1Char('b')),
            RemoteCacheStore::AssetArea::Staging, {}, &error);
        QVERIFY(!linkPath.isEmpty());
        QVERIFY(QFile::link(outsidePath, linkPath));
        QVERIFY2(store.beginTeardown(failedScope, teardownId, &error),
                 qPrintable(error));
        QCOMPARE(store.commitTeardown(failedScope, teardownId).outcome,
                 RemoteCacheStore::CommitOutcome::CleanupError);
    }

    FileManager files;
    UploadManager uploads(&files, nullptr, cacheRoot);
    QVERIFY(!uploads.receiverReadyForAdvertisement());
    QCOMPARE(uploads.receiverCleanupError(),
             QStringLiteral("logical_cleanup_not_committed"));
    QVERIFY(!uploads.retryReceiverAdvertisementCleanup());

    // Once the unsafe entry is removed, replaying the durable intent commits
    // the whole scope to quarantine. Physical deletion may remain asynchronous
    // without keeping the receiver unavailable.
    QVERIFY(QFile::remove(linkPath));
    QVERIFY(uploads.retryReceiverAdvertisementCleanup());
    QVERIFY(uploads.receiverReadyForAdvertisement());
    QVERIFY(QFileInfo::exists(outsidePath));
#endif
}

QString UploadRemovalSecurityTest::createReceivedFile(const QString& senderId,
                                                      const QString& fileId) {
    const QString stagingPath = QDir(uploadRoot()).filePath(
        senderId + QStringLiteral("/11111111-1111-4111-8111-111111111111"));
    if (!QDir().mkpath(stagingPath)) return {};

    const QString path = QDir(stagingPath).filePath(fileId + QStringLiteral(".png"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || file.write("owned", 5) != 5) {
        return {};
    }
    file.close();
    return QFileInfo(path).canonicalFilePath();
}

void UploadRemovalSecurityTest::removeFileIsBoundToSenderAndCanvas() {
    const QString senderA = QStringLiteral("sender_a");
    const QString senderB = QStringLiteral("sender_b");
    const QString fileId(64, QLatin1Char('a'));
    const QString expectedCanvas = QStringLiteral("canvas_expected");
    const QString otherCanvas = QStringLiteral("canvas_other");

    QVERIFY(QDir().mkpath(QDir(uploadRoot()).filePath(senderA)));
    const QString ownedPath = createReceivedFile(senderB, fileId);
    QVERIFY(!ownedPath.isEmpty());

    FileManager files;
    files.registerReceivedFilePath(fileId, ownedPath);
    files.associateFileWithIdea(fileId, expectedCanvas);
    UploadManager uploads(&files);

    auto removeMessage = [&](const QString& senderId, const QString& canvasId) {
        QJsonObject message;
        message["type"] = "remove_file";
        message["ownerDeviceId"] = senderId;
        message["fileId"] = fileId;
        message["canvasSessionId"] = canvasId;
        uploads.handleIncomingMessage(message);
    };

    removeMessage(senderA, expectedCanvas);
    QVERIFY2(QFileInfo::exists(ownedPath), "another sender must not delete this file");
    QCOMPARE(files.getFilePathForId(fileId), ownedPath);
    QVERIFY(files.getIdeaIdsForFile(fileId).contains(expectedCanvas));

    removeMessage(senderB, otherCanvas);
    QVERIFY2(QFileInfo::exists(ownedPath), "an unrelated canvas must not delete this file");
    QCOMPARE(files.getFilePathForId(fileId), ownedPath);
    QVERIFY(files.getIdeaIdsForFile(fileId).contains(expectedCanvas));

    removeMessage(senderA, QStringLiteral("default"));
    QVERIFY2(QFileInfo::exists(ownedPath), "DEFAULT_IDEA_ID must still enforce sender ownership");

    // Protocol v2 has no remove_file compatibility path. Even a tuple that
    // would have been valid under v1 must be ignored rather than translated.
    removeMessage(senderB, expectedCanvas);
    QVERIFY(QFileInfo::exists(ownedPath));
    QCOMPARE(files.getFilePathForId(fileId), ownedPath);
}

void UploadRemovalSecurityTest::removeAllIsBoundToSenderRoot() {
    const QString senderA = QStringLiteral("sender_a");
    const QString senderB = QStringLiteral("sender_b");
    const QString fileId(64, QLatin1Char('b'));
    const QString canvasId = QStringLiteral("canvas_shared_name");

    QVERIFY(QDir().mkpath(QDir(uploadRoot()).filePath(senderA)));
    const QString ownedPath = createReceivedFile(senderB, fileId);
    QVERIFY(!ownedPath.isEmpty());

    FileManager files;
    files.registerReceivedFilePath(fileId, ownedPath);
    files.associateFileWithIdea(fileId, canvasId);
    UploadManager uploads(&files);

    auto removeAllMessage = [&](const QString& senderId, const QString& ideaId) {
        QJsonObject message;
        message["type"] = "remove_all_files";
        message["ownerDeviceId"] = senderId;
        message["canvasSessionId"] = ideaId;
        message["removalId"] = QStringLiteral("22222222-2222-4222-8222-222222222222");
        uploads.handleIncomingMessage(message);
    };

    removeAllMessage(senderA, canvasId);
    QVERIFY2(QFileInfo::exists(ownedPath),
             "idea-scoped remove-all must not cross the authenticated sender root");
    QCOMPARE(files.getFilePathForId(fileId), ownedPath);
    QVERIFY(files.getIdeaIdsForFile(fileId).contains(canvasId));

    QVERIFY(QDir().mkpath(QDir(uploadRoot()).filePath(senderA)));
    removeAllMessage(senderA, QStringLiteral("default"));
    QVERIFY2(QFileInfo::exists(ownedPath),
             "DEFAULT_IDEA_ID remove-all must remain bounded to the sender root");
    QCOMPARE(files.getFilePathForId(fileId), ownedPath);

    // Protocol v2 has no remove_all_files compatibility path.
    removeAllMessage(senderB, canvasId);
    QVERIFY(QFileInfo::exists(ownedPath));
    QCOMPARE(files.getFilePathForId(fileId), ownedPath);
}

void UploadRemovalSecurityTest::removeAllFailureKeepsMappings() {
    const QString senderId = QStringLiteral("sender_failure");
    const QString fileId(64, QLatin1Char('c'));
    const QString canvasId = QStringLiteral("canvas_failure");
    QString path = createReceivedFile(senderId, fileId);
    QVERIFY(!path.isEmpty());
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir().mkpath(path)); // QFile::remove() must fail for this directory.
    path = QFileInfo(path).canonicalFilePath();

    FileManager files;
    files.registerReceivedFilePath(fileId, path);
    files.associateFileWithIdea(fileId, canvasId);
    UploadManager uploads(&files);

    QJsonObject message;
    message["type"] = "remove_all_files";
    message["ownerDeviceId"] = senderId;
    message["canvasSessionId"] = canvasId;
    message["removalId"] = QStringLiteral("33333333-3333-4333-8333-333333333333");
    uploads.handleIncomingMessage(message);

    QCOMPARE(files.getFilePathForId(fileId), path);
    QVERIFY(files.getIdeaIdsForFile(fileId).contains(canvasId));
    QVERIFY(QFileInfo(path).isDir());
}

void UploadRemovalSecurityTest::scopedRemovalFailureRollsBackTheWholeBatch() {
    const QString senderId = QStringLiteral("sender_transaction");
    const QString goodFileId(64, QLatin1Char('a'));
    const QString badFileId(64, QLatin1Char('b'));
    const QString canvasId = QStringLiteral("canvas_transaction");
    const QString goodPath = createReceivedFile(senderId, goodFileId);
    QString badPath = createReceivedFile(senderId, badFileId);
    QVERIFY(!goodPath.isEmpty());
    QVERIFY(!badPath.isEmpty());
    QVERIFY(QFile::remove(badPath));
    QVERIFY(QDir().mkpath(badPath));
    badPath = QFileInfo(badPath).canonicalFilePath();

    FileManager files;
    files.registerReceivedFilePath(goodFileId, goodPath);
    files.registerReceivedFilePath(badFileId, badPath);
    files.associateFileWithIdea(goodFileId, canvasId);
    files.associateFileWithIdea(badFileId, canvasId);
    UploadManager uploads(&files);

    QJsonObject message;
    message["type"] = "remove_all_files";
    message["ownerDeviceId"] = senderId;
    message["canvasSessionId"] = canvasId;
    message["removalId"] = QStringLiteral("34343434-3434-4434-8434-343434343434");
    uploads.handleIncomingMessage(message);

    QVERIFY2(QFileInfo::exists(goodPath),
             "a failed batch must not delete files validated earlier in the batch");
    QCOMPARE(files.getFilePathForId(goodFileId), goodPath);
    QCOMPARE(files.getFilePathForId(badFileId), badPath);
    QVERIFY(files.getIdeaIdsForFile(goodFileId).contains(canvasId));
    QVERIFY(files.getIdeaIdsForFile(badFileId).contains(canvasId));
}

void UploadRemovalSecurityTest::idempotentRecoveryIsBoundToPersistentSender() {
    const QString senderId = QStringLiteral("stable_sender");
    const QString foreignSenderId = QStringLiteral("foreign_sender");
    const QString remoteSessionId = QStringLiteral("session_stable");
    const QString retryUploadId = QStringLiteral("55555555-5555-4555-8555-555555555555");
    const QString firstUploadId = QStringLiteral("44444444-4444-4444-8444-444444444444");
    QImage image(12, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::cyan);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    QVERIFY(!bytes.isEmpty());
    const QString fileId = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    FileManager files;
    UploadManager uploads(&files);
    QSignalSpy replies(&uploads, &UploadManager::protocolV2UploadResponseReady);

    auto deliverUpload = [&](const QString& cacheOwner,
                             const QString& sessionId,
                             const QString& uploadId,
                             const QString& assetId,
                             const QString& mediaId) {
        QJsonObject manifestFile;
        manifestFile["assetId"] = assetId;
        manifestFile["fileId"] = fileId;
        manifestFile["sha256"] = fileId;
        manifestFile["name"] = QStringLiteral("image.png");
        manifestFile["extension"] = QStringLiteral("png");
        manifestFile["size"] = static_cast<double>(bytes.size());
        manifestFile["mediaIds"] = QJsonArray{mediaId};

        QJsonObject start;
        start["type"] = "upload_start";
        start["protocolVersion"] = 2;
        start["connectionGeneration"] = 11;
        start["generation"] = 1;
        start["ownerDeviceId"] = cacheOwner;
        start["remoteSessionId"] = sessionId;
        start["uploadId"] = uploadId;
        start["files"] = QJsonArray{manifestFile};
        uploads.handleIncomingMessage(start);

        QJsonObject chunk;
        chunk["type"] = "upload_chunk";
        chunk["protocolVersion"] = 2;
        chunk["connectionGeneration"] = 11;
        chunk["generation"] = 1;
        chunk["ownerDeviceId"] = cacheOwner;
        chunk["remoteSessionId"] = sessionId;
        chunk["uploadId"] = uploadId;
        chunk["assetId"] = assetId;
        chunk["offset"] = 0;
        chunk["size"] = static_cast<double>(bytes.size());
        chunk["sha256"] = fileId;
        chunk["data"] = QString::fromLatin1(bytes.toBase64());
        uploads.handleIncomingMessage(chunk);

        QJsonObject complete;
        complete["type"] = "upload_complete";
        complete["protocolVersion"] = 2;
        complete["connectionGeneration"] = 11;
        complete["generation"] = 1;
        complete["ownerDeviceId"] = cacheOwner;
        complete["remoteSessionId"] = sessionId;
        complete["uploadId"] = uploadId;
        complete["assets"] = QJsonArray{QJsonObject{
            {"assetId", assetId},
            {"offset", static_cast<double>(bytes.size())},
            {"size", static_cast<double>(bytes.size())},
            {"sha256", fileId},
        }};
        uploads.handleIncomingMessage(complete);
    };

    deliverUpload(senderId, remoteSessionId, firstUploadId,
                  QStringLiteral("asset_first"), QStringLiteral("media_first"));
    const QString canonicalExistingPath = files.getFilePathForId(fileId);
    QVERIFY(!canonicalExistingPath.isEmpty());
    QVERIFY(canonicalExistingPath.contains(
        senderId + QLatin1Char('/') + remoteSessionId + QStringLiteral("/validated/")));

    deliverUpload(senderId, remoteSessionId, retryUploadId,
                  QStringLiteral("asset_retry"), QStringLiteral("media_retry"));
    QCOMPARE(files.getFilePathForId(fileId), canonicalExistingPath);
    const QString retryStaging = QDir(uploadRoot()).filePath(
        senderId + QLatin1Char('/') + remoteSessionId
        + QStringLiteral("/staging/asset_retry.png"));
    QVERIFY2(!QFileInfo::exists(retryStaging),
             "byte-identical same-session retry staging must be removed");

    deliverUpload(foreignSenderId,
                  QStringLiteral("foreign_session"),
                  QStringLiteral("77777777-7777-4777-8777-777777777777"),
                  QStringLiteral("asset_foreign"),
                  QStringLiteral("media_foreign"));
    QCOMPARE(files.getFilePathForId(fileId), canonicalExistingPath);
    QVERIFY(!files.getIdeaIdsForFile(fileId).contains(QStringLiteral("foreign_session")));
    QVERIFY(!replies.isEmpty());
    QCOMPARE(replies.constLast().at(0).toJsonObject().value("type").toString(),
             QStringLiteral("upload_rejected"));
}

void UploadRemovalSecurityTest::interruptedUploadRemovesOnlyPartialStagingAndCanRetry() {
    const QString senderId = QStringLiteral("partial_sender");
    const QString uploadId = QStringLiteral("99990000-1111-4222-8333-444455556666");
    const QString remoteSessionId = QStringLiteral("session_partial");
    const QString assetId = QStringLiteral("asset_partial");
    const QByteArray bytes("\x89PNG\r\n\x1a\npartial-data", 20);
    const QString fileId = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    FileManager files;
    UploadManager uploads(&files);
    quint64 generation = 1;
    quint64 connectionGeneration = 7;

    QJsonObject manifestFile;
    manifestFile["assetId"] = assetId;
    manifestFile["fileId"] = fileId;
    manifestFile["sha256"] = fileId;
    manifestFile["name"] = QStringLiteral("partial.png");
    manifestFile["extension"] = QStringLiteral("png");
    manifestFile["size"] = static_cast<double>(bytes.size());
    manifestFile["mediaIds"] = QJsonArray{
        QStringLiteral("12345678-1234-4234-8234-123456789abc")
    };

    auto startUpload = [&] {
        QJsonObject start;
        start["type"] = "upload_start";
        start["protocolVersion"] = 2;
        start["connectionGeneration"] = static_cast<double>(connectionGeneration);
        start["generation"] = static_cast<double>(generation);
        start["ownerDeviceId"] = senderId;
        start["remoteSessionId"] = remoteSessionId;
        start["uploadId"] = uploadId;
        start["files"] = QJsonArray{manifestFile};
        uploads.handleIncomingMessage(start);
    };

    startUpload();
    const QString stagingDirectory = QDir(uploadRoot()).filePath(
        senderId + QLatin1Char('/') + remoteSessionId + QStringLiteral("/staging"));
    const QString stagingFile = QDir(stagingDirectory).filePath(assetId + QStringLiteral(".png"));
    QVERIFY(QDir(stagingDirectory).exists());
    QVERIFY(QFileInfo::exists(stagingFile));

    QJsonObject chunk;
    chunk["type"] = "upload_chunk";
    chunk["protocolVersion"] = 2;
    chunk["connectionGeneration"] = static_cast<double>(connectionGeneration);
    chunk["generation"] = static_cast<double>(generation);
    chunk["ownerDeviceId"] = senderId;
    chunk["remoteSessionId"] = remoteSessionId;
    chunk["uploadId"] = uploadId;
    chunk["assetId"] = assetId;
    chunk["offset"] = 0;
    chunk["size"] = 8;
    chunk["sha256"] = fileId;
    chunk["data"] = QString::fromLatin1(bytes.left(8).toBase64());
    uploads.handleIncomingMessage(chunk);
    QVERIFY(QFileInfo::exists(stagingFile));

    uploads.onConnectionLost();
    QVERIFY2(QFileInfo::exists(stagingFile),
             "transport loss within the lease must preserve durable staging");
    generation = 2;
    connectionGeneration = 8;
    QJsonObject resume{
        {"type", "upload_resume"}, {"protocolVersion", 2},
        {"connectionGeneration", static_cast<double>(connectionGeneration)},
        {"generation", static_cast<double>(generation)},
        {"ownerDeviceId", senderId}, {"remoteSessionId", remoteSessionId},
        {"uploadId", uploadId},
        {"assets", QJsonArray{QJsonObject{
            {"assetId", assetId}, {"offset", 8},
            {"size", static_cast<double>(bytes.size())}, {"sha256", fileId},
        }}},
    };
    uploads.handleIncomingMessage(resume);
    QCOMPARE(QFileInfo(stagingFile).size(), qint64(8));

    QJsonObject abort;
    abort["type"] = "upload_abort";
    abort["protocolVersion"] = 2;
    abort["connectionGeneration"] = static_cast<double>(connectionGeneration);
    abort["generation"] = static_cast<double>(generation);
    abort["ownerDeviceId"] = senderId;
    abort["remoteSessionId"] = remoteSessionId;
    abort["uploadId"] = uploadId;
    uploads.handleIncomingMessage(abort);

    QVERIFY2(!QDir(stagingDirectory).exists(),
             "an interrupted transfer must remove its partial staging directory");
    QVERIFY(files.getFilePathForId(fileId).isEmpty());

    startUpload();
    QVERIFY2(QDir(stagingDirectory).exists(),
             "cleanup must leave the receiver ready for an immediate retry");
    uploads.handleIncomingMessage(abort);
    QVERIFY(!QDir(stagingDirectory).exists());
}

void UploadRemovalSecurityTest::v2UploadTeardownQuarantinesAndDropsMappings() {
    const QString senderId = QStringLiteral("-sender_base64url");
    const QString remoteSessionId = QStringLiteral("session_teardown");
    const QString uploadId = QStringLiteral("upload_teardown");
    const QString assetId = QStringLiteral("asset_teardown");
    const QString teardownId = QStringLiteral("abababab-abab-4bab-8bab-abababababab");
    QImage image(9, 7, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::yellow);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    FileManager files;
    UploadManager uploads(&files);
    QSignalSpy replies(&uploads, &UploadManager::protocolV2UploadResponseReady);

    const QJsonObject manifest{
        {"assetId", assetId}, {"fileId", digest}, {"sha256", digest},
        {"name", "teardown.png"}, {"extension", "png"},
        {"size", static_cast<double>(bytes.size())},
        {"mediaIds", QJsonArray{QStringLiteral("media_teardown")}},
    };
    QJsonObject start{
        {"type", "upload_start"}, {"protocolVersion", 2},
        {"connectionGeneration", 3}, {"generation", 1},
        {"ownerDeviceId", senderId}, {"remoteSessionId", remoteSessionId},
        {"uploadId", uploadId}, {"files", QJsonArray{manifest}},
    };
    uploads.handleIncomingMessage(start);
    QJsonObject chunk{
        {"type", "upload_chunk"}, {"protocolVersion", 2},
        {"connectionGeneration", 3}, {"generation", 1},
        {"ownerDeviceId", senderId}, {"remoteSessionId", remoteSessionId},
        {"uploadId", uploadId}, {"assetId", assetId}, {"offset", 0},
        {"size", static_cast<double>(bytes.size())}, {"sha256", digest},
        {"data", QString::fromLatin1(bytes.toBase64())},
    };
    uploads.handleIncomingMessage(chunk);
    QJsonObject complete{
        {"type", "upload_complete"}, {"protocolVersion", 2},
        {"connectionGeneration", 3}, {"generation", 1},
        {"ownerDeviceId", senderId}, {"remoteSessionId", remoteSessionId},
        {"uploadId", uploadId},
        {"assets", QJsonArray{QJsonObject{
            {"assetId", assetId}, {"offset", static_cast<double>(bytes.size())},
            {"size", static_cast<double>(bytes.size())}, {"sha256", digest},
        }}},
    };
    uploads.handleIncomingMessage(complete);

    const QString validatedPath = files.getFilePathForId(digest);
    QVERIFY(!validatedPath.isEmpty());
    QVERIFY(QFileInfo::exists(validatedPath));
    QCOMPARE(replies.constLast().at(0).toJsonObject().value("type").toString(),
             QStringLiteral("upload_finished"));

    const RemoteCacheStore::CommitResult committed = uploads.teardownRemoteSession(
        senderId, remoteSessionId, 1, teardownId);
    QVERIFY(committed.acknowledgementSafe());
    QCOMPARE(uploads.lastTeardownRemovedFileCount(), 1);
    QVERIFY(files.getFilePathForId(digest).isEmpty());
    QVERIFY(!QFileInfo::exists(validatedPath));
    QTRY_COMPARE_WITH_TIMEOUT(
        uploads.remoteCacheStore()->state({senderId, remoteSessionId, 1}),
        RemoteCacheStore::SessionState::Closed, 5000);

    const RemoteCacheStore::CommitResult replay = uploads.teardownRemoteSession(
        senderId, remoteSessionId, 1, teardownId);
    QCOMPARE(replay.outcome, RemoteCacheStore::CommitOutcome::AlreadyCommitted);
    QVERIFY(replay.acknowledgementSafe());

    // A late duplicate can neither recreate the live namespace nor its mapping.
    uploads.handleIncomingMessage(chunk);
    QVERIFY(files.getFilePathForId(digest).isEmpty());
    QVERIFY(!QDir(QDir(uploadRoot()).filePath(senderId + QLatin1Char('/')
                                              + remoteSessionId)).exists());
}

void UploadRemovalSecurityTest::completedUploadAckIsReplayableAndInventoryBound()
{
    const QString senderId(43, QLatin1Char('A'));
    const QString remoteSessionId =
        QStringLiteral("12121212-3434-4567-8899-abcdefabcdef");
    const QString uploadId = QStringLiteral("upload-completion-replay");
    const QString assetId = QStringLiteral("asset-completion-replay");
    QImage image(11, 9, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::green);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    FileManager files;
    UploadManager uploads(&files);
    QSignalSpy replies(&uploads, &UploadManager::protocolV2UploadResponseReady);
    const QJsonObject manifest{
        {"assetId", assetId}, {"fileId", digest}, {"sha256", digest},
        {"name", "completion.png"}, {"extension", "png"},
        {"size", static_cast<double>(bytes.size())},
        {"mediaIds", QJsonArray{QStringLiteral("media-completion-replay")}},
    };
    const QJsonArray completionAssets{QJsonObject{
        {"assetId", assetId}, {"offset", static_cast<double>(bytes.size())},
        {"size", static_cast<double>(bytes.size())}, {"sha256", digest},
    }};
    auto boundMessage = [&](const QString& type, quint64 generation,
                            quint64 connectionGeneration) {
        return QJsonObject{
            {"type", type}, {"protocolVersion", 2},
            {"connectionGeneration", static_cast<double>(connectionGeneration)},
            {"generation", static_cast<double>(generation)},
            {"ownerDeviceId", senderId}, {"remoteSessionId", remoteSessionId},
            {"uploadId", uploadId},
        };
    };
    auto countReplies = [&](const QString& type) {
        int count = 0;
        for (const QList<QVariant>& arguments : replies) {
            if (arguments.at(0).toJsonObject().value("type").toString() == type) {
                ++count;
            }
        }
        return count;
    };

    QJsonObject start = boundMessage(QStringLiteral("upload_start"), 1, 3);
    start.insert("files", QJsonArray{manifest});
    uploads.handleIncomingMessage(start);
    QJsonObject chunk = boundMessage(QStringLiteral("upload_chunk"), 1, 3);
    chunk.insert("assetId", assetId);
    chunk.insert("offset", 0);
    chunk.insert("size", static_cast<double>(bytes.size()));
    chunk.insert("sha256", digest);
    chunk.insert("data", QString::fromLatin1(bytes.toBase64()));
    uploads.handleIncomingMessage(chunk);
    QJsonObject complete = boundMessage(QStringLiteral("upload_complete"), 1, 3);
    complete.insert("assets", completionAssets);
    uploads.handleIncomingMessage(complete);
    QCOMPARE(countReplies(QStringLiteral("upload_finished")), 1);
    const QString validatedPath = files.getFilePathForId(digest);
    QVERIFY(!validatedPath.isEmpty());
    QVERIFY(QFileInfo::exists(validatedPath));

    // If B's final ACK was lost, the server repeats only upload_complete.  B
    // must replay the exact result without recreating staging or decoding the
    // already-promoted asset a second time.
    uploads.handleIncomingMessage(complete);
    QCOMPARE(countReplies(QStringLiteral("upload_finished")), 2);
    QJsonObject replay = replies.constLast().at(0).toJsonObject();
    QVERIFY(replay.value("replay").toBool(false));
    QCOMPARE(replay.value("assets").toArray(), completionAssets);
    QVERIFY(QFileInfo::exists(validatedPath));
    QVERIFY(!QFileInfo::exists(QDir(uploadRoot()).filePath(
        senderId + QLatin1Char('/') + remoteSessionId
        + QStringLiteral("/staging/") + assetId + QStringLiteral(".png"))));

    // The idempotency result is bound to the complete immutable inventory.
    // A changed tuple is rejected and cannot inherit the previous success.
    QJsonObject mismatched = complete;
    QJsonArray mismatchedAssets = completionAssets;
    QJsonObject changed = mismatchedAssets.at(0).toObject();
    changed.insert("sha256", QString(64, QLatin1Char('f')));
    mismatchedAssets.replace(0, changed);
    mismatched.insert("assets", mismatchedAssets);
    uploads.handleIncomingMessage(mismatched);
    QCOMPARE(countReplies(QStringLiteral("upload_finished")), 2);
    QCOMPARE(replies.constLast().at(0).toJsonObject().value("type").toString(),
             QStringLiteral("upload_rejected"));
    QVERIFY(QFileInfo::exists(validatedPath));

    // A genuine same-process RemoteSession resume advances both generations;
    // the exact completion remains replayable under that authenticated tuple.
    QJsonObject resumed = boundMessage(QStringLiteral("upload_complete"), 2, 4);
    resumed.insert("assets", completionAssets);
    uploads.handleIncomingMessage(resumed);
    QCOMPARE(countReplies(QStringLiteral("upload_finished")), 3);
    replay = replies.constLast().at(0).toJsonObject();
    QCOMPARE(replay.value("generation").toInteger(), qint64(2));
    QVERIFY(replay.value("replay").toBool(false));

    // Once advanced, a stale generation cannot replay the terminal result.
    uploads.handleIncomingMessage(complete);
    QCOMPARE(countReplies(QStringLiteral("upload_finished")), 3);
    QCOMPARE(replies.constLast().at(0).toJsonObject().value("type").toString(),
             QStringLiteral("upload_rejected"));
}

void UploadRemovalSecurityTest::receiverRejectsMp4WhoseMediaSamplesCannotDecode()
{
    QByteArray bytes = oneFrameH264Mp4();
    QVERIFY(!bytes.isEmpty());
    const qsizetype mdatTypeOffset = bytes.indexOf(QByteArrayLiteral("mdat"));
    QVERIFY(mdatTypeOffset > 4);
    // mdat is the final top-level box in this fixture. Preserve the complete
    // ISO structure and sample tables while destroying only encoded samples.
    std::fill(bytes.begin() + mdatTypeOffset + 4, bytes.end(), '\0');

    const QString senderId(43, QLatin1Char('D'));
    const QString remoteSessionId =
        QStringLiteral("56565656-7878-49ab-8cde-123456789abc");
    const QString uploadId = QStringLiteral("upload-corrupt-mp4");
    const QString assetId = QStringLiteral("asset-corrupt-mp4");
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    FileManager files;
    UploadManager uploads(&files);
    QSignalSpy replies(&uploads, &UploadManager::protocolV2UploadResponseReady);
    const QJsonObject manifest{
        {"assetId", assetId}, {"fileId", digest}, {"sha256", digest},
        {"name", "corrupt.mp4"}, {"extension", "mp4"},
        {"size", static_cast<double>(bytes.size())},
        {"mediaIds", QJsonArray{QStringLiteral("media-corrupt-mp4")}},
    };
    uploads.handleIncomingMessage(QJsonObject{
        {"type", "upload_start"}, {"protocolVersion", 2},
        {"connectionGeneration", 7}, {"generation", 1},
        {"ownerDeviceId", senderId}, {"remoteSessionId", remoteSessionId},
        {"uploadId", uploadId}, {"files", QJsonArray{manifest}},
    });
    uploads.handleIncomingMessage(QJsonObject{
        {"type", "upload_chunk"}, {"protocolVersion", 2},
        {"connectionGeneration", 7}, {"generation", 1},
        {"ownerDeviceId", senderId}, {"remoteSessionId", remoteSessionId},
        {"uploadId", uploadId}, {"assetId", assetId}, {"offset", 0},
        {"size", static_cast<double>(bytes.size())}, {"sha256", digest},
        {"data", QString::fromLatin1(bytes.toBase64())},
    });
    uploads.handleIncomingMessage(QJsonObject{
        {"type", "upload_complete"}, {"protocolVersion", 2},
        {"connectionGeneration", 7}, {"generation", 1},
        {"ownerDeviceId", senderId}, {"remoteSessionId", remoteSessionId},
        {"uploadId", uploadId},
        {"assets", QJsonArray{QJsonObject{
            {"assetId", assetId}, {"offset", static_cast<double>(bytes.size())},
            {"size", static_cast<double>(bytes.size())}, {"sha256", digest},
        }}},
    });

    QVERIFY(!replies.isEmpty());
    QCOMPARE(replies.constLast().at(0).toJsonObject().value("type").toString(),
             QStringLiteral("upload_rejected"));
    QVERIFY(files.getFilePathForId(digest).isEmpty());
    const QString sessionRoot = QDir(uploadRoot()).filePath(
        senderId + QLatin1Char('/') + remoteSessionId);
    // Rejecting one invalid asset removes its staging/validated copies, but it
    // must not tear down the surrounding RemoteSession. A corrected manual
    // upload remains possible on the same session.
    QVERIFY(QDir(sessionRoot).exists());
    QVERIFY(!QFileInfo::exists(QDir(sessionRoot).filePath(
        QStringLiteral("staging/") + assetId + QStringLiteral(".mp4"))));
    QVERIFY(!QFileInfo::exists(QDir(sessionRoot).filePath(
        QStringLiteral("validated/") + assetId + QStringLiteral(".mp4"))));
    QVERIFY(uploads.remoteCacheStore()->acceptsCommands(
        RemoteCacheStore::Scope{senderId, remoteSessionId, 1}));
}

void UploadRemovalSecurityTest::leaseExpiryBulkTeardownIncludesValidatedScopes()
{
    FileManager files;
    UploadManager uploads(&files);
    const RemoteCacheStore::Scope first{
        QStringLiteral("sender_bulk_a"), QStringLiteral("session_bulk_a"), 4};
    const RemoteCacheStore::Scope second{
        QStringLiteral("sender_bulk_b"), QStringLiteral("session_bulk_b"), 9};
    const QString firstFileId(64, QLatin1Char('d'));
    const QString secondFileId(64, QLatin1Char('e'));
    QString error;

    QVERIFY2(uploads.remoteCacheStore()->ensureSession(first, &error),
             qPrintable(error));
    QVERIFY2(uploads.remoteCacheStore()->ensureSession(second, &error),
             qPrintable(error));
    const QString firstPath = uploads.remoteCacheStore()->assetPath(
        first, QStringLiteral("asset_bulk_a"),
        RemoteCacheStore::AssetArea::Validated, QStringLiteral("png"), &error);
    const QString secondPath = uploads.remoteCacheStore()->assetPath(
        second, QStringLiteral("asset_bulk_b"),
        RemoteCacheStore::AssetArea::Validated, QStringLiteral("png"), &error);
    QVERIFY2(!firstPath.isEmpty() && !secondPath.isEmpty(), qPrintable(error));
    QFile firstFile(firstPath);
    QVERIFY(firstFile.open(QIODevice::WriteOnly));
    QCOMPARE(firstFile.write("first", 5), qint64(5));
    firstFile.close();
    QFile secondFile(secondPath);
    QVERIFY(secondFile.open(QIODevice::WriteOnly));
    QCOMPARE(secondFile.write("second", 6), qint64(6));
    secondFile.close();
    files.registerReceivedFilePath(firstFileId, firstPath);
    files.registerReceivedFilePath(secondFileId, secondPath);

    const UploadManager::BulkTeardownResult result =
        uploads.teardownAllIncomingRemoteSessions(
            QStringLiteral("lease_expired"));
    QCOMPARE(result.discoveredScopes, 2);
    QCOMPARE(result.committedScopes, 2);
    QCOMPARE(result.cleanupErrorScopes, 0);
    QCOMPARE(result.removedFileMappings, 2);
    QVERIFY(result.quarantinedBytes >= 11);
    QVERIFY(result.allLogicallyCommitted());
    QVERIFY(files.getFilePathForId(firstFileId).isEmpty());
    QVERIFY(files.getFilePathForId(secondFileId).isEmpty());
    QVERIFY(!QFileInfo::exists(firstPath));
    QVERIFY(!QFileInfo::exists(secondPath));

    const auto localTombstone = uploads.remoteCacheStore()->tombstone(first);
    QVERIFY(localTombstone.has_value());
    QVERIFY(localTombstone->provisional);
    const QString officialTeardown =
        QStringLiteral("cdcdcdcd-cdcd-4dcd-8dcd-cdcdcdcdcdcd");
    const RemoteCacheStore::CommitResult official =
        uploads.teardownRemoteSession(first.senderDeviceId,
                                      first.remoteSessionId,
                                      first.generation,
                                      officialTeardown);
    QCOMPARE(official.outcome,
             RemoteCacheStore::CommitOutcome::AlreadyCommitted);
    QVERIFY(official.acknowledgementSafe());
    const auto adopted = uploads.remoteCacheStore()->tombstone(first);
    QVERIFY(adopted.has_value());
    QVERIFY(!adopted->provisional);
    QCOMPARE(adopted->teardownId, officialTeardown);
}

void UploadRemovalSecurityTest::terminalSignalsWaitForRendererBarrierBeforeQuarantine()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cacheRoot =
        QDir(temporary.path()).filePath(QStringLiteral("Uploads"));
    FileManager files;
    UploadManager uploads(&files, nullptr, cacheRoot);
    WebSocketClient transport(
        QDir(temporary.path()).filePath(QStringLiteral("identity")), false);
    uploads.setWebSocketClient(&transport);

    QString setupError;
    auto createScope = [&](const QString& sessionId,
                           QLatin1Char assetFill,
                           QLatin1Char fileFill) {
        const RemoteCacheStore::Scope scope{
            QString(64, QLatin1Char('a')), sessionId, 1};
        QString error;
        if (!uploads.remoteCacheStore()->ensureSession(scope, &error)) {
            setupError = error;
            return qMakePair(scope, QString());
        }
        const QString path = uploads.remoteCacheStore()->assetPath(
            scope, QString(64, assetFill),
            RemoteCacheStore::AssetArea::Validated,
            QStringLiteral("png"), &error);
        if (path.isEmpty()) {
            setupError = error;
            return qMakePair(scope, QString());
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            || file.write("cache", 5) != 5) {
            setupError = file.errorString();
            return qMakePair(scope, QString());
        }
        file.close();
        files.registerReceivedFilePath(QString(64, fileFill), path);
        return qMakePair(scope, path);
    };

    const auto leaseScope = createScope(
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QLatin1Char('b'), QLatin1Char('c'));
    QVERIFY2(setupError.isEmpty(), qPrintable(setupError));
    QVERIFY(!leaseScope.second.isEmpty());
    QSignalSpy barrierSpy(
        &uploads, &UploadManager::terminalIncomingCleanupRequired);
    emit transport.leaseExpired(QStringLiteral("boot"), 1);
    QCOMPARE(barrierSpy.count(), 1);
    QVERIFY(!uploads.receiverReadyForAdvertisement());
    QCOMPARE(uploads.receiverCleanupError(),
             QStringLiteral("renderer_teardown_pending"));
    QVERIFY(QFileInfo::exists(leaseScope.second));
    QVERIFY(!uploads.remoteCacheStore()->tombstone(leaseScope.first).has_value());
    // An eager recovery attempt is also barred until renderer settlement.
    QVERIFY(!uploads.retryReceiverAdvertisementCleanup());
    QVERIFY(QFileInfo::exists(leaseScope.second));

    const UploadManager::BulkTeardownResult leaseCleanup =
        uploads.completeTerminalIncomingCleanup(QStringLiteral("lease_expired"));
    QVERIFY(leaseCleanup.allLogicallyCommitted());
    QVERIFY(uploads.receiverReadyForAdvertisement());
    QVERIFY(!QFileInfo::exists(leaseScope.second));

    const auto restartScope = createScope(
        QStringLiteral("22222222-2222-4222-8222-222222222222"),
        QLatin1Char('d'), QLatin1Char('e'));
    QVERIFY2(setupError.isEmpty(), qPrintable(setupError));
    QVERIFY(!restartScope.second.isEmpty());
    emit transport.serverRestarted(QStringLiteral("old-boot"),
                                   QStringLiteral("new-boot"));
    QCOMPARE(barrierSpy.count(), 2);
    QVERIFY(!uploads.receiverReadyForAdvertisement());
    QVERIFY(QFileInfo::exists(restartScope.second));
    QVERIFY(!uploads.remoteCacheStore()->tombstone(restartScope.first).has_value());

    const UploadManager::BulkTeardownResult restartCleanup =
        uploads.completeTerminalIncomingCleanup(QStringLiteral("server_restart"));
    QVERIFY(restartCleanup.allLogicallyCommitted());
    QVERIFY(uploads.receiverReadyForAdvertisement());
    QVERIFY(!QFileInfo::exists(restartScope.second));
}

void UploadRemovalSecurityTest::startupSweepPurgesOrphansAndRecoversInterruptedRemoval() {
    const QString orphanDirectory = QDir(uploadRoot()).filePath(
        QStringLiteral("orphan_sender/11111111-2222-4333-8444-555566667777"));
    QVERIFY(QDir().mkpath(orphanDirectory));
    QFile orphanFile(QDir(orphanDirectory).filePath(QStringLiteral("partial.png")));
    QVERIFY(orphanFile.open(QIODevice::WriteOnly));
    QCOMPARE(orphanFile.write("partial", 7), 7);
    orphanFile.close();

    const QString senderName = QStringLiteral("tracked_sender");
    const QString trackedDirectory = QDir(uploadRoot()).filePath(
        senderName + QStringLiteral("/22222222-3333-4444-8555-666677778888"));
    QVERIFY(QDir().mkpath(trackedDirectory));
    const QString trackedFileId(64, QLatin1Char('9'));
    const QString trackedPath = QDir(trackedDirectory).filePath(trackedFileId
                                                               + QStringLiteral(".png"));
    QFile trackedFile(trackedPath);
    QVERIFY(trackedFile.open(QIODevice::WriteOnly));
    QCOMPARE(trackedFile.write("tracked", 7), 7);
    trackedFile.close();

    FileManager files;
    files.registerReceivedFilePath(trackedFileId, QFileInfo(trackedPath).canonicalFilePath());
    const QString quarantineName = QStringLiteral(
        ".mouffette-removing-33333333-4444-4555-8666-777788889999-%1")
        .arg(senderName);
    QDir root(uploadRoot());
    QVERIFY(root.rename(senderName, quarantineName));
    QVERIFY(!QFileInfo::exists(trackedPath));

    UploadManager uploads(&files);
    Q_UNUSED(uploads);

    QVERIFY2(!QDir(orphanDirectory).exists(),
             "untracked staging left by a crash must be removed on startup");
    QVERIFY2(QFileInfo::exists(trackedPath),
             "a mapped cache directory quarantined before a crash must be restored");
    QVERIFY(!root.exists(quarantineName));
}

void UploadRemovalSecurityTest::duplicateUploadClickIsIgnoredBeforeExplicitCancellation() {
    QWebSocketServer server(QStringLiteral("upload-state-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString nonce = QString::fromLatin1(QByteArray(32, 'u').toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    QPointer<QWebSocket> peer;
    connect(&server, &QWebSocketServer::newConnection, this, [&]() {
        peer = server.nextPendingConnection();
        QVERIFY(peer);
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "auth_challenge"}, {"protocolVersion", 2},
            {"serverBootId", bootId},
            {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"nonce", nonce}, {"issuedAt", 1},
        }).toJson(QJsonDocument::Compact)));
        connect(peer, &QWebSocket::textMessageReceived, this,
                [&, peer](const QString& encoded) {
            const QJsonObject request = QJsonDocument::fromJson(encoded.toUtf8()).object();
            if (request.value("type").toString() != QLatin1String("auth_response")) return;
            const QJsonObject policy{
                {"policyVersion", 1}, {"heartbeatIntervalMs", 750},
                {"leaseTimeoutMs", 3000}, {"scenePrepareTimeoutMs", 15000},
                {"sceneActivationLeadMs", 4000}, {"sceneMaxClockSkewMs", 50},
                {"uploadIdleTimeoutMs", 45000}, {"uploadTargetAckTimeoutMs", 30000},
                {"removalAckTimeoutMs", 30000},
            };
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                {"type", "welcome"}, {"protocolVersion", 2},
                {"serverBootId", bootId},
                {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"deviceId", request.value("deviceId")},
                {"runtimeId", request.value("runtimeId")},
                {"connectionGeneration", 1}, {"policy", policy},
                {"serverMonotonicMs", 1},
            }).toJson(QJsonDocument::Compact)));
        });
    });

    QTemporaryDir identityDirectory;
    QVERIFY(identityDirectory.isValid());
    // Unit tests must never prompt or block on the interactive system vault.
    WebSocketClient socket(identityDirectory.path(), false);
    QSignalSpy connected(&socket, &WebSocketClient::connected);
    socket.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 3000);
    QVERIFY(peer);

    const QString targetDeviceId(43, QLatin1Char('B'));
    const QString remoteSessionId =
        QStringLiteral("11111111-2222-4333-8444-555566667788");
    peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "remote_session_opened"}, {"protocolVersion", 2},
        {"serverBootId", bootId},
        {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"connectionGeneration", 1},
        {"remoteSessionId", remoteSessionId}, {"generation", 1},
        {"ownerConnectionGeneration", 1},
        {"targetConnectionGeneration", 1},
        {"phase", "Active"}, {"ownerDeviceId", socket.deviceId()},
        {"targetDeviceId", targetDeviceId},
        {"resumeToken", "memory_only_resume_token"},
    }).toJson(QJsonDocument::Compact)));
    QTRY_VERIFY_WITH_TIMEOUT(
        socket.remoteSessionCoordinator()->forPeer(targetDeviceId).active, 1000);

    QTemporaryDir sources;
    QVERIFY(sources.isValid());
    const QString sourcePath = QDir(sources.path()).filePath(QStringLiteral("pixel.png"));
    QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::magenta);
    QVERIFY(image.save(sourcePath));

    FileManager files;
    const QString fileId = files.getOrCreateFileId(sourcePath);
    const QString mediaId = QStringLiteral("44444444-5555-4666-8777-888899990000");
    QVERIFY(!fileId.isEmpty());
    files.associateMediaWithFile(mediaId, fileId);

    UploadFileInfo info;
    info.fileId = fileId;
    info.mediaId = mediaId;
    info.path = sourcePath;
    info.name = QStringLiteral("pixel.png");
    info.extension = QStringLiteral("png");
    info.size = QFileInfo(sourcePath).size();

    UploadManager uploads(&files);
    uploads.setWebSocketClient(&socket);
    uploads.setTargetClientId(targetDeviceId);
    uploads.setActiveIdeaId(QStringLiteral("canvas_click_guard"));

    QVERIFY(uploads.toggleUpload(QVector<UploadFileInfo>{info}));
    QCOMPARE(uploads.outgoingState(), UploadManager::OutgoingState::AwaitingTargetReady);
    QVERIFY(!uploads.toggleUpload(QVector<UploadFileInfo>{info}));
    QCOMPARE(uploads.outgoingState(), UploadManager::OutgoingState::AwaitingTargetReady);
    uploads.requestCancel();
    QCOMPARE(uploads.outgoingState(), UploadManager::OutgoingState::AwaitingTargetReady);

    QTest::qWait(1050);
    uploads.requestCancel();
    QCOMPARE(uploads.outgoingState(), UploadManager::OutgoingState::Cancelling);
    socket.disconnect();
}

void UploadRemovalSecurityTest::protocolV2RunsTwoOutgoingSessionsConcurrently() {
    QWebSocketServer server(QStringLiteral("upload-concurrency-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString nonce = QString::fromLatin1(QByteArray(32, 'v').toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    QPointer<QWebSocket> peer;
    QList<QJsonObject> uploadStarts;
    connect(&server, &QWebSocketServer::newConnection, this, [&]() {
        peer = server.nextPendingConnection();
        QVERIFY(peer);
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "auth_challenge"}, {"protocolVersion", 2},
            {"serverBootId", bootId},
            {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"nonce", nonce}, {"issuedAt", 1},
        }).toJson(QJsonDocument::Compact)));
        connect(peer, &QWebSocket::textMessageReceived, this,
                [&, peer](const QString& encoded) {
            const QJsonObject request = QJsonDocument::fromJson(encoded.toUtf8()).object();
            const QString type = request.value("type").toString();
            if (type == QLatin1String("auth_response")) {
                const QJsonObject policy{
                    {"policyVersion", 1}, {"heartbeatIntervalMs", 750},
                    {"leaseTimeoutMs", 3000}, {"scenePrepareTimeoutMs", 15000},
                    {"sceneActivationLeadMs", 4000}, {"sceneMaxClockSkewMs", 50},
                    {"uploadIdleTimeoutMs", 45000},
                    {"uploadTargetAckTimeoutMs", 30000},
                    {"removalAckTimeoutMs", 30000},
                };
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"type", "welcome"}, {"protocolVersion", 2},
                    {"serverBootId", bootId},
                    {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"deviceId", request.value("deviceId")},
                    {"runtimeId", request.value("runtimeId")},
                    {"connectionGeneration", 1}, {"policy", policy},
                    {"serverMonotonicMs", 1},
                }).toJson(QJsonDocument::Compact)));
            } else if (type == QLatin1String("heartbeat")) {
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"type", "heartbeat_ack"}, {"protocolVersion", 2},
                    {"serverBootId", bootId},
                    {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"connectionGeneration", 1},
                    {"sequence", request.value("sequence")},
                    {"clientMonotonicMs", request.value("clientMonotonicMs")},
                    {"serverMonotonicMs", request.value("clientMonotonicMs")},
                    {"serverEpochMs", 1},
                }).toJson(QJsonDocument::Compact)));
            } else if (type == QLatin1String("upload_start")) {
                uploadStarts.append(request);
            }
        });
    });

    QTemporaryDir identityDirectory;
    QVERIFY(identityDirectory.isValid());
    WebSocketClient socket(identityDirectory.path(), false);
    QSignalSpy connected(&socket, &WebSocketClient::connected);
    socket.connectToServer(QStringLiteral("ws://127.0.0.1:%1")
                               .arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 3000);
    QVERIFY(peer);

    const QString targetA(43, QLatin1Char('B'));
    const QString targetB(43, QLatin1Char('C'));
    const QString sessionA = QStringLiteral("aaaaaaaa-2222-4333-8444-555566667788");
    const QString sessionB = QStringLiteral("bbbbbbbb-2222-4333-8444-555566667788");
    auto openSession = [&](const QString& sessionId, const QString& targetId) {
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "remote_session_opened"}, {"protocolVersion", 2},
            {"serverBootId", bootId},
            {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"connectionGeneration", 1},
            {"remoteSessionId", sessionId}, {"generation", 1},
            {"ownerConnectionGeneration", 1},
            {"targetConnectionGeneration", 1},
            {"phase", "Active"}, {"ownerDeviceId", socket.deviceId()},
            {"targetDeviceId", targetId}, {"resumeToken", "memory_only_token"},
        }).toJson(QJsonDocument::Compact)));
    };
    openSession(sessionA, targetA);
    openSession(sessionB, targetB);
    QTRY_VERIFY_WITH_TIMEOUT(socket.remoteSessionCoordinator()->forPeer(targetA).active, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(socket.remoteSessionCoordinator()->forPeer(targetB).active, 1000);

    QTemporaryDir sources;
    QVERIFY(sources.isValid());
    FileManager files;
    auto makeUpload = [&](const QString& fileName, const QColor& color,
                          const QString& mediaId) {
        const QString path = QDir(sources.path()).filePath(fileName);
        QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(color);
        if (!image.save(path)) return UploadFileInfo{};
        const QString fileId = files.getOrCreateFileId(path);
        files.associateMediaWithFile(mediaId, fileId);
        return UploadFileInfo{fileId, mediaId, path, fileName,
                              QStringLiteral("png"), QFileInfo(path).size()};
    };
    const UploadFileInfo uploadA = makeUpload(
        QStringLiteral("alpha.png"), Qt::red,
        QStringLiteral("aaaaaaaa-5555-4666-8777-888899990000"));
    const UploadFileInfo uploadB = makeUpload(
        QStringLiteral("beta.png"), Qt::blue,
        QStringLiteral("bbbbbbbb-5555-4666-8777-888899990000"));
    QVERIFY(!uploadA.fileId.isEmpty());
    QVERIFY(!uploadB.fileId.isEmpty());

    UploadManager uploads(&files);
    uploads.setWebSocketClient(&socket);
    uploads.setTargetClientId(targetA);
    QVERIFY(uploads.toggleUpload({uploadA}));
    QCOMPARE(uploads.outgoingState(), UploadManager::OutgoingState::AwaitingTargetReady);
    QTest::qWait(310);
    uploads.setTargetClientId(targetB);
    QVERIFY(uploads.toggleUpload({uploadB}));
    QTRY_COMPARE_WITH_TIMEOUT(uploads.activeOutgoingTransferCount(), 2, 1000);
    QCOMPARE(uploads.uploadScheduler()->activeCount(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(uploadStarts.size(), 2, 1000);
    const QSet<QString> actualSessions{
        uploadStarts.at(0).value("remoteSessionId").toString(),
        uploadStarts.at(1).value("remoteSessionId").toString()};
    const QSet<QString> expectedSessions{sessionA, sessionB};
    QCOMPARE(actualSessions, expectedSessions);
    for (const QJsonObject& start : std::as_const(uploadStarts)) {
        QVERIFY(!start.contains("targetClientId"));
        QVERIFY(!start.contains("canvasSessionId"));
        QCOMPARE(start.value("protocolVersion").toInt(), 2);
        QCOMPARE(start.value("generation").toInt(), 1);
    }

    // Closing A is session-scoped. It must not route through the transport-loss
    // path, which would suspend the independent upload to B indefinitely.
    peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "remote_session_terminating"}, {"protocolVersion", 2},
        {"serverBootId", bootId},
        {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"connectionGeneration", 1},
        {"remoteSessionId", sessionA}, {"generation", 1},
        {"ownerConnectionGeneration", 1},
        {"targetConnectionGeneration", 1},
        {"phase", "Terminating"}, {"ownerDeviceId", socket.deviceId()},
        {"targetDeviceId", targetA},
        {"teardownId", "cccccccc-3333-4333-8333-666677778888"},
    }).toJson(QJsonDocument::Compact)));
    QTRY_COMPARE_WITH_TIMEOUT(uploads.activeOutgoingTransferCount(), 1, 1000);
    QCOMPARE(uploads.uploadScheduler()->activeCount(), 1);
    uploads.setTargetClientId(targetB);
    QCOMPARE(uploads.outgoingState(),
             UploadManager::OutgoingState::AwaitingTargetReady);
    socket.disconnect();
}

void UploadRemovalSecurityTest::protocolV2TargetedRemovalIsExactAndIdempotent() {
    QWebSocketServer server(QStringLiteral("asset-removal-target-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString nonce = QString::fromLatin1(QByteArray(32, 'r').toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    QPointer<QWebSocket> peer;
    QList<QJsonObject> clientMessages;
    connect(&server, &QWebSocketServer::newConnection, this, [&]() {
        peer = server.nextPendingConnection();
        QVERIFY(peer);
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "auth_challenge"}, {"protocolVersion", 2},
            {"serverBootId", bootId},
            {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"nonce", nonce}, {"issuedAt", 1},
        }).toJson(QJsonDocument::Compact)));
        connect(peer, &QWebSocket::textMessageReceived, this,
                [&, peer](const QString& encoded) {
            const QJsonObject request =
                QJsonDocument::fromJson(encoded.toUtf8()).object();
            const QString type = request.value("type").toString();
            if (type == QLatin1String("auth_response")) {
                const QJsonObject policy{
                    {"policyVersion", 1}, {"heartbeatIntervalMs", 750},
                    {"leaseTimeoutMs", 3000}, {"scenePrepareTimeoutMs", 15000},
                    {"sceneActivationLeadMs", 4000}, {"sceneMaxClockSkewMs", 50},
                    {"uploadIdleTimeoutMs", 45000},
                    {"uploadTargetAckTimeoutMs", 30000},
                    {"removalAckTimeoutMs", 30000},
                };
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"type", "welcome"}, {"protocolVersion", 2},
                    {"serverBootId", bootId},
                    {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"deviceId", request.value("deviceId")},
                    {"runtimeId", request.value("runtimeId")},
                    {"connectionGeneration", 1}, {"policy", policy},
                    {"serverMonotonicMs", 1},
                }).toJson(QJsonDocument::Compact)));
            } else if (type == QLatin1String("heartbeat")) {
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"type", "heartbeat_ack"}, {"protocolVersion", 2},
                    {"serverBootId", bootId},
                    {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"connectionGeneration", 1},
                    {"sequence", request.value("sequence")},
                    {"clientMonotonicMs", request.value("clientMonotonicMs")},
                    {"serverMonotonicMs", request.value("clientMonotonicMs")},
                    {"serverEpochMs", 1},
                }).toJson(QJsonDocument::Compact)));
            } else {
                clientMessages.append(request);
            }
        });
    });

    QTemporaryDir identityDirectory;
    QTemporaryDir cacheDirectory;
    QVERIFY(identityDirectory.isValid());
    QVERIFY(cacheDirectory.isValid());
    WebSocketClient socket(identityDirectory.path(), false);
    QSignalSpy connected(&socket, &WebSocketClient::connected);
    socket.connectToServer(QStringLiteral("ws://127.0.0.1:%1")
                               .arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 3000);
    QVERIFY(peer);

    const QString ownerDeviceId(43, QLatin1Char('A'));
    const QString remoteSessionId =
        QStringLiteral("dddddddd-2222-4333-8444-555566667788");
    peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "remote_session_opened"}, {"protocolVersion", 2},
        {"serverBootId", bootId},
        {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"connectionGeneration", 1},
        {"remoteSessionId", remoteSessionId}, {"generation", 1},
        {"ownerConnectionGeneration", 1},
        {"targetConnectionGeneration", 1},
        {"phase", "Active"}, {"ownerDeviceId", ownerDeviceId},
        {"targetDeviceId", socket.deviceId()},
        {"resumeToken", "memory_only_removal_token"},
    }).toJson(QJsonDocument::Compact)));
    QTRY_VERIFY_WITH_TIMEOUT(
        socket.remoteSessionCoordinator()->forPeer(ownerDeviceId).active, 1000);

    FileManager files;
    const QString cacheRoot = QDir(cacheDirectory.path()).filePath("Uploads");
    UploadManager uploads(&files, nullptr, cacheRoot);
    uploads.setWebSocketClient(&socket);
    const RemoteCacheStore::Scope scope{ownerDeviceId, remoteSessionId, 1};
    QString error;
    QVERIFY2(uploads.remoteCacheStore()->ensureSession(scope, &error),
             qPrintable(error));
    const QString assetId(64, QLatin1Char('e'));
    const QString digest(64, QLatin1Char('f'));
    const qint64 assetSize = 17;
    const QString validatedPath = uploads.remoteCacheStore()->assetPath(
        scope, assetId, RemoteCacheStore::AssetArea::Validated,
        QStringLiteral("png"), &error);
    QVERIFY2(!validatedPath.isEmpty(), qPrintable(error));
    QFile cachedFile(validatedPath);
    QVERIFY(cachedFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(cachedFile.write(QByteArray(assetSize, 'x')), assetSize);
    cachedFile.close();
    files.registerReceivedFilePath(digest, validatedPath);
    files.associateFileWithIdea(digest, remoteSessionId);

    auto command = [&](const QString& removalId) {
        return QJsonObject{
            {"type", "upload_remove"}, {"protocolVersion", 2},
            {"serverBootId", bootId},
            {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"connectionGeneration", 1},
            {"remoteSessionId", remoteSessionId}, {"generation", 1},
            {"ownerDeviceId", ownerDeviceId},
            {"targetDeviceId", socket.deviceId()},
            {"removalId", removalId}, {"uploadId", "upload-removal-exact"},
            {"assetId", assetId}, {"offset", assetSize}, {"size", assetSize},
            {"sha256", digest}, {"fileId", digest}, {"extension", "png"},
        };
    };

    // A server-derived field mismatch fails closed without touching the file.
    QJsonObject malformed = command(
        QStringLiteral("11111111-2222-4333-8444-555566667799"));
    malformed.insert("fileId", QString(64, QLatin1Char('0')));
    peer->sendTextMessage(QString::fromUtf8(
        QJsonDocument(malformed).toJson(QJsonDocument::Compact)));
    QTRY_VERIFY_WITH_TIMEOUT(!clientMessages.isEmpty(), 1000);
    const QJsonObject rejection = clientMessages.constLast();
    QCOMPARE(rejection.value("type").toString(), QStringLiteral("upload_removed"));
    QCOMPARE(rejection.value("result").toString(), QStringLiteral("cleanup_error"));
    QVERIFY(!rejection.value("cacheQuarantined").toBool(true));
    QVERIFY(QFileInfo::exists(validatedPath));
    QCOMPARE(QFileInfo(files.getFilePathForId(digest)).canonicalFilePath(),
             QFileInfo(validatedPath).canonicalFilePath());

    const QString removalId =
        QStringLiteral("22222222-3333-4444-8555-666666666677");
    peer->sendTextMessage(QString::fromUtf8(
        QJsonDocument(command(removalId)).toJson(QJsonDocument::Compact)));
    QTRY_VERIFY_WITH_TIMEOUT(clientMessages.size() >= 2, 1000);
    const QJsonObject committed = clientMessages.constLast();
    QCOMPARE(committed.value("type").toString(), QStringLiteral("upload_removed"));
    QCOMPARE(committed.value("removalId").toString(), removalId);
    QCOMPARE(committed.value("result").toString(), QStringLiteral("committed"));
    QVERIFY(committed.value("cacheQuarantined").toBool(false));
    QVERIFY(!QFileInfo::exists(validatedPath));
    QVERIFY(files.getFilePathForId(digest).isEmpty());
    QVERIFY(uploads.remoteCacheStore()->acceptsCommands(scope));

    // The same immutable command replays the durable local tombstone and does
    // not close or recreate the surrounding RemoteSession.
    peer->sendTextMessage(QString::fromUtf8(
        QJsonDocument(command(removalId)).toJson(QJsonDocument::Compact)));
    QTRY_VERIFY_WITH_TIMEOUT(clientMessages.size() >= 3, 1000);
    const QJsonObject replay = clientMessages.constLast();
    QCOMPARE(replay.value("type").toString(), QStringLiteral("upload_removed"));
    QCOMPARE(replay.value("result").toString(), QStringLiteral("committed"));
    QCOMPARE(replay.value("quarantinedBytes").toInteger(), assetSize);
    QVERIFY(uploads.remoteCacheStore()->acceptsCommands(scope));

    // A tombstone is bound to the complete immutable removal tuple, not only
    // removalId + path. Reusing the ID with new size/digest metadata must never
    // replay a successful commit for a different logical request.
    QJsonObject conflictingReplay = command(removalId);
    conflictingReplay.insert("offset", assetSize + 1);
    conflictingReplay.insert("size", assetSize + 1);
    conflictingReplay.insert("sha256", QString(64, QLatin1Char('9')));
    conflictingReplay.insert("fileId", QString(64, QLatin1Char('9')));
    peer->sendTextMessage(QString::fromUtf8(
        QJsonDocument(conflictingReplay).toJson(QJsonDocument::Compact)));
    QTRY_VERIFY_WITH_TIMEOUT(clientMessages.size() >= 4, 1000);
    const QJsonObject conflict = clientMessages.constLast();
    QCOMPARE(conflict.value("type").toString(), QStringLiteral("upload_removed"));
    QCOMPARE(conflict.value("result").toString(), QStringLiteral("cleanup_error"));
    QVERIFY(!conflict.value("cacheQuarantined").toBool(true));
    socket.disconnect();
}

QTEST_GUILESS_MAIN(UploadRemovalSecurityTest)
#include "tst_UploadRemovalSecurity.moc"
