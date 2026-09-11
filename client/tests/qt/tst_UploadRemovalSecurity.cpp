#include <QtTest>

#include "backend/files/FileManager.h"
#include "backend/files/LocalFileRepository.h"
#include "backend/network/RemoteFileTracker.h"
#include "backend/network/UploadManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>

class UploadRemovalSecurityTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void removeFileIsBoundToSenderAndCanvas();
    void removeAllIsBoundToSenderRoot();
    void removeAllFailureKeepsMappings();
    void idempotentRecoveryIsBoundToPersistentSender();

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

QTEST_GUILESS_MAIN(UploadRemovalSecurityTest)
#include "tst_UploadRemovalSecurity.moc"
