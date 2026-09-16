#include "backend/media/MediaDecoder.h"
#include "backend/domain/media/MediaFilePolicy.h"

#include <QAbstractVideoBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageIOHandler>
#include <QVideoFrameFormat>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {
constexpr quint64 MiB = 1024ULL * 1024ULL;
constexpr AVRational microseconds{1, 1000000};

QString avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return QString::fromUtf8(text);
}

quint64 saturatedBytes(long double bytes) {
    if (!std::isfinite(bytes) || bytes >= std::numeric_limits<quint64>::max())
        return std::numeric_limits<quint64>::max();
    return static_cast<quint64>(std::max<long double>(0, std::ceil(bytes)));
}

struct FormatDeleter { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
struct CodecDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct SwrDeleter { void operator()(SwrContext* p) const { swr_free(&p); } };
struct SwsDeleter { void operator()(SwsContext* p) const { sws_freeContext(p); } };
using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

int interrupted(void* opaque) {
    const auto* callbacks = static_cast<const MediaDecoder::DecodeCallbacks*>(opaque);
    return callbacks && callbacks->cancelled && callbacks->cancelled();
}

FormatPtr openFormat(const QString& path, QString& error,
                     const MediaDecoder::DecodeCallbacks* callbacks = nullptr) {
    AVFormatContext* context = avformat_alloc_context();
    if (!context) { error = QStringLiteral("Unable to allocate the media reader"); return {}; }
    context->interrupt_callback = {interrupted, const_cast<MediaDecoder::DecodeCallbacks*>(callbacks)};
    context->error_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
    const QByteArray encoded = QFile::encodeName(path);
    int result = avformat_open_input(&context, encoded.constData(), nullptr, nullptr);
    if (result < 0) { error = avError(result); return {}; }
    FormatPtr format(context);
    result = avformat_find_stream_info(context, nullptr);
    if (result < 0) { error = avError(result); return {}; }
    return format;
}

int rotationFor(const AVStream* stream) {
    const AVPacketSideData* matrix = av_packet_side_data_get(
        stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (!matrix || matrix->size < 9 * sizeof(int32_t)) return 0;
    const double rotation = -av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix->data));
    if (!std::isfinite(rotation)) return 0;
    return ((qRound(rotation / 90.0) * 90) % 360 + 360) % 360;
}

QSize unrotatedDisplaySize(AVFormatContext* format, AVStream* stream, const AVFrame* frame = nullptr) {
    const int width = frame ? frame->width : stream->codecpar->width;
    const int height = frame ? frame->height : stream->codecpar->height;
    const AVRational sar = av_guess_sample_aspect_ratio(format, stream, const_cast<AVFrame*>(frame));
    const double scaled = sar.num > 0 && sar.den > 0 ? width * av_q2d(sar) : width;
    if (width <= 0 || height <= 0 || !std::isfinite(scaled)
        || scaled <= 0 || scaled > std::numeric_limits<int>::max()) return {};
    return QSize(std::max(1, qRound(scaled)), height);
}

QSize displaySize(AVFormatContext* format, AVStream* stream) {
    QSize size = unrotatedDisplaySize(format, stream);
    if (rotationFor(stream) % 180) size.transpose();
    return size;
}

qint64 sourceDuration(AVFormatContext* format, AVStream* stream) {
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) return format->duration;
    return stream->duration != AV_NOPTS_VALUE && stream->duration > 0
        ? av_rescale_q(stream->duration, stream->time_base, microseconds) : 0;
}

CodecPtr openCodec(AVStream* stream, QString& error) {
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) { error = QStringLiteral("No decoder is available for a media stream"); return {}; }
    CodecPtr context(avcodec_alloc_context3(codec));
    if (!context) { error = QStringLiteral("Unable to allocate the decoder"); return {}; }
    int result = avcodec_parameters_to_context(context.get(), stream->codecpar);
    if (result >= 0) {
        context->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
        // Fixed bounded worker count makes temporary-memory reservations predictable.
        context->thread_count = 1;
        context->pkt_timebase = stream->time_base;
        result = avcodec_open2(context.get(), codec, nullptr);
    }
    if (result < 0) { error = avError(result); return {}; }
    return context;
}

