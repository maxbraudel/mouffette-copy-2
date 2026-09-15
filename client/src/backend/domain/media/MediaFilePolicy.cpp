#include "backend/domain/media/MediaFilePolicy.h"
#include "backend/config/AppConfig.h"
#include "MediaFormatContract.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMediaFormat>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtEndian>
#include <limits>
#include <optional>

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

enum class VideoFrameProbeResult {
    Decoded,
    Failed,
    TimedOut
};

struct VideoFrameProbe {
    VideoFrameProbeResult result = VideoFrameProbeResult::Failed;
    QSize frameSize;
    QImage firstFrame;
};

// QMediaFormat only reports advertised codec capability.  It cannot prove
// that this concrete sample table and mdat payload can be consumed.  Use the
// same Qt Multimedia pipeline as rendering (QT_MEDIA_BACKEND=ffmpeg in the
// application configuration) and require a real decoded frame.  Every QObject
// is stack-owned, the source is detached before returning, and the nested loop
// has a hard deadline so corrupt media cannot leave a decoder or file handle
// alive indefinitely.
VideoFrameProbe decodeFirstMp4Frame(const QString& path)
{
    if (!QCoreApplication::instance()
        || !QAbstractEventDispatcher::instance(QThread::currentThread())) {
        return {};
    }

    bool decoded = false;
    bool failed = false;
    bool timedOut = false;
    QSize frameSize;
    QImage firstFrame;
    QEventLoop eventLoop;
    QTimer deadline;
    deadline.setSingleShot(true);

    // Destruction is deliberately ordered: the player releases its backend
    // and source before the sink goes away.
    QVideoSink sink;
    QMediaPlayer player;
    QObject::connect(&sink, &QVideoSink::videoFrameChanged, &eventLoop,
                     [&](const QVideoFrame& frame) {
        if (!frame.isValid()) return;
        firstFrame = frame.toImage();
        frameSize = firstFrame.isNull() ? frame.size() : firstFrame.size();
        decoded = true;
        eventLoop.quit();
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &eventLoop,
                     [&](QMediaPlayer::Error error, const QString&) {
        if (error == QMediaPlayer::NoError) return;
        failed = true;
        eventLoop.quit();
    });
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &eventLoop,
                     [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::InvalidMedia) {
            failed = true;
            eventLoop.quit();
        } else if (status == QMediaPlayer::EndOfMedia && !decoded) {
            // Let an already queued final videoFrameChanged win before
            // declaring a one-frame stream undecodable.
            QTimer::singleShot(0, &eventLoop, [&] {
                if (!decoded) failed = true;
                eventLoop.quit();
            });
        }
    });
    QObject::connect(&deadline, &QTimer::timeout, &eventLoop, [&] {
        timedOut = true;
        eventLoop.quit();
    });

    deadline.start(AppConfig::instance().mediaProbeTimeoutMs());
    player.setVideoOutput(&sink);
    player.setSource(QUrl::fromLocalFile(path));
    player.play();
    if (!decoded && !failed && !timedOut) {
        eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
    }

    deadline.stop();
    player.stop();
    player.setVideoOutput(nullptr);
    player.setSource(QUrl());
    if (decoded) {
        return {VideoFrameProbeResult::Decoded, frameSize, firstFrame};
    }
    return {timedOut ? VideoFrameProbeResult::TimedOut
                     : VideoFrameProbeResult::Failed, {}, {}};
}

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

enum class VideoTrackValidation {
    NotVideo,
    Invalid,
    UnsupportedCodec,
    Supported
};

