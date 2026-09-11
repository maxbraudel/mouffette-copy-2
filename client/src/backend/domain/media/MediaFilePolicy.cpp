#include "backend/domain/media/MediaFilePolicy.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QSet>
#include <QtEndian>
#include <limits>

namespace MediaFilePolicy {
namespace {

QString normalizedExtension(const QString& extension) {
    QString normalized = extension.trimmed().toLower();
    while (normalized.startsWith(QLatin1Char('.'))) {
        normalized.remove(0, 1);
    }
    return normalized;
}

struct IsoBox {
    quint64 offset = 0;
    quint64 size = 0;
    quint64 headerSize = 0;
    QByteArray type;
    bool extendsToLimit = false;

    quint64 payloadOffset() const { return offset + headerSize; }
    quint64 endOffset() const { return offset + size; }
};

// Import validation runs synchronously from UI-facing paths. Bound the number
// of container records we will inspect so a crafted file made of millions of
// eight-byte boxes cannot monopolize the event loop. Real MP4 files normally
// contain orders of magnitude fewer boxes, including fragmented media.
struct IsoParseBudget {
    static constexpr quint64 kMaximumBoxes = 65'536;
    quint64 remainingBoxes = kMaximumBoxes;
};

bool readExactly(QFile& file, quint64 offset, qsizetype size, QByteArray& bytes) {
    if (offset > static_cast<quint64>(std::numeric_limits<qint64>::max())
        || !file.seek(static_cast<qint64>(offset))) {
        return false;
    }
    bytes = file.read(size);
    return bytes.size() == size;
}

bool readBox(QFile& file,
             quint64 offset,
             quint64 limit,
             IsoBox& box,
             IsoParseBudget& budget) {
    if (budget.remainingBoxes == 0) return false;
    --budget.remainingBoxes;
    if (offset > limit || limit - offset < 8) return false;
    box = IsoBox{};

    QByteArray header;
    if (!readExactly(file, offset, 8, header)) return false;

    const quint32 compactSize = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(header.constData()));
    quint64 size = compactSize;
    quint64 headerSize = 8;
    if (compactSize == 1) {
        QByteArray extendedSize;
        if (limit - offset < 16 || !readExactly(file, offset + 8, 8, extendedSize)) return false;
        size = qFromBigEndian<quint64>(
            reinterpret_cast<const uchar*>(extendedSize.constData()));
        headerSize = 16;
    } else if (compactSize == 0) {
        size = limit - offset;
        box.extendsToLimit = true;
    }

    if (size < headerSize || size > limit - offset) return false;
    box.offset = offset;
    box.size = size;
    box.headerSize = headerSize;
    box.type = header.mid(4, 4);
    return true;
}

bool fullBoxHasMinimumPayload(QFile& file,
                              const IsoBox& box,
                              quint64 version0Minimum,
                              quint64 version1Minimum) {
    const quint64 payloadSize = box.size - box.headerSize;
    if (payloadSize < 4) return false;

    QByteArray fullBoxHeader;
    if (!readExactly(file, box.payloadOffset(), 4, fullBoxHeader)) return false;

    const quint8 version = static_cast<quint8>(fullBoxHeader.at(0));
    if (version == 0) return payloadSize >= version0Minimum;
    if (version == 1 && version1Minimum > 0) return payloadSize >= version1Minimum;
    return false;
}

bool isMp4Brand(const QByteArray& brand) {
    if (brand.size() != 4) return false;
    return brand == QByteArrayLiteral("isom")
        || brand.startsWith("iso")
        || brand.startsWith("mp4")
        || brand == QByteArrayLiteral("avc1")
        || brand == QByteArrayLiteral("dash")
        || brand == QByteArrayLiteral("M4V ")
        || brand == QByteArrayLiteral("M4VP")
        || brand == QByteArrayLiteral("MSNV")
        || brand == QByteArrayLiteral("F4V ")
        || brand == QByteArrayLiteral("cmfc")
        || brand == QByteArrayLiteral("cmfs");
}

bool hasMp4Brand(QFile& file, const IsoBox& ftyp) {
    const quint64 payloadSize = ftyp.size - ftyp.headerSize;
    if (payloadSize < 8 || payloadSize > 64 * 1024) return false;

    QByteArray payload;
    if (!readExactly(file, ftyp.payloadOffset(), static_cast<qsizetype>(payloadSize), payload)) {
        return false;
    }

    if (isMp4Brand(payload.first(4))) return true;
    for (qsizetype offset = 8; offset + 4 <= payload.size(); offset += 4) {
        if (isMp4Brand(payload.mid(offset, 4))) return true;
    }
    return false;
}

bool readHandlerType(QFile& file, const IsoBox& handlerBox, QByteArray& handlerType) {
    // HandlerBox is a version-0 FullBox followed by pre_defined, handler_type,
    // three reserved words, and an optional name.
    const quint64 payloadSize = handlerBox.size - handlerBox.headerSize;
    if (payloadSize < 24) return false;

    QByteArray handler;
    if (!readExactly(file, handlerBox.payloadOffset(), 12, handler)
        || static_cast<quint8>(handler.at(0)) != 0) {
        return false;
    }
    handlerType = handler.mid(8, 4);
    return handlerType.size() == 4;
}

bool sampleDescriptionHasBoundedEntry(QFile& file,
                                      const IsoBox& sampleDescription,
                                      IsoParseBudget& budget) {
    // SampleDescriptionBox: FullBox header + entry_count + sample entries.
    const quint64 payloadSize = sampleDescription.size - sampleDescription.headerSize;
    if (payloadSize < 8) return false;

    QByteArray descriptionHeader;
    if (!readExactly(file, sampleDescription.payloadOffset(), 8, descriptionHeader)
        || static_cast<quint8>(descriptionHeader.at(0)) != 0) {
        return false;
    }

    const quint32 entryCount = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(descriptionHeader.constData() + 4));
    if (entryCount == 0) return false;