QVideoFrameFormat::PixelFormat qtPixelFormat(AVPixelFormat pixelFormat) {
    using F = QVideoFrameFormat;
    switch (pixelFormat) {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: return F::Format_YUV420P;
    case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: return F::Format_YUV422P;
    case AV_PIX_FMT_NV12: return F::Format_NV12;
    case AV_PIX_FMT_NV21: return F::Format_NV21;
    case AV_PIX_FMT_UYVY422: return F::Format_UYVY;
    case AV_PIX_FMT_YUYV422: return F::Format_YUYV;
    case AV_PIX_FMT_GRAY8: return F::Format_Y8;
    case AV_PIX_FMT_GRAY16LE: return F::Format_Y16;
    case AV_PIX_FMT_P010LE: return F::Format_P010;
    case AV_PIX_FMT_P016LE: return F::Format_P016;
    case AV_PIX_FMT_YUV420P10LE: return F::Format_YUV420P10;
    case AV_PIX_FMT_RGBA: return F::Format_RGBA8888;
    case AV_PIX_FMT_BGRA: return F::Format_BGRA8888;
    case AV_PIX_FMT_ARGB: return F::Format_ARGB8888;
    case AV_PIX_FMT_ABGR: return F::Format_ABGR8888;
    default: return F::Format_Invalid;
    }
}

void applyColors(QVideoFrameFormat& format, const AVFrame* frame) {
    using F = QVideoFrameFormat;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709: format.setColorSpace(F::ColorSpace_BT709); break;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_FCC:
        format.setColorSpace(F::ColorSpace_BT601); break;
    case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL:
        format.setColorSpace(F::ColorSpace_BT2020); break;
    default: break;
    }
    const bool jpegRange = frame->format == AV_PIX_FMT_YUVJ420P || frame->format == AV_PIX_FMT_YUVJ422P;
    format.setColorRange(frame->color_range == AVCOL_RANGE_JPEG || jpegRange ? F::ColorRange_Full
        : frame->color_range == AVCOL_RANGE_MPEG ? F::ColorRange_Video : F::ColorRange_Unknown);
    if (const auto* light = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        if (light->size >= sizeof(AVContentLightMetadata)) {
            const auto* metadata = reinterpret_cast<const AVContentLightMetadata*>(light->data);
            if (metadata->MaxCLL) format.setMaxLuminance(float(metadata->MaxCLL));
        }
    } else if (const auto* mastering = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        if (mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
            const auto* metadata = reinterpret_cast<const AVMasteringDisplayMetadata*>(mastering->data);
            if (metadata->has_luminance && metadata->max_luminance.den > 0)
                format.setMaxLuminance(float(av_q2d(metadata->max_luminance)));
        }
    }
    switch (frame->color_trc) {
    case AVCOL_TRC_BT709: format.setColorTransfer(F::ColorTransfer_BT709); break;
    case AVCOL_TRC_SMPTE170M: format.setColorTransfer(F::ColorTransfer_BT601); break;
    case AVCOL_TRC_LINEAR: format.setColorTransfer(F::ColorTransfer_Linear); break;
    case AVCOL_TRC_GAMMA22: format.setColorTransfer(F::ColorTransfer_Gamma22); break;
    case AVCOL_TRC_GAMMA28: format.setColorTransfer(F::ColorTransfer_Gamma28); break;
    case AVCOL_TRC_SMPTE2084: format.setColorTransfer(F::ColorTransfer_ST2084); break;
    case AVCOL_TRC_ARIB_STD_B67: format.setColorTransfer(F::ColorTransfer_STD_B67); break;
    default: break;
    }
}