std::optional<QMediaFormat::VideoCodec> videoCodecForSampleEntry(const QByteArray& entryType) {
    if (entryType == QByteArrayLiteral("avc1")
        || entryType == QByteArrayLiteral("avc2")
        || entryType == QByteArrayLiteral("avc3")
        || entryType == QByteArrayLiteral("avc4")) {
        return QMediaFormat::VideoCodec::H264;
    }
    if (entryType == QByteArrayLiteral("hvc1")
        || entryType == QByteArrayLiteral("hev1")) {
        return QMediaFormat::VideoCodec::H265;
    }
    if (entryType == QByteArrayLiteral("av01")) {
        return QMediaFormat::VideoCodec::AV1;
    }
    if (entryType == QByteArrayLiteral("vp08")) {
        return QMediaFormat::VideoCodec::VP8;
    }
    if (entryType == QByteArrayLiteral("vp09")) {
        return QMediaFormat::VideoCodec::VP9;
    }
    if (entryType == QByteArrayLiteral("mp4v")) {
        return QMediaFormat::VideoCodec::MPEG4;
    }
    if (entryType == QByteArrayLiteral("mp1v")) {
        return QMediaFormat::VideoCodec::MPEG1;
    }
    if (entryType == QByteArrayLiteral("mp2v")) {
        return QMediaFormat::VideoCodec::MPEG2;
    }
    if (entryType == QByteArrayLiteral("jpeg")
        || entryType == QByteArrayLiteral("mjpg")
        || entryType == QByteArrayLiteral("mjpa")
        || entryType == QByteArrayLiteral("mjpb")) {
        return QMediaFormat::VideoCodec::MotionJPEG;
    }
    return std::nullopt;
}

bool decoderSupportsMp4Codec(QMediaFormat::VideoCodec codec) {
    QMediaFormat capabilities(QMediaFormat::MPEG4);
    return capabilities.supportedVideoCodecs(QMediaFormat::Decode).contains(codec);
}

VideoTrackValidation validateVideoSampleDescription(QFile& file,
                                                    const IsoBox& sampleDescription,
                                                    IsoParseBudget& budget) {
    // SampleDescriptionBox: FullBox header + entry_count + sample entries.
    const quint64 payloadSize = sampleDescription.size - sampleDescription.headerSize;
    if (payloadSize < 8) return VideoTrackValidation::Invalid;

    QByteArray descriptionHeader;
    if (!readExactly(file, sampleDescription.payloadOffset(), 8, descriptionHeader)
        || static_cast<quint8>(descriptionHeader.at(0)) != 0) {
        return VideoTrackValidation::Invalid;
    }

    const quint32 entryCount = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(descriptionHeader.constData() + 4));
    if (entryCount == 0) return VideoTrackValidation::Invalid;

    quint64 cursor = sampleDescription.payloadOffset() + 8;
    const quint64 limit = sampleDescription.endOffset();
    // Every SampleEntry contains an explicit Box header plus the common
    // reserved/data_reference_index prefix (8 bytes). This preflight both caps
    // the loop and rejects impossible attacker-controlled entry counts.
    if (entryCount > (limit - cursor) / 16) return VideoTrackValidation::Invalid;

    bool allCodecsSupported = true;
    for (quint32 index = 0; index < entryCount; ++index) {
        IsoBox entry;
        if (!readBox(file, cursor, limit, entry, budget)
            || entry.extendsToLimit
            // ISO/IEC 14496-12 VisualSampleEntry has 78 mandatory payload
            // bytes before any codec-specific child boxes.
            || entry.size - entry.headerSize < 78) {
            return VideoTrackValidation::Invalid;
        }

        QByteArray dimensions;
        if (!readExactly(file, entry.payloadOffset() + 24, 4, dimensions)) {
            return VideoTrackValidation::Invalid;
        }
        const quint16 width = qFromBigEndian<quint16>(
            reinterpret_cast<const uchar*>(dimensions.constData()));
        const quint16 height = qFromBigEndian<quint16>(
            reinterpret_cast<const uchar*>(dimensions.constData() + 2));
        if (width == 0 || height == 0) return VideoTrackValidation::Invalid;

        const auto codec = videoCodecForSampleEntry(entry.type);
        if (!codec.has_value() || !decoderSupportsMp4Codec(*codec)) {
            allCodecsSupported = false;
        }
        cursor = entry.endOffset();
    }
    if (cursor != limit) return VideoTrackValidation::Invalid;
    return allCodecsSupported
        ? VideoTrackValidation::Supported
        : VideoTrackValidation::UnsupportedCodec;
}

