#include "backend/domain/media/MediaFilePolicy.h"

#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

namespace {

void appendBigEndian32(QByteArray& target, quint32 value) {
    char bytes[4];
    qToBigEndian(value, reinterpret_cast<uchar*>(bytes));
    target.append(bytes, 4);
}

QByteArray box(const QByteArray& type, const QByteArray& payload) {
    Q_ASSERT(type.size() == 4);
    QByteArray result;
    appendBigEndian32(result, static_cast<quint32>(8 + payload.size()));
    result.append(type);
    result.append(payload);
    return result;
}

QByteArray minimalIsoMedia(const QByteArray& handlerType) {
    QByteArray ftypPayload("isom", 4);
    appendBigEndian32(ftypPayload, 0);
    ftypPayload.append("isomiso2", 8);

    QByteArray handlerPayload(8, '\0'); // FullBox header + pre_defined.
    handlerPayload.append(handlerType);
    handlerPayload.append(QByteArray(12, '\0'));

    const QByteArray moov = box("moov", box("trak", box("mdia", box("hdlr", handlerPayload))));
    return box("ftyp", ftypPayload) + moov + box("mdat", QByteArray(8, '\1'));
}

QByteArray isoMediaWithSampleDescription(const QByteArray& handlerType,
                                         const QByteArray& sampleDescriptionPayload) {
    QByteArray ftypPayload("isom", 4);
    appendBigEndian32(ftypPayload, 0);
    ftypPayload.append("isomiso2", 8);

    QByteArray handlerPayload(8, '\0');
    handlerPayload.append(handlerType);
    handlerPayload.append(QByteArray(12, '\0'));

    const QByteArray sampleTable = box("stbl", box("stsd", sampleDescriptionPayload));
    const QByteArray mediaInformation = box("minf", sampleTable);
    const QByteArray media = box("mdia",
        box("mdhd", QByteArray(24, '\0'))
        + box("hdlr", handlerPayload)
        + mediaInformation);
    const QByteArray track = box("trak", box("tkhd", QByteArray(84, '\0')) + media);
    return box("ftyp", ftypPayload)
        + box("moov", track)
        + box("mdat", QByteArray(8, '\1'));
}

QString fixturePath() {
    return QString::fromUtf8(TEST_VIDEO_FILE);
}

} // namespace