class CpuFrameBuffer final : public QAbstractVideoBuffer {
public:
    CpuFrameBuffer(QByteArray bytes, QVideoFrameFormat format,
                   const std::array<int, 4>& strides, const std::array<int, 4>& offsets,
                   const std::array<int, 4>& sizes, int planes)
        : m_bytes(std::move(bytes)), m_format(std::move(format)), m_strides(strides),
          m_offsets(offsets), m_sizes(sizes), m_planes(planes) {}
    MapData map(QVideoFrame::MapMode mode) override {
        if (mode != QVideoFrame::ReadOnly) return {};
        MapData result;
        result.planeCount = m_planes;
        for (int p = 0; p < m_planes; ++p) {
            result.data[p] = reinterpret_cast<uchar*>(const_cast<char*>(m_bytes.constData())) + m_offsets[p];
            result.bytesPerLine[p] = m_strides[p];
            result.dataSize[p] = m_sizes[p];
        }
        return result;
    }
    QVideoFrameFormat format() const override { return m_format; }
private:
    QByteArray m_bytes;
    QVideoFrameFormat m_format;
    std::array<int, 4> m_strides, m_offsets, m_sizes;
    int m_planes;
};

// Qt's private frame object is deliberately opaque. Include a conservative
// bookkeeping allowance; presentation conversion caches are separately bounded
// by ResidentVideoPlayer's ephemeral frame wrappers.
constexpr quint64 QtFrameBookkeepingBytes = 512;

template<typename T> struct VectorGrowth {
    size_t capacity;
    quint64 peakBytes;
    quint64 retainedBytes;
};
template<typename T> VectorGrowth<T> nextVectorGrowth(const std::vector<T>& values) {
    if (values.size() < values.capacity()) return {values.capacity(), 0, 0};
    const size_t next = values.capacity() == 0 ? 1
        : values.capacity() > values.max_size() / 2 ? values.max_size() : values.capacity() * 2;
    if (next <= values.size()) throw std::bad_alloc();
    return {next, quint64(next) * sizeof(T), quint64(next - values.capacity()) * sizeof(T)};
}

struct DecoderJob {
    const MediaDecoder::DecodeCallbacks& callbacks;
    QString error;
    quint64 scratch = 64 * MiB;
    std::shared_ptr<ResidentMediaAsset> asset = std::make_shared<ResidentMediaAsset>();
    bool check(quint64 additional = 0) {
        if (callbacks.cancelled && callbacks.cancelled()) { error = QStringLiteral("cancelled"); return false; }
        if (additional > std::numeric_limits<quint64>::max() - asset->residentBytes
            || asset->residentBytes + additional > std::numeric_limits<quint64>::max() - scratch
            || (callbacks.reserve && !callbacks.reserve(asset->residentBytes + additional + scratch))) {
            error = QStringLiteral("memory_unavailable"); return false;
        }
        return true;
    }
};