VideoTrackValidation validateVideoSampleTable(QFile& file,
                                              const IsoBox& sampleTable,
                                              IsoParseBudget& budget) {
    quint64 cursor = sampleTable.payloadOffset();
    bool hasDescription = false;
    VideoTrackValidation descriptionStatus = VideoTrackValidation::Invalid;
    while (cursor < sampleTable.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, sampleTable.endOffset(), child, budget)) {
            return VideoTrackValidation::Invalid;
        }
        if (child.type == QByteArrayLiteral("stsd")) {
            if (hasDescription) return VideoTrackValidation::Invalid;
            hasDescription = true;
            descriptionStatus = validateVideoSampleDescription(file, child, budget);
        }
        cursor = child.endOffset();
    }
    return hasDescription ? descriptionStatus : VideoTrackValidation::Invalid;
}

VideoTrackValidation validateVideoMediaInformation(QFile& file,
                                                   const IsoBox& mediaInformation,
                                                   IsoParseBudget& budget) {
    quint64 cursor = mediaInformation.payloadOffset();
    bool hasSampleTable = false;
    VideoTrackValidation sampleTableStatus = VideoTrackValidation::Invalid;
    while (cursor < mediaInformation.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, mediaInformation.endOffset(), child, budget)) {
            return VideoTrackValidation::Invalid;
        }
        if (child.type == QByteArrayLiteral("stbl")) {
            if (hasSampleTable) return VideoTrackValidation::Invalid;
            hasSampleTable = true;
            sampleTableStatus = validateVideoSampleTable(file, child, budget);
        }
        cursor = child.endOffset();
    }
    return hasSampleTable ? sampleTableStatus : VideoTrackValidation::Invalid;
}

VideoTrackValidation validateMediaBox(QFile& file,
                                      const IsoBox& media,
                                      IsoParseBudget& budget) {
    quint64 cursor = media.payloadOffset();
    bool hasMediaHeader = false;
    bool hasHandler = false;
    bool hasMediaInformation = false;
    QByteArray handlerType;
    IsoBox mediaInformation;
    while (cursor < media.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, media.endOffset(), child, budget)) {
            return VideoTrackValidation::Invalid;
        }
        if (child.type == QByteArrayLiteral("mdhd")) {
            // MediaHeaderBox is 24 bytes for version 0 and 36 for version 1,
            // including its FullBox header.
            if (hasMediaHeader || !fullBoxHasMinimumPayload(file, child, 24, 36)) {
                return VideoTrackValidation::Invalid;
            }
            hasMediaHeader = true;
        } else if (child.type == QByteArrayLiteral("hdlr")) {
            if (hasHandler) return VideoTrackValidation::Invalid;
            hasHandler = true;
            if (!readHandlerType(file, child, handlerType)) {
                return VideoTrackValidation::Invalid;
            }
        } else if (child.type == QByteArrayLiteral("minf")) {
            if (hasMediaInformation) return VideoTrackValidation::Invalid;
            hasMediaInformation = true;
            mediaInformation = child;
        }
        cursor = child.endOffset();
    }
    if (!hasMediaHeader || !hasHandler || !hasMediaInformation) {
        return VideoTrackValidation::Invalid;
    }
    if (handlerType != QByteArrayLiteral("vide")) {
        return VideoTrackValidation::NotVideo;
    }
    return validateVideoMediaInformation(file, mediaInformation, budget);
}

