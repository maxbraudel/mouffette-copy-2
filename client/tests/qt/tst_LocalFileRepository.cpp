#include <QFile>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QDateTime>
#include <QtTest>

#include "backend/files/LocalFileRepository.h"

class LocalFileRepositoryTest final : public QObject {
    Q_OBJECT

private slots:
    void init() {
        LocalFileRepository::instance().clear();
    }

    void cleanup() {
        LocalFileRepository::instance().clear();
    }

    void survivingDuplicateRemainsUsable_data() {
        QTest::addColumn<int>("registration");
        QTest::addColumn<bool>("replaceFirst");
        for (int registration = 0; registration < 3; ++registration) {
            QTest::newRow(qPrintable(QStringLiteral("api-%1-deleted").arg(registration)))
                << registration << false;
            QTest::newRow(qPrintable(QStringLiteral("api-%1-replaced").arg(registration)))
                << registration << true;
        }
    }

    void survivingDuplicateRemainsUsable() {
        QFETCH(int, registration);
        QFETCH(bool, replaceFirst);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString first = temporary.filePath("first.mp4");
        const QString second = temporary.filePath("second.mp4");
        QFile file(first);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("AAAA", 4), qint64(4));
        file.close();
        QVERIFY(QFile::copy(first, second));
        const QString id = QString::fromLatin1(QCryptographicHash::hash(
            QByteArrayLiteral("AAAA"), QCryptographicHash::Sha256).toHex());
        auto& repository = LocalFileRepository::instance();
        for (const auto& path : {first, second}) {
            if (registration == 0) repository.registerVerifiedLocalFile(id, path);
            else if (registration == 1) QCOMPARE(repository.getOrCreateFileId(path), id);
            else repository.registerReceivedFilePath(id, path);
        }
        QCOMPARE(repository.getFilePathForId(id), QFileInfo(first).canonicalFilePath());
        const QString verifiedSecond = QFileInfo(second).canonicalFilePath();
        if (replaceFirst) {
            const auto modified = QFileInfo(first).lastModified();
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("BBBB", 4), qint64(4));
            QVERIFY(file.setFileTime(modified.addSecs(5), QFileDevice::FileModificationTime));
            file.close();
        } else {
            QVERIFY(QFile::remove(first));
        }
        // This lookup must succeed without another registration or hash pass.
        QCOMPARE(repository.getFilePathForId(id), verifiedSecond);
        QVERIFY(repository.hasFileId(id));
        QVERIFY(repository.getAllFileIds().contains(id));
        if (replaceFirst) {
            const QString changedId = repository.getOrCreateFileId(first);
            QVERIFY(!changedId.isEmpty());
            QVERIFY(changedId != id);
            QCOMPARE(repository.getFilePathForId(id), verifiedSecond);
            QCOMPARE(repository.getFilePathForId(changedId), QFileInfo(first).canonicalFilePath());
        }
        QVERIFY(QFile::remove(second));
        QVERIFY(repository.getFilePathForId(id).isEmpty());
    }

    void retiringIdentityRemovesEveryDuplicate_data() {
        QTest::addColumn<bool>("received");
        QTest::newRow("local") << false;
        QTest::newRow("received") << true;
    }

    void retiringIdentityRemovesEveryDuplicate() {
        QFETCH(bool, received);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString first = temporary.filePath("first.png");
        const QString second = temporary.filePath("second.png");
        QFile file(first);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("same", 4), qint64(4));
        file.close();
        QVERIFY(QFile::copy(first, second));
        auto& repository = LocalFileRepository::instance();
        const QString id = repository.getOrCreateFileId(first);
        repository.registerVerifiedLocalFile(id, second);
        QVERIFY(repository.getFileIdsUnderPathPrefix(QFileInfo(second).canonicalFilePath()).contains(id));
        if (received) repository.removeReceivedFileMapping(id);
        else repository.removeFileMapping(id);
        QVERIFY(!repository.hasFileId(id));
        QVERIFY(repository.getFilePathForId(id).isEmpty());
        QVERIFY(repository.getFileIdsUnderPathPrefix(QFileInfo(temporary.path()).canonicalFilePath()).isEmpty());
        QVERIFY(repository.getAllFileIds().isEmpty());
        // Re-register only the first path. Its old duplicate must not reappear
        // as a fallback after the identity was explicitly retired.
        repository.registerVerifiedLocalFile(id, first);
        QVERIFY(QFile::remove(first));
        QVERIFY(repository.getFilePathForId(id).isEmpty());
    }

    void replacementAtSamePathAndSizeRotatesIdentity() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("scene-video.mp4"));

        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("AAAA", 4), qint64(4));
        file.close();

        LocalFileRepository& repository = LocalFileRepository::instance();
        const QString firstId = repository.getOrCreateFileId(path);
        QVERIFY(!firstId.isEmpty());
        QCOMPARE(firstId,
                 QString::fromLatin1(QCryptographicHash::hash(
                     QByteArrayLiteral("AAAA"), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(repository.getOrCreateFileId(path), firstId);

        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write("BBBB", 4), qint64(4));
        file.close();

        const QString replacementId = repository.getOrCreateFileId(path);
        QVERIFY(!replacementId.isEmpty());
        QVERIFY(replacementId != firstId);
        QVERIFY(repository.getFilePathForId(firstId).isEmpty());
        QCOMPARE(repository.getFilePathForId(replacementId), QFileInfo(path).canonicalFilePath());
    }
};

QTEST_MAIN(LocalFileRepositoryTest)
#include "tst_LocalFileRepository.moc"