    quint64 cursor = sampleDescription.payloadOffset() + 8;
    const quint64 limit = sampleDescription.endOffset();
    // Every SampleEntry contains an explicit Box header plus the common
    // reserved/data_reference_index prefix (8 bytes). This preflight both caps
    // the loop and rejects impossible attacker-controlled entry counts.
    if (entryCount > (limit - cursor) / 16) return false;

    for (quint32 index = 0; index < entryCount; ++index) {
        IsoBox entry;
        if (!readBox(file, cursor, limit, entry, budget)
            || entry.extendsToLimit
            || entry.size - entry.headerSize < 8) {
            return false;
        }
        cursor = entry.endOffset();
    }
    return cursor == limit;
}

bool sampleTableHasDescription(QFile& file,
                               const IsoBox& sampleTable,
                               IsoParseBudget& budget) {
    quint64 cursor = sampleTable.payloadOffset();
    bool hasDescription = false;
    while (cursor < sampleTable.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, sampleTable.endOffset(), child, budget)) return false;
        if (child.type == QByteArrayLiteral("stsd")) {
            if (hasDescription || !sampleDescriptionHasBoundedEntry(file, child, budget)) return false;
            hasDescription = true;
        }
        cursor = child.endOffset();
    }
    return hasDescription;
}

bool mediaInformationHasSampleTable(QFile& file,
                                    const IsoBox& mediaInformation,
                                    IsoParseBudget& budget) {
    quint64 cursor = mediaInformation.payloadOffset();
    bool hasSampleTable = false;
    while (cursor < mediaInformation.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, mediaInformation.endOffset(), child, budget)) return false;
        if (child.type == QByteArrayLiteral("stbl")) {
            if (hasSampleTable || !sampleTableHasDescription(file, child, budget)) return false;
            hasSampleTable = true;
        }
        cursor = child.endOffset();
    }
    return hasSampleTable;
}

bool mediaBoxIsStructuredVideo(QFile& file,
                               const IsoBox& media,
                               IsoParseBudget& budget) {
    quint64 cursor = media.payloadOffset();
    bool hasMediaHeader = false;
    bool hasHandler = false;
    bool hasVideoHandler = false;
    bool hasMediaInformation = false;
    while (cursor < media.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, media.endOffset(), child, budget)) return false;
        if (child.type == QByteArrayLiteral("mdhd")) {
            // MediaHeaderBox is 24 bytes for version 0 and 36 for version 1,
            // including its FullBox header.
            if (hasMediaHeader || !fullBoxHasMinimumPayload(file, child, 24, 36)) return false;
            hasMediaHeader = true;
        } else if (child.type == QByteArrayLiteral("hdlr")) {
            if (hasHandler) return false;
            hasHandler = true;
            QByteArray handlerType;
            if (!readHandlerType(file, child, handlerType)) return false;
            hasVideoHandler = handlerType == QByteArrayLiteral("vide");
        } else if (child.type == QByteArrayLiteral("minf")) {
            if (hasMediaInformation || !mediaInformationHasSampleTable(file, child, budget)) return false;
            hasMediaInformation = true;
        }
        cursor = child.endOffset();
    }
    return hasMediaHeader && hasHandler && hasVideoHandler && hasMediaInformation;
}