bool storeFrame(DecoderJob& job, AVFormatContext* format, AVStream* stream, AVFrame* source,
                qint64 originUs, qint64 fallbackDurationUs) {
    const QSize target = unrotatedDisplaySize(format, stream, source);
    if (!target.isValid()) { job.error = QStringLiteral("Invalid decoded video dimensions"); return false; }
    AVPixelFormat pixel = static_cast<AVPixelFormat>(source->format);
    auto qtPixel = qtPixelFormat(pixel);
    const bool convert = qtPixel == QVideoFrameFormat::Format_Invalid
        || target.width() != source->width || target.height() != source->height;
    const bool convertedToRgba = qtPixel == QVideoFrameFormat::Format_Invalid;
    if (convertedToRgba) { pixel = AV_PIX_FMT_RGBA; qtPixel = QVideoFrameFormat::Format_RGBA8888; }
    const int size = av_image_get_buffer_size(pixel, target.width(), target.height(), 32);
    if (size <= 0) { job.error = QStringLiteral("Invalid decoded video allocation"); return false; }
    const quint64 bytes = quint64(size) + sizeof(CpuFrameBuffer) + QtFrameBookkeepingBytes;
    const auto growth = nextVectorGrowth(job.asset->frames);
    // A vector reallocation temporarily owns both old and new arrays.
    if (!job.check(bytes + growth.peakBytes)) return false;
    if (growth.retainedBytes) {
        job.asset->frames.reserve(growth.capacity);
        job.asset->residentBytes += growth.retainedBytes;
    }
    QByteArray data(size, Qt::Uninitialized);
    uint8_t* planes[4]{};
    int strides[4]{};
    if (av_image_fill_arrays(planes, strides, reinterpret_cast<uint8_t*>(data.data()),
                             pixel, target.width(), target.height(), 32) < 0) return false;
    if (convert) {
        std::unique_ptr<SwsContext, SwsDeleter> scaler(sws_getContext(source->width, source->height,
            static_cast<AVPixelFormat>(source->format), target.width(), target.height(), pixel,
            SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!scaler) { job.error = QStringLiteral("Unsupported video pixel conversion"); return false; }
        const int* coefficients = sws_getCoefficients(source->colorspace == AVCOL_SPC_BT709
            ? SWS_CS_ITU709 : source->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_DEFAULT);
        sws_setColorspaceDetails(scaler.get(), coefficients, source->color_range == AVCOL_RANGE_JPEG,
                                coefficients, convertedToRgba || source->color_range == AVCOL_RANGE_JPEG,
                                0, 1 << 16, 1 << 16);
        if (sws_scale(scaler.get(), source->data, source->linesize, 0, source->height, planes, strides)
            != target.height()) { job.error = QStringLiteral("Video pixel conversion failed"); return false; }
    } else {
        const uint8_t* sourcePlanes[4] = {source->data[0], source->data[1], source->data[2], source->data[3]};
        av_image_copy(planes, strides, sourcePlanes, source->linesize,
                      pixel, target.width(), target.height());
    }
    const int count = av_pix_fmt_count_planes(pixel);
    std::array<int, 4> offsets{}, rowBytes{}, planeBytes{};
    for (int p = 0; p < count; ++p) {
        offsets[p] = int(planes[p] - reinterpret_cast<uint8_t*>(data.data()));
        rowBytes[p] = strides[p];
        planeBytes[p] = (p + 1 < count ? int(planes[p + 1] - planes[p]) : size - offsets[p]);
    }
    QVideoFrameFormat frameFormat(target, qtPixel);
    applyColors(frameFormat, source);
    if (convertedToRgba) frameFormat.setColorRange(QVideoFrameFormat::ColorRange_Full);
    QVideoFrame frame(std::make_unique<CpuFrameBuffer>(std::move(data), frameFormat,
                                                     rowBytes, offsets, planeBytes, count));
    const qint64 timestamp = source->best_effort_timestamp != AV_NOPTS_VALUE
        ? av_rescale_q(source->best_effort_timestamp, stream->time_base, microseconds) - originUs
        : job.asset->frames.empty() ? 0 : job.asset->frames.back().timestampUs + job.asset->frames.back().durationUs;
    const qint64 duration = source->duration > 0
        ? av_rescale_q(source->duration, stream->time_base, microseconds) : fallbackDurationUs;
    frame.setStartTime(std::max<qint64>(0, timestamp));
    frame.setEndTime(std::max<qint64>(0, timestamp) + std::max<qint64>(1, duration));
    frame.setRotation(static_cast<QtVideo::Rotation>(rotationFor(stream)));
    job.asset->frames.push_back({std::move(frame), std::max<qint64>(0, timestamp), std::max<qint64>(1, duration)});
    job.asset->residentBytes += bytes;
    return true;
}

bool hashSource(const QString& path, DecoderJob& job) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { job.error = file.errorString(); return false; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (!job.check()) return false;
        const QByteArray bytes = file.read(MiB);
        if (file.error() != QFileDevice::NoError) { job.error = file.errorString(); return false; }
        hash.addData(bytes);
    }
    job.asset->sha256 = QString::fromLatin1(hash.result().toHex());
    return true;
}
} // namespace