VideoTrackValidation validateTrack(QFile& file,
                                   const IsoBox& track,
                                   IsoParseBudget& budget) {
    quint64 cursor = track.payloadOffset();
    bool hasTrackHeader = false;
    bool hasMediaBox = false;
    VideoTrackValidation mediaStatus = VideoTrackValidation::Invalid;
    while (cursor < track.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, track.endOffset(), child, budget)) {
            return VideoTrackValidation::Invalid;
        }
        if (child.type == QByteArrayLiteral("tkhd")) {
            // TrackHeaderBox is 84 bytes for version 0 and 96 for version 1,
            // including its FullBox header.
            if (hasTrackHeader || !fullBoxHasMinimumPayload(file, child, 84, 96)) {
                return VideoTrackValidation::Invalid;
            }
            hasTrackHeader = true;
        } else if (child.type == QByteArrayLiteral("mdia")) {
            if (hasMediaBox) return VideoTrackValidation::Invalid;
            hasMediaBox = true;
            mediaStatus = validateMediaBox(file, child, budget);
        }
        cursor = child.endOffset();
    }
    if (!hasTrackHeader || !hasMediaBox) return VideoTrackValidation::Invalid;
    return mediaStatus;
}

VideoTrackValidation validateMovieBox(QFile& file,
                                      const IsoBox& movie,
                                      IsoParseBudget& budget) {
    quint64 cursor = movie.payloadOffset();
    bool hasSupportedVideoTrack = false;
    bool hasUnsupportedVideoTrack = false;
    while (cursor < movie.endOffset()) {
        IsoBox child;
        if (!readBox(file, cursor, movie.endOffset(), child, budget)) {
            return VideoTrackValidation::Invalid;
        }
        if (child.type == QByteArrayLiteral("trak")) {
            const VideoTrackValidation status = validateTrack(file, child, budget);
            if (status == VideoTrackValidation::Invalid) {
                return VideoTrackValidation::Invalid;
            }
            if (status == VideoTrackValidation::Supported) {
                hasSupportedVideoTrack = true;
            } else if (status == VideoTrackValidation::UnsupportedCodec) {
                hasUnsupportedVideoTrack = true;
            }
        }
        cursor = child.endOffset();
    }
    if (hasUnsupportedVideoTrack) return VideoTrackValidation::UnsupportedCodec;
    return hasSupportedVideoTrack
        ? VideoTrackValidation::Supported
        : VideoTrackValidation::Invalid;
}

VideoTrackValidation validateMp4Container(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 16) {
        return VideoTrackValidation::Invalid;
    }

    const quint64 fileSize = static_cast<quint64>(file.size());
    quint64 cursor = 0;
    IsoParseBudget budget;
    bool validFtyp = false;
    bool hasSupportedVideoTrack = false;
    bool hasUnsupportedVideoTrack = false;
    bool hasMediaData = false;
    while (cursor < fileSize) {
        IsoBox box;
        if (!readBox(file, cursor, fileSize, box, budget)) {
            return VideoTrackValidation::Invalid;
        }
        if (box.type == QByteArrayLiteral("ftyp")) {
            validFtyp = validFtyp || hasMp4Brand(file, box);
        } else if (box.type == QByteArrayLiteral("moov")) {
            const VideoTrackValidation status = validateMovieBox(file, box, budget);
            if (status == VideoTrackValidation::Invalid) {
                return VideoTrackValidation::Invalid;
            }
            hasSupportedVideoTrack = hasSupportedVideoTrack
                || status == VideoTrackValidation::Supported;
            hasUnsupportedVideoTrack = hasUnsupportedVideoTrack
                || status == VideoTrackValidation::UnsupportedCodec;
        } else if (box.type == QByteArrayLiteral("mdat")) {
            hasMediaData = hasMediaData || box.size > box.headerSize;
        }
        cursor = box.endOffset();
    }

    if (!validFtyp || !hasMediaData) return VideoTrackValidation::Invalid;
    if (hasUnsupportedVideoTrack) return VideoTrackValidation::UnsupportedCodec;
    return hasSupportedVideoTrack
        ? VideoTrackValidation::Supported
        : VideoTrackValidation::Invalid;
}