class MediaFilePolicyTest final : public QObject {
    Q_OBJECT

private slots:
    void acceptsRealMp4Fixture() {
        const QString fixture = fixturePath();
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }
        QCOMPARE(MediaFilePolicy::classifyLocalFile(fixture), MediaFilePolicy::Kind::Mp4Video);
        QVERIFY(MediaFilePolicy::isMp4Video(fixture));
        QVERIFY(MediaFilePolicy::isAcceptedLocalFile(fixture));
    }

    void rejectsNonMp4VideoExtensionsEvenForIsoMedia() {
        const QString fixture = fixturePath();
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString renamed = temporary.filePath(QStringLiteral("video.mov"));
        QVERIFY(QFile::copy(fixture, renamed));
        QCOMPARE(MediaFilePolicy::classifyLocalFile(renamed), MediaFilePolicy::Kind::UnsupportedVideo);
        QVERIFY(!MediaFilePolicy::isAcceptedLocalFile(renamed));
    }

    void rejectsFakeMp4() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile fake(temporary.filePath(QStringLiteral("fake.mp4")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        QCOMPARE(fake.write("not an mp4 container"), qint64(20));
        fake.close();
        QCOMPARE(MediaFilePolicy::classifyLocalFile(fake.fileName()), MediaFilePolicy::Kind::UnsupportedVideo);
    }

    void rejectsEmbeddedFtypText() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile fake(temporary.filePath(QStringLiteral("embedded.mp4")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        QVERIFY(fake.write("arbitrary bytes containing ftyp but no ISO boxes") > 0);
        fake.close();
        QCOMPARE(MediaFilePolicy::classifyLocalFile(fake.fileName()), MediaFilePolicy::Kind::UnsupportedVideo);
    }

    void rejectsAudioOnlyIsoMediaRenamedMp4() {
        QByteArray sampleDescriptionPayload(4, '\0');
        appendBigEndian32(sampleDescriptionPayload, 1);
        sampleDescriptionPayload.append(box("mp4a", QByteArray(8, '\0')));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile audioOnly(temporary.filePath(QStringLiteral("audio.mp4")));
        QVERIFY(audioOnly.open(QIODevice::WriteOnly));
        const QByteArray bytes = isoMediaWithSampleDescription("soun", sampleDescriptionPayload);
        QCOMPARE(audioOnly.write(bytes), qint64(bytes.size()));
        audioOnly.close();
        QCOMPARE(MediaFilePolicy::classifyLocalFile(audioOnly.fileName()), MediaFilePolicy::Kind::UnsupportedVideo);
    }

    void rejectsHandlerOnlyFakeVideoTrack() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile fake(temporary.filePath(QStringLiteral("handler-only.mp4")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        const QByteArray bytes = minimalIsoMedia("vide");
        QCOMPARE(fake.write(bytes), qint64(bytes.size()));
        fake.close();
        QCOMPARE(MediaFilePolicy::classifyLocalFile(fake.fileName()), MediaFilePolicy::Kind::UnsupportedVideo);
        QVERIFY(!MediaFilePolicy::isAcceptedLocalFile(fake.fileName()));
    }

    void rejectsOutOfBoundsSampleDescriptionEntry() {
        QByteArray sampleDescriptionPayload(4, '\0'); // FullBox header.
        appendBigEndian32(sampleDescriptionPayload, 1); // entry_count.
        appendBigEndian32(sampleDescriptionPayload, 64); // Larger than the enclosing stsd payload.
        sampleDescriptionPayload.append("avc1", 4);
        sampleDescriptionPayload.append(QByteArray(8, '\0'));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile fake(temporary.filePath(QStringLiteral("unbounded-entry.mp4")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        const QByteArray bytes = isoMediaWithSampleDescription("vide", sampleDescriptionPayload);
        QCOMPARE(fake.write(bytes), qint64(bytes.size()));
        fake.close();
        QCOMPARE(MediaFilePolicy::classifyLocalFile(fake.fileName()), MediaFilePolicy::Kind::UnsupportedVideo);
        QVERIFY(!MediaFilePolicy::isAcceptedLocalFile(fake.fileName()));
    }

    void rejectsTruncatedMp4() {
        const QString fixture = fixturePath();
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString truncatedPath = temporary.filePath(QStringLiteral("truncated.mp4"));
        QVERIFY(QFile::copy(fixture, truncatedPath));
        QFile truncated(truncatedPath);
        QVERIFY(truncated.open(QIODevice::ReadWrite));
        QVERIFY(truncated.resize(std::max<qint64>(16, truncated.size() / 2)));
        truncated.close();
        QCOMPARE(MediaFilePolicy::classifyLocalFile(truncatedPath), MediaFilePolicy::Kind::UnsupportedVideo);
    }

    void rejectsPathologicalBoxCount() {
        QByteArray sampleDescriptionPayload(4, '\0');
        appendBigEndian32(sampleDescriptionPayload, 1);
        sampleDescriptionPayload.append(box("avc1", QByteArray(8, '\0')));

        const QByteArray validMedia = isoMediaWithSampleDescription("vide", sampleDescriptionPayload);
        const QByteArray tinyFreeBox = box("free", QByteArray());
        constexpr int pathologicalBoxCount = 70'000;
        QByteArray bytes;
        bytes.reserve(pathologicalBoxCount * tinyFreeBox.size() + validMedia.size());
        for (int index = 0; index < pathologicalBoxCount; ++index) {
            bytes.append(tinyFreeBox);
        }
        // This suffix is otherwise a structurally valid MP4. Acceptance would
        // prove that validation walked the entire attacker-controlled prefix.
        bytes.append(validMedia);

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile pathological(temporary.filePath(QStringLiteral("too-many-boxes.mp4")));
        QVERIFY(pathological.open(QIODevice::WriteOnly));
        QCOMPARE(pathological.write(bytes), qint64(bytes.size()));
        pathological.close();
        QCOMPARE(MediaFilePolicy::classifyLocalFile(pathological.fileName()),
                 MediaFilePolicy::Kind::UnsupportedVideo);
    }

    void keepsImageImportSupport() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString imagePath = temporary.filePath(QStringLiteral("image.png"));
        QImage image(16, 12, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::magenta);
        QVERIFY(image.save(imagePath));
        QCOMPARE(MediaFilePolicy::classifyLocalFile(imagePath), MediaFilePolicy::Kind::Image);
        QVERIFY(MediaFilePolicy::isAcceptedLocalFile(imagePath));
    }
};

QTEST_MAIN(MediaFilePolicyTest)
#include "tst_MediaFilePolicy.moc"