MediaDecoder::Probe MediaDecoder::probe(const QString& path) {
    Probe result;
    const auto validation = MediaFilePolicy::validateLocalFileMetadata(path);
    if (!validation.accepted()) {
        result.error = MediaFilePolicy::validationErrorDescription(validation);
        return result;
    }
    result.video = validation.kind == MediaFilePolicy::Kind::Mp4Video;
    if (!result.video) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        result.displaySize = reader.size();
        if (reader.transformation() & QImageIOHandler::TransformationRotate90) result.displaySize.transpose();
        result.estimatedBytes = quint64(result.displaySize.width()) * result.displaySize.height() * 4;
        result.scratchBytes = std::max<quint64>(64 * MiB, result.estimatedBytes * 4);
        return result;
    }
    auto format = openFormat(path, result.error);
    if (!format) return result;
    const int index = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) { result.error = QStringLiteral("The MP4 contains no decodable video stream"); return result; }
    AVStream* stream = format->streams[index];
    result.displaySize = displaySize(format.get(), stream);
    if (!result.displaySize.isValid()) { result.error = QStringLiteral("Invalid video dimensions"); return result; }
    result.durationUs = sourceDuration(format.get(), stream);
    const AVRational rate = av_guess_frame_rate(format.get(), stream, nullptr);
    const double fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 30.0;
    const qint64 videoDurationUs = stream->duration > 0 && stream->duration != AV_NOPTS_VALUE
        ? av_rescale_q(stream->duration, stream->time_base, microseconds) : result.durationUs;
    const long double frames = stream->nb_frames > 0 ? stream->nb_frames
        : std::ceil(videoDurationUs / 1e6L * fps);
    const QSize storedSize = unrotatedDisplaySize(format.get(), stream);
    const AVPixelFormat advertisedPixel = static_cast<AVPixelFormat>(stream->codecpar->format);
    const bool nativePixels = qtPixelFormat(advertisedPixel) != QVideoFrameFormat::Format_Invalid;
    const int frameBytes = advertisedPixel != AV_PIX_FMT_NONE
        ? av_image_get_buffer_size(nativePixels ? advertisedPixel : AV_PIX_FMT_RGBA,
                                   storedSize.width(), storedSize.height(), 32) : -1;
    const long double allocationPerFrame = frameBytes > 0 ? frameBytes
        : storedSize.width() * static_cast<long double>(storedSize.height()) * 8;
    long double bytes = std::max<long double>(1, frames)
        * (allocationPerFrame + 2 * sizeof(ResidentVideoFrame) + sizeof(CpuFrameBuffer) + QtFrameBookkeepingBytes);
    result.scratchBytes = std::max<quint64>(64 * MiB, saturatedBytes(
        stream->codecpar->width * static_cast<long double>(stream->codecpar->height) * 8 * 24));
    const int audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, index, nullptr, 0);
    if (audioIndex >= 0) {
        const AVStream* audioStream = format->streams[audioIndex];
        const AVCodecParameters* p = audioStream->codecpar;
        const qint64 audioDurationUs = audioStream->duration > 0 && audioStream->duration != AV_NOPTS_VALUE
            ? av_rescale_q(audioStream->duration, audioStream->time_base, microseconds) : result.durationUs;
        bytes += audioDurationUs / 1e6L * p->sample_rate * p->ch_layout.nb_channels * sizeof(float);
    }
    result.estimatedBytes = saturatedBytes(bytes);
    return result;
}

