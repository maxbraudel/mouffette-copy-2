#include <QFile>
#include <QTemporaryDir>
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