bool contentLooksLikeVideo(const QString& path) {
    QMimeDatabase database;
    return database.mimeTypeForFile(path, QMimeDatabase::MatchContent)
        .name().startsWith(QLatin1String("video/"));
}

bool imageFormatMatchesExtension(const QByteArray& format, const QString& extension) {
    const QByteArray normalizedFormat = format.trimmed().toLower();
    if (extension == QLatin1String("jpg") || extension == QLatin1String("jpeg")) {
        return normalizedFormat == QByteArrayLiteral("jpg")
            || normalizedFormat == QByteArrayLiteral("jpeg");
    }
    return normalizedFormat == extension.toLatin1();
}

bool pngDeclaresAnimation(const QString& path) {
    QFile file(path);
    QByteArray signature;
    if (!file.open(QIODevice::ReadOnly)
        || !readExactly(file, 0, 8, signature)
        || signature != QByteArray("\x89PNG\r\n\x1a\n", 8)) {
        return false;
    }

    const quint64 fileSize = static_cast<quint64>(file.size());
    quint64 cursor = 8;
    quint64 chunksRemaining = IsoParseBudget::kMaximumBoxes;
    while (chunksRemaining-- > 0 && cursor <= fileSize && fileSize - cursor >= 12) {
        QByteArray header;
        if (!readExactly(file, cursor, 8, header)) return false;
        const quint64 payloadSize = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar*>(header.constData()));
        if (payloadSize > fileSize - cursor - 12) return false;
        const QByteArray type = header.mid(4, 4);
        if (type == QByteArrayLiteral("acTL")) return true;
        cursor += 12 + payloadSize;
        if (type == QByteArrayLiteral("IEND")) return false;
    }
    return false;
}

bool webpDeclaresAnimation(const QString& path) {
    QFile file(path);
    QByteArray header;
    if (!file.open(QIODevice::ReadOnly)
        || !readExactly(file, 0, 12, header)
        || header.first(4) != QByteArrayLiteral("RIFF")
        || header.mid(8, 4) != QByteArrayLiteral("WEBP")) {
        return false;
    }

    const quint64 declaredSize = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(header.constData() + 4));
    const quint64 fileSize = static_cast<quint64>(file.size());
    if (declaredSize < 4 || declaredSize > fileSize - 8) return false;
    const quint64 limit = declaredSize + 8;

    quint64 cursor = 12;
    quint64 chunksRemaining = IsoParseBudget::kMaximumBoxes;
    while (chunksRemaining-- > 0 && cursor <= limit && limit - cursor >= 8) {
        QByteArray chunkHeader;
        if (!readExactly(file, cursor, 8, chunkHeader)) return false;
        const QByteArray type = chunkHeader.first(4);
        const quint64 payloadSize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(chunkHeader.constData() + 4));
        const quint64 paddedPayloadSize = payloadSize + (payloadSize & 1ULL);
        if (paddedPayloadSize > limit - cursor - 8) return false;
        if (type == QByteArrayLiteral("ANIM") || type == QByteArrayLiteral("ANMF")) {
            return true;
        }
        if (type == QByteArrayLiteral("VP8X") && payloadSize >= 1) {
            QByteArray flags;
            if (!readExactly(file, cursor + 8, 1, flags)) return false;
            if ((static_cast<quint8>(flags.at(0)) & 0x02U) != 0) return true;
        }
        cursor += 8 + paddedPayloadSize;
    }
    return false;
}

