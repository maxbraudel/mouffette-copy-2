#include "backend/domain/media/MediaFilePolicy.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <limits>

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

quint32 pngCrc32(const QByteArray& bytes) {
    quint32 crc = 0xffffffffU;
    for (const char byte : bytes) {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void appendPngChunk(QByteArray& png, const QByteArray& type, const QByteArray& payload) {
    appendBigEndian32(png, static_cast<quint32>(payload.size()));
    png.append(type);
    png.append(payload);
    appendBigEndian32(png, pngCrc32(type + payload));
}

QByteArray headerOnlyPng(quint32 width, quint32 height) {
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    QByteArray ihdr;
    appendBigEndian32(ihdr, width);
    appendBigEndian32(ihdr, height);
    ihdr.append(char(8)); // bit depth
    ihdr.append(char(6)); // RGBA
    ihdr.append(char(0)); // compression
    ihdr.append(char(0)); // filter
    ihdr.append(char(0)); // no interlace
    appendPngChunk(png, QByteArrayLiteral("IHDR"), ihdr);
    // An empty IDAT is enough for libpng to expose IHDR dimensions while the
    // policy still rejects the file before attempting a decompression.
    appendPngChunk(png, QByteArrayLiteral("IDAT"), QByteArray());
    appendPngChunk(png, QByteArrayLiteral("IEND"), QByteArray());
    return png;
}

QByteArray visualSampleEntry(const QByteArray& codec,
                             quint16 width = 16,
                             quint16 height = 16) {
    QByteArray payload(78, '\0');
    qToBigEndian<quint16>(1, reinterpret_cast<uchar*>(payload.data() + 6));
    qToBigEndian(width, reinterpret_cast<uchar*>(payload.data() + 24));
    qToBigEndian(height, reinterpret_cast<uchar*>(payload.data() + 26));
    qToBigEndian<quint16>(1, reinterpret_cast<uchar*>(payload.data() + 40));
    qToBigEndian<quint16>(24, reinterpret_cast<uchar*>(payload.data() + 74));
    return box(codec, payload);
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

bool zeroTopLevelBoxPayload(QByteArray& bytes, const QByteArray& requestedType)
{
    quint64 cursor = 0;
    const quint64 limit = static_cast<quint64>(bytes.size());
    while (cursor <= limit && limit - cursor >= 8) {
        const auto* header = reinterpret_cast<const uchar*>(bytes.constData() + cursor);
        const quint32 compactSize = qFromBigEndian<quint32>(header);
        quint64 headerSize = 8;
        quint64 boxSize = compactSize;
        if (compactSize == 1) {
            if (limit - cursor < 16) return false;
            boxSize = qFromBigEndian<quint64>(header + 8);
            headerSize = 16;
        } else if (compactSize == 0) {
            boxSize = limit - cursor;
        }
        if (boxSize < headerSize || boxSize > limit - cursor) return false;
        if (QByteArrayView(bytes).sliced(static_cast<qsizetype>(cursor + 4), 4)
            == QByteArrayView(requestedType)) {
            const quint64 payloadSize = boxSize - headerSize;
            if (payloadSize == 0
                || payloadSize > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
                return false;
            }
            const qsizetype payloadOffset =
                static_cast<qsizetype>(cursor + headerSize);
            std::fill(bytes.begin() + payloadOffset,
                      bytes.begin() + payloadOffset
                          + static_cast<qsizetype>(payloadSize),
                      '\0');
            return true;
        }
        cursor += boxSize;
    }
    return false;
}

QString fixturePath() {
    return QString::fromUtf8(TEST_VIDEO_FILE);
}

} // namespace

class MediaFilePolicyTest final : public QObject {
    Q_OBJECT

private slots:
    void detectsSourceMutationAgainstExpectedSha256() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("scene-source.png"));
        const QByteArray original = QByteArrayLiteral("original-scene-bytes");
        const QByteArray replacement = QByteArrayLiteral("modified-scene-bytes");
        QCOMPARE(original.size(), replacement.size());

        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(source.write(original), qint64(original.size()));
        source.close();
        const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
        QVERIFY(MediaFilePolicy::matchesSha256(path, expected));
        QVERIFY(!MediaFilePolicy::matchesSha256(path, expected.toUpper()));

        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(source.write(replacement), qint64(replacement.size()));
        source.close();
        QVERIFY(!MediaFilePolicy::matchesSha256(path, expected));
    }

    void acceptsRealMp4Fixture() {
        const QString fixture = fixturePath();
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }
        QCOMPARE(MediaFilePolicy::classifyLocalFile(fixture), MediaFilePolicy::Kind::Mp4Video);
        QVERIFY(MediaFilePolicy::isMp4Video(fixture));
        QVERIFY(MediaFilePolicy::isAcceptedLocalFile(fixture));
    }

    void acceptsMp4ExtensionWithoutCaseSensitivity() {
        const QString fixture = fixturePath();
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString upperCasePath = temporary.filePath(QStringLiteral("VIDEO.MP4"));
        QVERIFY(QFile::copy(fixture, upperCasePath));
        QCOMPARE(MediaFilePolicy::validateLocalFile(upperCasePath).kind,
                 MediaFilePolicy::Kind::Mp4Video);
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

    void rejectsUnknownVideoCodecEvenWithValidVisualSampleEntry() {
        QByteArray sampleDescriptionPayload(4, '\0');
        appendBigEndian32(sampleDescriptionPayload, 1);
        sampleDescriptionPayload.append(visualSampleEntry("zzzz"));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile unsupported(temporary.filePath(QStringLiteral("unsupported-codec.mp4")));
        QVERIFY(unsupported.open(QIODevice::WriteOnly));
        const QByteArray bytes = isoMediaWithSampleDescription("vide", sampleDescriptionPayload);
        QCOMPARE(unsupported.write(bytes), qint64(bytes.size()));
        unsupported.close();

        const auto validation = MediaFilePolicy::validateLocalFile(unsupported.fileName());
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::UnsupportedVideo);
        QCOMPARE(validation.errorCode, QStringLiteral("video_codec_not_supported"));
    }

    void rejectsIncompleteVisualSampleEntry() {
        QByteArray sampleDescriptionPayload(4, '\0');
        appendBigEndian32(sampleDescriptionPayload, 1);
        sampleDescriptionPayload.append(box("avc1", QByteArray(8, '\0')));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile incomplete(temporary.filePath(QStringLiteral("incomplete-video-entry.mp4")));
        QVERIFY(incomplete.open(QIODevice::WriteOnly));
        const QByteArray bytes = isoMediaWithSampleDescription("vide", sampleDescriptionPayload);
        QCOMPARE(incomplete.write(bytes), qint64(bytes.size()));
        incomplete.close();

        const auto validation = MediaFilePolicy::validateLocalFile(incomplete.fileName());
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::UnsupportedVideo);
        QCOMPARE(validation.errorCode, QStringLiteral("invalid_mp4_video"));
    }

    void rejectsDeclaredVideoTrackWithoutDecodableSamples() {
        QByteArray sampleDescriptionPayload(4, '\0');
        appendBigEndian32(sampleDescriptionPayload, 1);
        sampleDescriptionPayload.append(visualSampleEntry("avc1"));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile tableOnly(temporary.filePath(QStringLiteral("table-only.mp4")));
        QVERIFY(tableOnly.open(QIODevice::WriteOnly));
        const QByteArray bytes =
            isoMediaWithSampleDescription("vide", sampleDescriptionPayload);
        QCOMPARE(tableOnly.write(bytes), qint64(bytes.size()));
        tableOnly.close();

        // The ISO preflight sees a supported visual sample description and a
        // non-empty mdat. Acceptance must still fail because FFmpeg cannot
        // obtain a real frame from the missing sample/timing/chunk tables.
        const auto validation = MediaFilePolicy::validateLocalFile(tableOnly.fileName());
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::UnsupportedVideo);
        QVERIFY(validation.errorCode == QLatin1String("video_decode_failed")
                || validation.errorCode == QLatin1String("video_decode_timeout"));
    }

    void rejectsCorruptedMdatEvenWhenMovieTablesRemainValid() {
        const QString fixture = fixturePath();
        QVERIFY2(QFile::exists(fixture),
                 qPrintable(QStringLiteral("Required video fixture is missing: %1")
                                .arg(fixture)));
        QFile source(fixture);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QByteArray corruptedBytes = source.readAll();
        source.close();
        QVERIFY(!corruptedBytes.isEmpty());
        QVERIFY(zeroTopLevelBoxPayload(corruptedBytes, QByteArrayLiteral("mdat")));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile corrupted(temporary.filePath(QStringLiteral("corrupted-mdat.mp4")));
        QVERIFY(corrupted.open(QIODevice::WriteOnly));
        QCOMPARE(corrupted.write(corruptedBytes), qint64(corruptedBytes.size()));
        corrupted.close();

        // ftyp/moov/sample tables and all declared sizes are unchanged. Only
        // actual frame decoding can detect the destroyed media samples.
        const auto validation = MediaFilePolicy::validateLocalFile(corrupted.fileName());
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::UnsupportedVideo);
        QVERIFY(validation.errorCode == QLatin1String("video_decode_failed")
                || validation.errorCode == QLatin1String("video_decode_timeout"));
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
        sampleDescriptionPayload.append(visualSampleEntry("avc1"));

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

    void acceptsAllowedImageExtensionsCaseInsensitively_data() {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<QString>("extension");

        QTest::newRow("png") << QByteArrayLiteral("png") << QStringLiteral("PnG");
        QTest::newRow("jpg") << QByteArrayLiteral("jpeg") << QStringLiteral("JpG");
        QTest::newRow("jpeg") << QByteArrayLiteral("jpeg") << QStringLiteral("JpEg");
        QTest::newRow("webp") << QByteArrayLiteral("webp") << QStringLiteral("WeBp");
        QTest::newRow("avif") << QByteArrayLiteral("avif") << QStringLiteral("AvIf");
    }

    void acceptsAllowedImageExtensionsCaseInsensitively() {
        QFETCH(QByteArray, format);
        QFETCH(QString, extension);
        if (!QImageWriter::supportedImageFormats().contains(format)) {
            QSKIP(qPrintable(QStringLiteral("Qt image writer does not provide %1 in this build")
                                 .arg(QString::fromLatin1(format))));
        }

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString imagePath = temporary.filePath(QStringLiteral("image.%1").arg(extension));
        QImage image(16, 12, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::green);
        QVERIFY(image.save(imagePath, format.constData()));

        const auto validation = MediaFilePolicy::validateLocalFile(imagePath);
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::Image);
        QCOMPARE(validation.errorCode, QString());
    }

    void acceptsStaticAvifWhenRuntimeDecoderIsAvailable() {
        // 16x12 AV1 still image in an AVIF container. Keeping the fixture
        // inline makes decoder-policy coverage independent of optional tools.
        const QByteArray avif = QByteArray::fromBase64(QByteArrayLiteral(
            "AAAAIGZ0eXBhdmlmAAAAAGF2aWZtaWYxbWlhZk1BMUIAAAD5bWV0YQAAAAAAAAAvaGRscgAAAAAAAAAAcGljdAAAAAAAAAAAAAAAAFBpY3R1cmVIYW5kbGVyAAAAAA5waXRtAAAAAAABAAAAHmlsb2MAAAAARAAAAQABAAAAAQAAASEAAAAcAAAAKGlpbmYAAAAAAAEAAAAaaW5mZQIAAAAAAQAAYXYwMUNvbG9yAAAAAGppcHJwAAAAS2lwY28AAAAUaXNwZQAAAAAAAAAQAAAADAAAABBwaXhpAAAAAAMICAgAAAAMYXYxQ4EADAAAAAATY29scm5jbHgAAgACAAIAAAAAF2lwbWEAAAAAAAAAAQABBAECgwQAAAAkbWRhdAoGGAz+2wCAMhIYAAAAUAAAAACpjmy2qrHGvZA="));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile imageFile(temporary.filePath(QStringLiteral("still.AvIf")));
        QVERIFY(imageFile.open(QIODevice::WriteOnly));
        QCOMPARE(imageFile.write(avif), qint64(avif.size()));
        imageFile.close();

        QImageReader probe(imageFile.fileName());
        probe.setDecideFormatFromContent(true);
        const bool runtimeCanDecode = probe.canRead() && !probe.read().isNull();
        const auto validation = MediaFilePolicy::validateLocalFile(imageFile.fileName());
        if (!runtimeCanDecode) {
            QCOMPARE(validation.kind, MediaFilePolicy::Kind::Unsupported);
            QVERIFY(!validation.errorCode.isEmpty());
            return;
        }
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::Image);
        QCOMPARE(validation.imageSize, QSize(16, 12));
    }

    void rejectsImageFormatsOutsideAllowlist() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString imagePath = temporary.filePath(QStringLiteral("image.bmp"));
        QImage image(16, 12, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        QVERIFY(image.save(imagePath, "BMP"));
        const auto validation = MediaFilePolicy::validateLocalFile(imagePath);
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::Unsupported);
        QCOMPARE(validation.errorCode, QStringLiteral("unsupported_image_extension"));
    }

    void rejectsImageWhoseContentDoesNotMatchExtension() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString pngPath = temporary.filePath(QStringLiteral("source.png"));
        const QString renamedPath = temporary.filePath(QStringLiteral("renamed.jpg"));
        QImage image(16, 12, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::yellow);
        QVERIFY(image.save(pngPath, "PNG"));
        QVERIFY(QFile::copy(pngPath, renamedPath));
        const auto validation = MediaFilePolicy::validateLocalFile(renamedPath);
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::Unsupported);
        QCOMPARE(validation.errorCode, QStringLiteral("image_extension_mismatch"));
    }

    void rejectsMp4ContentRenamedAsAllowedImage() {
        const QString fixture = fixturePath();
        if (!QFile::exists(fixture)) {
            QSKIP(qPrintable(QStringLiteral("Optional real-video fixture is missing: %1").arg(fixture)));
        }
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString renamedPath = temporary.filePath(QStringLiteral("video.PNG"));
        QVERIFY(QFile::copy(fixture, renamedPath));
        const auto validation = MediaFilePolicy::validateLocalFile(renamedPath);
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::UnsupportedVideo);
        QCOMPARE(validation.errorCode, QStringLiteral("mp4_only"));
    }

    void rejectsTruncatedImageAfterHeaderProbeSucceeds() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString imagePath = temporary.filePath(QStringLiteral("truncated.png"));

        QImage image(128, 96, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.setPixelColor(x, y, QColor(x % 256, y % 256, (x * 17 + y * 29) % 256));
            }
        }
        QVERIFY(image.save(imagePath, "PNG"));

        QFile truncated(imagePath);
        QVERIFY(truncated.open(QIODevice::ReadWrite));
        QVERIFY(truncated.size() > 64);
        QVERIFY(truncated.resize(truncated.size() / 2));
        truncated.close();

        QImageReader headerProbe(imagePath);
        headerProbe.setDecideFormatFromContent(true);
        QVERIFY(headerProbe.canRead());
        QCOMPARE(headerProbe.size(), image.size());

        const auto validation = MediaFilePolicy::validateLocalFile(imagePath);
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::Unsupported);
        QCOMPARE(validation.errorCode, QStringLiteral("image_decode_failed"));
    }

    void rejectsAnimatedWebp() {
        // Deterministic two-frame 2x2 lossless WebP generated for this test.
        const QByteArray animatedWebp = QByteArray::fromBase64(
            QByteArrayLiteral("UklGRoQAAABXRUJQVlA4WAoAAAACAAAAAQAAAQAAQU5JTQYAAAD/////AABBTk1GKAAAAAAAAAAAAAEAAAEAAOgDAAJWUDhMDwAAAC8BQAAABxD1j/4HIqL/AQBBTk1GKAAAAAAAAAAAAAEAAAEAAOgDAABWUDhMDwAAAC8BQAAABxDR//4HIqL/AQA="));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile animated(temporary.filePath(QStringLiteral("animated.WeBp")));
        QVERIFY(animated.open(QIODevice::WriteOnly));
        QCOMPARE(animated.write(animatedWebp), qint64(animatedWebp.size()));
        animated.close();

        QImageReader probe(animated.fileName());
        probe.setDecideFormatFromContent(true);
        if (!probe.canRead()) {
            QSKIP("Qt image plugins in this build cannot decode WebP");
        }

        const auto validation = MediaFilePolicy::validateLocalFile(animated.fileName());
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::Unsupported);
        QCOMPARE(validation.errorCode, QStringLiteral("animated_image_not_supported"));
    }

    void rejectsAnimatedPngEvenWhenTheQtPluginExposesOnlyItsFirstFrame() {
        const QByteArray animatedPng = QByteArray::fromBase64(QByteArrayLiteral(
            "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAACXBIWXMAAAABAAAAAQBPJcTWAAAACGFjVEwAAAACAAAAAPONk3AAAAAaZmNUTAAAAAAAAAACAAAAAgAAAAAAAAAAAAEAAQAA5AEz4QAAABBJREFUeJxj/MMAAixgkgEADQQBAr9QFbMAAAAaZmNUTAAAAAEAAAACAAAAAgAAAAAAAAAAAAEAAQAAf3LZNQAAABZmZEFUAAAAAnicY2Rg+MvAwMDCAAYACxcBA+4aPD4AAAAASUVORK5CYII="));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile animated(temporary.filePath(QStringLiteral("animated.PnG")));
        QVERIFY(animated.open(QIODevice::WriteOnly));
        QCOMPARE(animated.write(animatedPng), qint64(animatedPng.size()));
        animated.close();

        const auto validation = MediaFilePolicy::validateLocalFile(animated.fileName());
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::Unsupported);
        QCOMPARE(validation.errorCode, QStringLiteral("animated_image_not_supported"));
    }

    void rejectsImagesAboveSixtyFourMegapixelsBeforeDecode() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile oversized(temporary.filePath(QStringLiteral("oversized.png")));
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        const QByteArray bytes = headerOnlyPng(8001, 8000);
        QCOMPARE(oversized.write(bytes), qint64(bytes.size()));
        oversized.close();

        QImageReader headerProbe(oversized.fileName());
        headerProbe.setDecideFormatFromContent(true);
        QVERIFY(headerProbe.canRead());
        QCOMPARE(headerProbe.size(), QSize(8001, 8000));

        const auto validation = MediaFilePolicy::validateLocalFile(oversized.fileName());
        QCOMPARE(validation.kind, MediaFilePolicy::Kind::Unsupported);
        QCOMPARE(validation.errorCode, QStringLiteral("image_pixel_limit_exceeded"));
    }

    void reportsDecodedImageBudget() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString imagePath = temporary.filePath(QStringLiteral("image.png"));
        QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::black);
        QVERIFY(image.save(imagePath));
        const auto validation = MediaFilePolicy::validateLocalFile(imagePath);
        QVERIFY(validation.accepted());
        QCOMPARE(validation.imageSize, QSize(32, 24));
        QCOMPARE(validation.decodedRgbaBytes, quint64(32 * 24 * 4));
    }

    void enforcesCumulativeDecodedRgbaBudgetWithoutOverflow() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString imagePath = temporary.filePath(QStringLiteral("budget.png"));
        QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::black);
        QVERIFY(image.save(imagePath));
        constexpr quint64 decodedBytes = 32ULL * 24ULL * 4ULL;

        const auto exactBoundary = MediaFilePolicy::validateLocalFile(
            imagePath, MediaFilePolicy::MaximumPreparedImageBytes - decodedBytes);
        QCOMPARE(exactBoundary.kind, MediaFilePolicy::Kind::Image);

        const auto oneByteOver = MediaFilePolicy::validateLocalFile(
            imagePath, MediaFilePolicy::MaximumPreparedImageBytes - decodedBytes + 1);
        QCOMPARE(oneByteOver.kind, MediaFilePolicy::Kind::Unsupported);
        QCOMPARE(oneByteOver.errorCode, QStringLiteral("decoded_rgba_budget_exceeded"));

        const auto overflowAttempt = MediaFilePolicy::validateLocalFile(
            imagePath, std::numeric_limits<quint64>::max());
        QCOMPARE(overflowAttempt.kind, MediaFilePolicy::Kind::Unsupported);
        QCOMPARE(overflowAttempt.errorCode, QStringLiteral("decoded_rgba_budget_exceeded"));
    }

    void scenePreparationFailsClosedAtCumulativeBoundaryWithoutLargeAllocation() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString imagePath = temporary.filePath(QStringLiteral("tiny.png"));
        QImage image(2, 2, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::black);
        QVERIFY(image.save(imagePath));
        constexpr quint64 decodedBytes = 2ULL * 2ULL * 4ULL;

        const QList<MediaFilePolicy::PreparationAsset> sceneAssets{
            {QStringLiteral("image-local"), imagePath, MediaFilePolicy::Kind::Image},
            {QStringLiteral("image-remote"), imagePath, MediaFilePolicy::Kind::Image}
        };

        const auto exactBoundary = MediaFilePolicy::validatePreparationAssets(
            sceneAssets,
            MediaFilePolicy::MaximumPreparedImageBytes - (2ULL * decodedBytes));
        QVERIFY(exactBoundary.accepted);
        QCOMPARE(exactBoundary.totalDecodedRgbaBytes,
                 MediaFilePolicy::MaximumPreparedImageBytes);

        const auto oneByteOver = MediaFilePolicy::validatePreparationAssets(
            sceneAssets,
            MediaFilePolicy::MaximumPreparedImageBytes - (2ULL * decodedBytes) + 1ULL);
        QVERIFY(!oneByteOver.accepted);
        QCOMPARE(oneByteOver.failedAssetId, QStringLiteral("image-remote"));
        QCOMPARE(oneByteOver.errorCode, QStringLiteral("decoded_rgba_budget_exceeded"));
        QVERIFY(oneByteOver.totalDecodedRgbaBytes
                <= MediaFilePolicy::MaximumPreparedImageBytes);
    }
};

// QMediaPlayer uses the same GUI-capable application context as Mouffette. The
// test remains headless through QT_QPA_PLATFORM=offscreen in CMake.
QTEST_MAIN(MediaFilePolicyTest)
#include "tst_MediaFilePolicy.moc"
