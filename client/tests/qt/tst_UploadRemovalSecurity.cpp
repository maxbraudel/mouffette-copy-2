#include <QtTest>

#include "backend/files/FileManager.h"
#include "backend/files/LocalFileRepository.h"
#include "backend/network/RemoteFileTracker.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>

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
    void startupSweepPurgesOrphansAndRecoversInterruptedRemoval();
    void duplicateUploadClickIsIgnoredBeforeExplicitCancellation();

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
        message["senderClientId"] = senderId;
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

    removeMessage(senderB, expectedCanvas);
    QVERIFY(!QFileInfo::exists(ownedPath));
    QVERIFY(files.getFilePathForId(fileId).isEmpty());
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
        message["senderClientId"] = senderId;
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

    removeAllMessage(senderB, canvasId);
    QVERIFY(!QFileInfo::exists(ownedPath));
    QVERIFY(files.getFilePathForId(fileId).isEmpty());
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
    message["senderClientId"] = senderId;
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
    message["senderClientId"] = senderId;
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
    const QString fileId(64, QLatin1Char('d'));
    const QString oldUploadId = QStringLiteral("44444444-4444-4444-8444-444444444444");
    const QString retryUploadId = QStringLiteral("55555555-5555-4555-8555-555555555555");
    const QString retryCanvas = QStringLiteral("canvas_after_server_restart");
    const QString oldDirectory = QDir(uploadRoot()).filePath(senderId + QLatin1Char('/') + oldUploadId);
    QVERIFY(QDir().mkpath(oldDirectory));
    const QString existingPath = QDir(oldDirectory).filePath(fileId + QStringLiteral(".png"));
    QImage image(12, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::cyan);
    QVERIFY(image.save(existingPath));

    QFile source(existingPath);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray bytes = source.readAll();
    source.close();
    QVERIFY(!bytes.isEmpty());

    FileManager files;
    const QString canonicalExistingPath = QFileInfo(existingPath).canonicalFilePath();
    files.registerReceivedFilePath(fileId, canonicalExistingPath);
    files.associateFileWithIdea(fileId, QStringLiteral("canvas_before_server_restart"));
    UploadManager uploads(&files);

    auto deliverUpload = [&](const QString& cacheOwner,
                             const QString& uploadId,
                             const QString& canvasId,
                             const QString& mediaId) {
        QJsonObject manifestFile;
        manifestFile["fileId"] = fileId;
        manifestFile["name"] = QStringLiteral("image.png");
        manifestFile["extension"] = QStringLiteral("png");
        manifestFile["sizeBytes"] = static_cast<double>(bytes.size());
        manifestFile["mediaIds"] = QJsonArray{mediaId};

        QJsonObject start;
        start["type"] = "upload_start";
        start["senderClientId"] = QStringLiteral("ephemeral_session");
        start["senderPersistentClientId"] = cacheOwner;
        start["uploadId"] = uploadId;
        start["canvasSessionId"] = canvasId;
        start["files"] = QJsonArray{manifestFile};
        uploads.handleIncomingMessage(start);

        QJsonObject chunk;
        chunk["type"] = "upload_chunk";
        chunk["senderClientId"] = QStringLiteral("ephemeral_session");
        chunk["senderPersistentClientId"] = cacheOwner;
        chunk["uploadId"] = uploadId;
        chunk["canvasSessionId"] = canvasId;
        chunk["fileId"] = fileId;
        chunk["chunkIndex"] = 0;
        chunk["data"] = QString::fromLatin1(bytes.toBase64());
        uploads.handleIncomingMessage(chunk);

        QJsonObject complete;
        complete["type"] = "upload_complete";
        complete["senderClientId"] = QStringLiteral("ephemeral_session");
        complete["senderPersistentClientId"] = cacheOwner;
        complete["uploadId"] = uploadId;
        complete["canvasSessionId"] = canvasId;
        uploads.handleIncomingMessage(complete);
    };

    deliverUpload(senderId, retryUploadId, retryCanvas,
                  QStringLiteral("66666666-6666-4666-8666-666666666666"));
    QCOMPARE(files.getFilePathForId(fileId), canonicalExistingPath);
    QVERIFY(files.getIdeaIdsForFile(fileId).contains(retryCanvas));
    const QString retryStaging = QDir(uploadRoot()).filePath(senderId + QLatin1Char('/') + retryUploadId);
    QVERIFY2(!QDir(retryStaging).exists(), "byte-identical retry staging must be removed");

    const QString foreignCanvas = QStringLiteral("foreign_canvas");
    deliverUpload(foreignSenderId,
                  QStringLiteral("77777777-7777-4777-8777-777777777777"),
                  foreignCanvas,
                  QStringLiteral("88888888-8888-4888-8888-888888888888"));
    QCOMPARE(files.getFilePathForId(fileId), canonicalExistingPath);
    QVERIFY(!files.getIdeaIdsForFile(fileId).contains(foreignCanvas));
}