bool avifDeclaresSequence(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 16) return false;

    const quint64 fileSize = static_cast<quint64>(file.size());
    quint64 cursor = 0;
    IsoParseBudget budget;
    while (cursor < fileSize) {
        IsoBox candidate;
        if (!readBox(file, cursor, fileSize, candidate, budget)) return false;
        if (candidate.type == QByteArrayLiteral("ftyp")) {
            const quint64 payloadSize = candidate.size - candidate.headerSize;
            if (payloadSize < 8 || payloadSize > 64 * 1024) return false;
            QByteArray payload;
            if (!readExactly(file, candidate.payloadOffset(),
                             static_cast<qsizetype>(payloadSize), payload)) {
                return false;
            }
            if (payload.first(4) == QByteArrayLiteral("avis")) return true;
            for (qsizetype offset = 8; offset + 4 <= payload.size(); offset += 4) {
                if (payload.mid(offset, 4) == QByteArrayLiteral("avis")) return true;
            }
            return false;
        }
        cursor = candidate.endOffset();
    }
    return false;
}

bool jpegDeclaresMultiplePictures(const QString& path) {
    QFile file(path);
    QByteArray signature;
    if (!file.open(QIODevice::ReadOnly)
        || !readExactly(file, 0, 2, signature)
        || signature != QByteArray("\xff\xd8", 2)) {
        return false;
    }

    quint64 cursor = 2;
    const quint64 fileSize = static_cast<quint64>(file.size());
    quint64 segmentsRemaining = IsoParseBudget::kMaximumBoxes;
    while (segmentsRemaining-- > 0 && cursor < fileSize) {
        QByteArray markerPrefix;
        if (!readExactly(file, cursor, 2, markerPrefix)
            || static_cast<quint8>(markerPrefix.at(0)) != 0xffU) {
            return false;
        }
        const quint8 marker = static_cast<quint8>(markerPrefix.at(1));
        cursor += 2;
        if (marker == 0xdaU || marker == 0xd9U) return false; // SOS or EOI.
        if (marker == 0x00U || marker == 0xffU
            || marker == 0xd8U || (marker >= 0xd0U && marker <= 0xd7U)) {
            continue;
        }
        QByteArray lengthBytes;
        if (!readExactly(file, cursor, 2, lengthBytes)) return false;
        const quint16 segmentLength = qFromBigEndian<quint16>(
            reinterpret_cast<const uchar*>(lengthBytes.constData()));
        if (segmentLength < 2 || segmentLength > fileSize - cursor) return false;
        if (marker == 0xe2U && segmentLength >= 6) { // APP2/Multi-Picture Format.
            QByteArray identifier;
            if (!readExactly(file, cursor + 2, 4, identifier)) return false;
            if (identifier == QByteArray("MPF\0", 4)) return true;
        }
        cursor += segmentLength;
    }
    return false;
}