std::shared_ptr<ResidentMediaAsset> MediaDecoder::decode(
    const QString& path, const DecodeCallbacks& callbacks, QString* error) {
    DecoderJob job{callbacks};
    auto fail = [&]() -> std::shared_ptr<ResidentMediaAsset> {
        if (error) *error = job.error.isEmpty() ? QStringLiteral("Media decoding failed") : job.error;
        return {};
    };
    if (error) error->clear();
    try {
        if (!job.check()) return fail();
        const QFileInfo before(path);
        const qint64 sourceBytes = before.size();
        const QDateTime sourceModified = before.lastModified();
        const Probe metadata = probe(path);
        if (!metadata.accepted()) { job.error = metadata.error; return fail(); }
        job.asset->video = metadata.video;
        job.asset->displaySize = metadata.displaySize;
        if (!metadata.video) {
            job.scratch = metadata.scratchBytes;
            if (!job.check(metadata.estimatedBytes)) return fail();
            QImageReader reader(path);
            reader.setAutoTransform(true);
            job.asset->image = reader.read();
            if (job.asset->image.isNull() || reader.error() == QImageReader::InvalidDataError) {
                job.error = reader.errorString(); return fail();
            }
            if (job.asset->image.format() != QImage::Format_RGBA8888) {
                job.asset->image = job.asset->image.convertToFormat(QImage::Format_RGBA8888);
            }
            if (job.asset->image.isNull()) { job.error = QStringLiteral("Image conversion failed"); return fail(); }
            job.asset->displaySize = job.asset->image.size();
            job.asset->residentBytes = job.asset->image.sizeInBytes();
        } else {
            auto format = openFormat(path, job.error, &callbacks);
            if (!format) return fail();
            const int videoIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            const int audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
            if (videoIndex < 0) { job.error = QStringLiteral("No video stream"); return fail(); }
            AVStream* videoStream = format->streams[videoIndex];
            // Codec references, conversion buffers, demux packets and reader buffers.
            job.scratch = std::max<quint64>(64 * MiB, saturatedBytes(
                videoStream->codecpar->width * static_cast<long double>(videoStream->codecpar->height) * 8 * 24));
            if (!job.check()) return fail();
            auto video = openCodec(videoStream, job.error);
            if (!video) return fail();
            CodecPtr audio;
            std::unique_ptr<SwrContext, SwrDeleter> resampler;
            qint64 audioCursorUs = 0;
            bool haveAudioCursor = false;
            if (audioIndex >= 0) {
                audio = openCodec(format->streams[audioIndex], job.error);
                if (!audio) return fail();
                if (audio->sample_rate <= 0 || audio->ch_layout.nb_channels <= 0 || audio->ch_layout.nb_channels > 64) {
                    job.error = QStringLiteral("Invalid audio format"); return fail();
                }
                job.asset->audioFormat.setSampleRate(audio->sample_rate);
                job.asset->audioFormat.setChannelCount(audio->ch_layout.nb_channels);
                job.asset->audioFormat.setSampleFormat(QAudioFormat::Float);
                // FFmpeg and Qt use the same canonical speaker ordering for
                // the first 18 native positions (Qt reserves bit zero).
                // Unknown/custom layouts retain their channel count instead
                // of silently replacing it with a guessed stereo layout.
                constexpr uint64_t standardSpeakers = (uint64_t(1) << 18) - 1;
                if (audio->ch_layout.order == AV_CHANNEL_ORDER_NATIVE
                    && (audio->ch_layout.u.mask & ~standardSpeakers) == 0)
                    job.asset->audioFormat.setChannelConfig(
                        QAudioFormat::ChannelConfig(quint32(audio->ch_layout.u.mask << 1)));
                SwrContext* raw = nullptr;
                const int result = swr_alloc_set_opts2(&raw, &audio->ch_layout, AV_SAMPLE_FMT_FLT,
                    audio->sample_rate, &audio->ch_layout, audio->sample_fmt, audio->sample_rate, 0, nullptr);
                resampler.reset(raw);
                if (result < 0 || !resampler || swr_init(resampler.get()) < 0) {
                    job.error = QStringLiteral("Unable to decode the full audio track"); return fail();
                }
            }
            qint64 originUs = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
            const AVRational frameRate = av_guess_frame_rate(format.get(), videoStream, nullptr);
            const qint64 fallbackDurationUs = frameRate.num > 0 && frameRate.den > 0
                ? std::max<qint64>(1, av_rescale_q(1, av_inv_q(frameRate), microseconds)) : 33333;
            std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
            FramePtr frame(av_frame_alloc());
            if (!packet || !frame) { job.error = QStringLiteral("Unable to allocate decoding buffers"); return fail(); }
            auto storeAudio = [&](AVFrame* decoded) -> bool {
                const int channels = job.asset->audioFormat.channelCount();
                const int rate = job.asset->audioFormat.sampleRate();
                if (decoded && (decoded->sample_rate != rate || decoded->ch_layout.nb_channels != channels
                    || decoded->format != audio->sample_fmt)) {
                    job.error = QStringLiteral("The audio format changes during the video"); return false;
                }
                const int inputSamples = decoded ? decoded->nb_samples : 0;
                const int capacity = swr_get_out_samples(resampler.get(), inputSamples);
                if (capacity < 0 || capacity > std::numeric_limits<int>::max() / (channels * int(sizeof(float)))) {
                    job.error = QStringLiteral("Invalid audio sample count"); return false;
                }
                const quint64 allocation = quint64(capacity) * channels * sizeof(float);
                const auto growth = nextVectorGrowth(job.asset->audio);
                if (!job.check(allocation + growth.peakBytes)) return false;
                if (growth.retainedBytes) {
                    job.asset->audio.reserve(growth.capacity);
                    job.asset->residentBytes += growth.retainedBytes;
                }
                QByteArray pcm(qsizetype(allocation), Qt::Uninitialized);
                uint8_t* destination = reinterpret_cast<uint8_t*>(pcm.data());
                const int count = swr_convert(resampler.get(), &destination, capacity,
                    decoded ? const_cast<const uint8_t**>(decoded->extended_data) : nullptr, inputSamples);
                if (count < 0) { job.error = avError(count); return false; }
                if (!count) return true;
                if (decoded && decoded->best_effort_timestamp != AV_NOPTS_VALUE) {
                    const qint64 pts = av_rescale_q(decoded->best_effort_timestamp,
                        format->streams[audioIndex]->time_base, microseconds) - originUs;
                    if (!haveAudioCursor || std::abs(pts - audioCursorUs) > 2000) audioCursorUs = pts;
                }
                haveAudioCursor = true;
                pcm.resize(qsizetype(count) * channels * sizeof(float));
                job.asset->residentBytes += pcm.capacity();
                job.asset->audio.push_back({std::move(pcm), audioCursorUs, count});
                audioCursorUs += av_rescale_q(count, AVRational{1, rate}, microseconds);
                return true;
            };
            double publishedProgress = -1;
            auto drain = [&](AVCodecContext* codec, AVStream* stream, bool isVideo) -> bool {
                for (;;) {
                    if (!job.check()) return false;
                    const int result = avcodec_receive_frame(codec, frame.get());
                    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
                    if (result < 0) { job.error = avError(result); return false; }
                    if (frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT)) {
                        job.error = QStringLiteral("A media frame is corrupt"); return false;
                    }
                    const bool stored = isVideo
                        ? storeFrame(job, format.get(), stream, frame.get(), originUs, fallbackDurationUs)
                        : storeAudio(frame.get());
                    av_frame_unref(frame.get());
                    if (!stored) return false;
                    if (callbacks.progress && metadata.durationUs > 0 && !job.asset->frames.empty()) {
                        const double progress = std::min(0.99,
                            double(job.asset->frames.back().timestampUs) / metadata.durationUs);
                        // Long clips must not enqueue one UI update per frame.
                        if (progress - publishedProgress >= 0.005) {
                            publishedProgress = progress;
                            callbacks.progress(progress);
                        }
                    }
                }
            };
            int readResult = 0;
            quint64 discardedVideoPackets = 0;
            while ((readResult = av_read_frame(format.get(), packet.get())) >= 0) {
                if (!job.check()) return fail();
                if (packet->stream_index == videoIndex && (packet->flags & AV_PKT_FLAG_DISCARD))
                    ++discardedVideoPackets;
                if (packet->flags & AV_PKT_FLAG_CORRUPT) { job.error = QStringLiteral("A media packet is corrupt"); return fail(); }
                AVCodecContext* codec = packet->stream_index == videoIndex ? video.get()
                    : packet->stream_index == audioIndex ? audio.get() : nullptr;
                if (codec) {
                    int result = avcodec_send_packet(codec, packet.get());
                    if (result == AVERROR(EAGAIN)) {
                        if (!drain(codec, format->streams[packet->stream_index], codec == video.get())) return fail();
                        result = avcodec_send_packet(codec, packet.get());
                    }
                    if (result < 0) { job.error = avError(result); return fail(); }
                    if (!drain(codec, format->streams[packet->stream_index], codec == video.get())) return fail();
                }
                av_packet_unref(packet.get());
            }
            if (readResult != AVERROR_EOF || (format->pb && format->pb->error < 0)) {
                job.error = avError(readResult != AVERROR_EOF ? readResult : format->pb->error); return fail();
            }
            // Null packets are mandatory: delayed B-frames and audio tail must
            // be owned before readiness is ever published.
            for (AVCodecContext* codec : {video.get(), audio.get()}) {
                if (!codec) continue;
                const int result = avcodec_send_packet(codec, nullptr);
                if (result < 0 && result != AVERROR_EOF) { job.error = avError(result); return fail(); }
                if (!drain(codec, format->streams[codec == video.get() ? videoIndex : audioIndex], codec == video.get())) return fail();
            }
            if (resampler) {
                while (swr_get_delay(resampler.get(), job.asset->audioFormat.sampleRate()) > 0) {
                    const auto countBefore = job.asset->audio.size();
                    if (!storeAudio(nullptr)) return fail();
                    if (job.asset->audio.size() == countBefore) break;
                }
            }
            if (job.asset->frames.empty()) { job.error = QStringLiteral("The video decoded no frames"); return fail(); }
            if (audio && job.asset->audio.empty()) { job.error = QStringLiteral("The audio track decoded no samples"); return fail(); }
            std::stable_sort(job.asset->frames.begin(), job.asset->frames.end(),
                [](const auto& a, const auto& b) { return a.timestampUs < b.timestampUs; });
            const auto& last = job.asset->frames.back();
            job.asset->durationUs = std::max(last.timestampUs + last.durationUs, audioCursorUs);
            // Demuxers can report clean EOF for truncated indexed streams.
            // MP4 edit lists intentionally mark preroll/trimmed samples as
            // discard: they must be decoded for codec state but need not
            // produce a displayed frame. Do not reject a valid trimmed clip.
            const quint64 expectedFrames = videoStream->nb_frames > 0
                ? quint64(videoStream->nb_frames) - std::min(quint64(videoStream->nb_frames), discardedVideoPackets) : 0;
            if (expectedFrames > job.asset->frames.size()) {
                job.error = QStringLiteral("The video ended before every indexed frame was decoded (%1 of %2)")
                    .arg(job.asset->frames.size()).arg(expectedFrames); return fail();
            }
        }
        if (!hashSource(path, job)) return fail();
        const QFileInfo after(path);
        if (!after.exists() || after.size() != sourceBytes || after.lastModified() != sourceModified) {
            job.error = QStringLiteral("The source file changed while it was being decoded"); return fail();
        }
        if (!job.check()) return fail();
        if (callbacks.progress) callbacks.progress(1.0);
        return job.asset;
    } catch (const std::bad_alloc&) {
        job.error = QStringLiteral("memory_unavailable");
        return fail();
    }
}