void UploadRemovalSecurityTest::interruptedUploadRemovesOnlyPartialStagingAndCanRetry() {
    const QString senderId = QStringLiteral("partial_sender");
    const QString uploadId = QStringLiteral("99990000-1111-4222-8333-444455556666");
    const QString canvasId = QStringLiteral("partial_canvas");
    const QString fileId(64, QLatin1Char('f'));
    const QByteArray bytes("\x89PNG\r\n\x1a\npartial-data", 20);

    FileManager files;
    UploadManager uploads(&files);

    QJsonObject manifestFile;
    manifestFile["fileId"] = fileId;
    manifestFile["name"] = QStringLiteral("partial.png");
    manifestFile["extension"] = QStringLiteral("png");
    manifestFile["sizeBytes"] = static_cast<double>(bytes.size());
    manifestFile["mediaIds"] = QJsonArray{
        QStringLiteral("12345678-1234-4234-8234-123456789abc")
    };

    auto startUpload = [&] {
        QJsonObject start;
        start["type"] = "upload_start";
        start["senderClientId"] = QStringLiteral("ephemeral_sender");
        start["senderPersistentClientId"] = senderId;
        start["uploadId"] = uploadId;
        start["canvasSessionId"] = canvasId;
        start["files"] = QJsonArray{manifestFile};
        uploads.handleIncomingMessage(start);
    };

    startUpload();
    const QString stagingDirectory = QDir(uploadRoot()).filePath(senderId + QLatin1Char('/') + uploadId);
    const QString stagingFile = QDir(stagingDirectory).filePath(fileId + QStringLiteral(".png"));
    QVERIFY(QDir(stagingDirectory).exists());
    QVERIFY(QFileInfo::exists(stagingFile));

    QJsonObject chunk;
    chunk["type"] = "upload_chunk";
    chunk["senderClientId"] = QStringLiteral("ephemeral_sender");
    chunk["senderPersistentClientId"] = senderId;
    chunk["uploadId"] = uploadId;
    chunk["canvasSessionId"] = canvasId;
    chunk["fileId"] = fileId;
    chunk["chunkIndex"] = 0;
    chunk["data"] = QString::fromLatin1(bytes.left(8).toBase64());
    uploads.handleIncomingMessage(chunk);
    QVERIFY(QFileInfo::exists(stagingFile));

    QJsonObject abort;
    abort["type"] = "upload_abort";
    abort["senderClientId"] = QStringLiteral("ephemeral_sender");
    abort["senderPersistentClientId"] = senderId;
    abort["uploadId"] = uploadId;
    abort["canvasSessionId"] = canvasId;
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

    WebSocketClient socket;
    QSignalSpy connected(&socket, &WebSocketClient::connected);
    socket.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
    QScopedPointer<QWebSocket> peer(server.nextPendingConnection());
    QVERIFY(peer);

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
    uploads.setTargetClientId(QStringLiteral("target_client"));
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

QTEST_GUILESS_MAIN(UploadRemovalSecurityTest)
#include "tst_UploadRemovalSecurity.moc"