bool contentDeclaresMultipleImages(const QString& path, const QString& extension) {
    if (extension == QLatin1String("png")) return pngDeclaresAnimation(path);
    if (extension == QLatin1String("webp")) return webpDeclaresAnimation(path);
    if (extension == QLatin1String("avif")) return avifDeclaresSequence(path);
    if (extension == QLatin1String("jpg") || extension == QLatin1String("jpeg")) {
        return jpegDeclaresMultiplePictures(path);
    }
    return false;
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

ValidationResult validateLocalFile(const QString& path, quint64 preparedImageBytes) {
    ValidationResult result;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        result.errorCode = QStringLiteral("file_unreadable");
        return result;
    }

    const QString extension = normalizedExtension(info.suffix());
    if (extension == QLatin1String("mp4")) {
        const VideoTrackValidation mp4Status = validateMp4Container(info.absoluteFilePath());
        result.kind = Kind::UnsupportedVideo;
        if (mp4Status == VideoTrackValidation::UnsupportedCodec) {
            result.errorCode = QStringLiteral("video_codec_not_supported");
            return result;
        }
        if (mp4Status != VideoTrackValidation::Supported) {
            result.errorCode = QStringLiteral("invalid_mp4_video");
            return result;
        }

        const VideoFrameProbe frameProbe =
            decodeFirstMp4Frame(info.canonicalFilePath());
        if (frameProbe.result != VideoFrameProbeResult::Decoded) {
            result.errorCode = frameProbe.result == VideoFrameProbeResult::TimedOut
                ? QStringLiteral("video_decode_timeout")
                : QStringLiteral("video_decode_failed");
            return result;
        }
        result.kind = Kind::Mp4Video;
        result.videoSize = frameProbe.frameSize;
        result.videoFirstFrame = frameProbe.firstFrame;
        return result;
    }

    if (isKnownVideoExtension(extension) || contentLooksLikeVideo(info.absoluteFilePath())) {
        result.kind = Kind::UnsupportedVideo;
        result.errorCode = QStringLiteral("mp4_only");
        return result;
    }

    if (!MediaFormatContract::isAllowedImageExtension(extension)) {
        result.errorCode = QStringLiteral("unsupported_image_extension");
        return result;
    }
    if (contentDeclaresMultipleImages(info.absoluteFilePath(), extension)) {
        result.errorCode = QStringLiteral("animated_image_not_supported");
        return result;
    }

    QImageReader reader(info.absoluteFilePath());
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead()) {
        result.errorCode = QStringLiteral("invalid_image_content");
        return result;
    }
    if (!imageFormatMatchesExtension(reader.format(), extension)) {
        result.errorCode = QStringLiteral("image_extension_mismatch");
        return result;
    }
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty()) {
        result.errorCode = QStringLiteral("invalid_image_dimensions");
        return result;
    }
    const quint64 width = static_cast<quint64>(size.width());
    const quint64 height = static_cast<quint64>(size.height());
    if (width > MaximumImagePixels || height > MaximumImagePixels
        || width * height > MaximumImagePixels) {
        result.errorCode = QStringLiteral("image_pixel_limit_exceeded");
        return result;
    }

    const quint64 decodedRgbaBytes = width * height * 4ULL;
    if (decodedRgbaBytes > MaximumPreparedImageBytes
        || preparedImageBytes > MaximumPreparedImageBytes
        || decodedRgbaBytes > MaximumPreparedImageBytes - preparedImageBytes) {
        result.errorCode = QStringLiteral("decoded_rgba_budget_exceeded");
        return result;
    }

    const int imageCount = reader.imageCount();
    if (imageCount > 1) {
        result.errorCode = QStringLiteral("animated_image_not_supported");
        return result;
    }

    // canRead() and size() only validate the header for several plugins. Decode
    // the complete first frame so truncated/corrupt files cannot be uploaded.
    const QImage decoded = reader.read();
    if (decoded.isNull() || decoded.size() != size) {
        result.errorCode = QStringLiteral("image_decode_failed");
        return result;
    }
    // Plugins are allowed to report an unknown imageCount() until after the
    // first decode. Trying the next frame closes that ambiguity for WebP/AVIF
    // and any future animation-capable allowlisted handler.
    if (reader.jumpToNextImage()) {
        result.errorCode = QStringLiteral("animated_image_not_supported");
        return result;
    }

    result.kind = Kind::Image;
    result.imageSize = size;
    result.decodedRgbaBytes = decodedRgbaBytes;
    return result;
}