bool trackIsStructuredVideo(QFile& file,
                            const IsoBox& track,
                            IsoParseBudget& budget) {
    quint64 cursor = track.payloadOffset();
    bool hasTrackHeader = false;
    bool hasVideoMedia = false;
    bool hasMediaBox = false;
    while (cursor < track.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, track.endOffset(), child, budget)) return false;
        if (child.type == QByteArrayLiteral("tkhd")) {
            // TrackHeaderBox is 84 bytes for version 0 and 96 for version 1,
            // including its FullBox header.
            if (hasTrackHeader || !fullBoxHasMinimumPayload(file, child, 84, 96)) return false;
            hasTrackHeader = true;
        } else if (child.type == QByteArrayLiteral("mdia")) {
            if (hasMediaBox) return false;
            hasMediaBox = true;
            hasVideoMedia = mediaBoxIsStructuredVideo(file, child, budget);
        }
        cursor = child.endOffset();
    }
    return hasTrackHeader && hasMediaBox && hasVideoMedia;
}

bool movieBoxHasVideoTrack(QFile& file,
                           const IsoBox& movie,
                           IsoParseBudget& budget) {
    quint64 cursor = movie.payloadOffset();
    bool hasVideoTrack = false;
    while (cursor < movie.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, movie.endOffset(), child, budget)) return false;
        if (child.type == QByteArrayLiteral("trak") && trackIsStructuredVideo(file, child, budget)) {
            hasVideoTrack = true;
        }
        cursor = child.endOffset();
    }
    return hasVideoTrack;
}

bool isMp4ContainerWithVideo(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 16) return false;

    const quint64 fileSize = static_cast<quint64>(file.size());
    quint64 cursor = 0;
    IsoParseBudget budget;
    bool validFtyp = false;
    bool hasVideoTrack = false;
    bool hasMediaData = false;
    while (cursor < fileSize) {
        IsoBox box;
        if (!readBox(file, cursor, fileSize, box, budget)) return false;
        if (box.type == QByteArrayLiteral("ftyp")) {
            validFtyp = validFtyp || hasMp4Brand(file, box);
        } else if (box.type == QByteArrayLiteral("moov")) {
            hasVideoTrack = hasVideoTrack || movieBoxHasVideoTrack(file, box, budget);
        } else if (box.type == QByteArrayLiteral("mdat")) {
            hasMediaData = hasMediaData || box.size > box.headerSize;
        }
        cursor = box.endOffset();
    }

    return validFtyp && hasVideoTrack && hasMediaData;
}

bool contentIsMp4(const QString& path) {
    // Verify box boundaries, an MP4 brand, and a real video track. This is
    // deterministic across platforms, unlike their MIME databases.
    return isMp4ContainerWithVideo(path);
}

bool contentLooksLikeVideo(const QString& path) {
    QMimeDatabase database;
    return database.mimeTypeForFile(path, QMimeDatabase::MatchContent)
        .name().startsWith(QLatin1String("video/"));
}

} // namespace

bool isKnownVideoExtension(const QString& extension) {
    static const QSet<QString> extensions = {
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("m4v"),
        QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("avi"),
        QStringLiteral("wmv"), QStringLiteral("flv"), QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("3gp"), QStringLiteral("3g2"),
        QStringLiteral("ts"), QStringLiteral("m2ts"), QStringLiteral("mts")
    };
    return extensions.contains(normalizedExtension(extension));
}

Kind classifyLocalFile(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        return Kind::Unsupported;
    }

    const QString extension = normalizedExtension(info.suffix());
    if (extension == QLatin1String("mp4")) {
        return contentIsMp4(info.absoluteFilePath()) ? Kind::Mp4Video : Kind::UnsupportedVideo;
    }

    if (isKnownVideoExtension(extension) || contentLooksLikeVideo(info.absoluteFilePath())) {
        return Kind::UnsupportedVideo;
    }

    QImageReader reader(info.absoluteFilePath());
    return reader.canRead() ? Kind::Image : Kind::Unsupported;
}

bool isAcceptedLocalFile(const QString& path) {
    const Kind kind = classifyLocalFile(path);
    return kind == Kind::Image || kind == Kind::Mp4Video;
}

bool isMp4Video(const QString& path) {
    return classifyLocalFile(path) == Kind::Mp4Video;
}

} // namespace MediaFilePolicy