QString validationErrorDescription(const ValidationResult& validation)
{
    const QString& code = validation.errorCode;
    if (code == QLatin1String("file_unreadable")) {
        return QStringLiteral("the source file is missing or unreadable");
    }
    if (code == QLatin1String("video_codec_not_supported")) {
        return QStringLiteral("the MP4 video codec is not supported");
    }
    if (code == QLatin1String("invalid_mp4_video")) {
        return QStringLiteral("the file is not a valid MP4 video");
    }
    if (code == QLatin1String("video_decode_timeout")) {
        return QStringLiteral("the MP4 decoder timed out");
    }
    if (code == QLatin1String("video_decode_failed")) {
        return QStringLiteral("the MP4 video could not be decoded");
    }
    if (code == QLatin1String("mp4_only")) {
        return QStringLiteral("only MP4 is accepted for video");
    }
    if (code == QLatin1String("unsupported_image_extension")) {
        return QStringLiteral("the image extension is not accepted");
    }
    if (code == QLatin1String("animated_image_not_supported")) {
        return QStringLiteral("animated images are not supported");
    }
    if (code == QLatin1String("invalid_image_content")) {
        return QStringLiteral("the image format is unavailable or its content is invalid");
    }
    if (code == QLatin1String("image_extension_mismatch")) {
        return QStringLiteral("the image content does not match its extension");
    }
    if (code == QLatin1String("invalid_image_dimensions")) {
        return QStringLiteral("the image dimensions are invalid");
    }
    if (code == QLatin1String("image_pixel_limit_exceeded")) {
        return QStringLiteral("the image exceeds the 64-megapixel limit");
    }
    if (code == QLatin1String("decoded_rgba_budget_exceeded")) {
        return QStringLiteral("the decoded image memory limit would be exceeded");
    }
    if (code == QLatin1String("image_decode_failed")) {
        return QStringLiteral("the image is truncated or could not be decoded");
    }
    return code.isEmpty()
        ? QStringLiteral("the media type does not match its canvas item")
        : QStringLiteral("media validation failed (%1)").arg(code);
}

PreparationValidationResult validatePreparationAssets(
    const QList<PreparationAsset>& assets,
    quint64 preparedImageBytes)
{
    PreparationValidationResult result;
    result.totalDecodedRgbaBytes = preparedImageBytes;
    if (preparedImageBytes > MaximumPreparedImageBytes) {
        result.errorCode = QStringLiteral("decoded_rgba_budget_exceeded");
        return result;
    }

    for (const PreparationAsset& asset : assets) {
        if (asset.expectedKind != Kind::Image
            && asset.expectedKind != Kind::Mp4Video) {
            result.failedAssetId = asset.assetId;
            result.errorCode = QStringLiteral("invalid_declared_media_kind");
            return result;
        }

        const ValidationResult validation =
            validateLocalFile(asset.path, result.totalDecodedRgbaBytes);
        if (!validation.accepted()) {
            result.failedAssetId = asset.assetId;
            result.errorCode = validation.errorCode.isEmpty()
                ? QStringLiteral("media_validation_failed")
                : validation.errorCode;
            return result;
        }
        if (validation.kind != asset.expectedKind) {
            result.failedAssetId = asset.assetId;
            result.errorCode = QStringLiteral("media_kind_mismatch");
            return result;
        }
        if (validation.kind == Kind::Image) {
            // validateLocalFile() has already performed the subtraction-based
            // overflow check against MaximumPreparedImageBytes.
            result.totalDecodedRgbaBytes += validation.decodedRgbaBytes;
        }
    }

    result.accepted = true;
    return result;
}

Kind classifyLocalFile(const QString& path) {
    return validateLocalFile(path).kind;
}

bool isAcceptedLocalFile(const QString& path) {
    return validateLocalFile(path).accepted();
}

bool isMp4Video(const QString& path) {
    return classifyLocalFile(path) == Kind::Mp4Video;
}

bool matchesSha256(const QString& path, const QString& expectedSha256) {
    if (expectedSha256.size() != 64) return false;
    for (const QChar character : expectedSha256) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) {
            return false;
        }
    }

    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath();
    if (!info.exists() || !info.isFile() || info.isSymLink()
        || canonicalPath.isEmpty()) return false;

    QFile file(canonicalPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) return false;
        hash.addData(QByteArrayView(block));
    }
    return file.error() == QFileDevice::NoError
        && hash.result().toHex() == expectedSha256.toLatin1();
}

} // namespace MediaFilePolicy
