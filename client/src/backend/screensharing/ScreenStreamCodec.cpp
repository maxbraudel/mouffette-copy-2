#include "backend/screensharing/ScreenStreamCodec.h"
#include "backend/screensharing/ScreenCaptureVideoBuffer.h"

#include <QAbstractVideoBuffer>
#include <QVideoFrameFormat>
#include <algorithm>
#include <array>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {
struct CodecDelete { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct FilterDelete { void operator()(AVBSFContext* p) const { av_bsf_free(&p); } };
using Codec = std::unique_ptr<AVCodecContext, CodecDelete>;
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
using Packet = std::unique_ptr<AVPacket, PacketDelete>;
using Filter = std::unique_ptr<AVBSFContext, FilterDelete>;

QString failure(const char* operation, int status) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, buffer, sizeof(buffer));
    return QString::fromLatin1(operation) + QStringLiteral(": ") + QString::fromUtf8(buffer);
}

AVPixelFormat captureFormat(QVideoFrameFormat::PixelFormat format) {
    using F = QVideoFrameFormat;
    switch (format) {
    case F::Format_ARGB8888: case F::Format_ARGB8888_Premultiplied: case F::Format_XRGB8888: return AV_PIX_FMT_ARGB;
    case F::Format_BGRA8888: case F::Format_BGRA8888_Premultiplied: case F::Format_BGRX8888: return AV_PIX_FMT_BGRA;
    case F::Format_ABGR8888: case F::Format_XBGR8888: return AV_PIX_FMT_ABGR;
    case F::Format_RGBA8888: case F::Format_RGBX8888: return AV_PIX_FMT_RGBA;
    case F::Format_NV12: return AV_PIX_FMT_NV12;
    case F::Format_NV21: return AV_PIX_FMT_NV21;
    case F::Format_YUV420P: case F::Format_YV12: return AV_PIX_FMT_YUV420P;
    case F::Format_YUV422P: return AV_PIX_FMT_YUV422P;
    case F::Format_UYVY: return AV_PIX_FMT_UYVY422;
    case F::Format_YUYV: return AV_PIX_FMT_YUYV422;
    case F::Format_P010: return AV_PIX_FMT_P010LE;
    case F::Format_P016: return AV_PIX_FMT_P016LE;
    default: return AV_PIX_FMT_NONE;
    }
}

QSize boundedSize(QSize source, int maximumEdge) {
    if (source.width() > maximumEdge || source.height() > maximumEdge)
        source.scale(maximumEdge, maximumEdge, Qt::KeepAspectRatio);
    return QSize(std::max(2, source.width() & ~1), std::max(2, source.height() & ~1));
}

// DXGI returns the unrotated desktop texture with QVideoFrameFormat rotation
// metadata. Bake that transform into the bounded YUV planes, before encoding,
// so a portrait monitor has the same geometry on every receiving platform.
void orientPlanes(const AVFrame* source, AVFrame* target, int rotation, bool mirrored) {
    const bool nv12 = source->format == AV_PIX_FMT_NV12;
    for (int plane = 0; plane < (nv12 ? 2 : 3); ++plane) {
        const int width = plane == 0 ? source->width : source->width / 2;
        const int height = plane == 0 ? source->height : source->height / 2;
        const int targetWidth = plane == 0 ? target->width : target->width / 2;
        const int bytes = nv12 && plane != 0 ? 2 : 1;
        // Tile transposes to keep both source and destination cache-friendly.
        for (int top = 0; top < height; top += 32)
            for (int left = 0; left < width; left += 32)
                for (int y = top; y < std::min(top + 32, height); ++y)
                    for (int x = left; x < std::min(left + 32, width); ++x) {
                        int dx = x, dy = y;
                        switch (rotation) {
                        case 90: dx = height - 1 - y; dy = x; break;
                        case 180: dx = width - 1 - x; dy = height - 1 - y; break;
                        case 270: dx = y; dy = width - 1 - x; break;
                        default: break;
                        }
                        if (mirrored) dx = targetWidth - 1 - dx;
                        const auto* from = source->data[plane] + y * source->linesize[plane] + x * bytes;
                        auto* to = target->data[plane] + dy * target->linesize[plane] + dx * bytes;
                        to[0] = from[0];
                        if (bytes == 2) to[1] = from[1];
                    }
    }
}

struct Nals {
    QByteArray sps, pps;
    bool idr = false;
};
Nals inspectAnnexB(const QByteArray& bytes) {
    Nals result;
    auto startCode = [&](qsizetype from, qsizetype& prefix) {
        for (qsizetype i = from; i + 3 <= bytes.size(); ++i) {
            if (bytes[i] != 0 || bytes[i + 1] != 0) continue;
            if (bytes[i + 2] == 1) { prefix = 3; return i; }
            if (i + 4 <= bytes.size() && bytes[i + 2] == 0 && bytes[i + 3] == 1) { prefix = 4; return i; }
        }
        return qsizetype(-1);
    };
    qsizetype prefix = 0, position = startCode(0, prefix);
    while (position >= 0) {
        const auto payload = position + prefix;
        qsizetype nextPrefix = 0;
        const auto next = startCode(payload, nextPrefix);
        if (payload < bytes.size()) {
            const int type = static_cast<unsigned char>(bytes[payload]) & 31;
            if (type == 5) result.idr = true;
            if (type == 7 || type == 8) {
                const QByteArray nal = bytes.mid(position, (next < 0 ? bytes.size() : next) - position);
                if (type == 7) result.sps = nal; else result.pps = nal;
            }
        }
        position = next; prefix = nextPrefix;
    }
    return result;
}

class DecodedBuffer final : public QAbstractVideoBuffer {
public:
    explicit DecodedBuffer(Frame frame) : m_frame(std::move(frame)) {
        m_format = QVideoFrameFormat(QSize(m_frame->width, m_frame->height),
            m_frame->format == AV_PIX_FMT_NV12 ? QVideoFrameFormat::Format_NV12 : QVideoFrameFormat::Format_YUV420P);
        m_format.setColorSpace(m_frame->colorspace == AVCOL_SPC_BT470BG || m_frame->colorspace == AVCOL_SPC_SMPTE170M
            ? QVideoFrameFormat::ColorSpace_BT601 : QVideoFrameFormat::ColorSpace_BT709);
        m_format.setColorRange(m_frame->color_range == AVCOL_RANGE_JPEG || m_frame->format == AV_PIX_FMT_YUVJ420P
            ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
        m_format.setColorTransfer(QVideoFrameFormat::ColorTransfer_BT709);
    }
    QVideoFrameFormat format() const override { return m_format; }
    MapData map(QVideoFrame::MapMode mode) override {
        if (mode != QVideoFrame::ReadOnly) return {};
        MapData data;
        data.planeCount = m_frame->format == AV_PIX_FMT_NV12 ? 2 : 3;
        for (int i = 0; i < data.planeCount; ++i) {
            data.data[i] = m_frame->data[i];
            data.bytesPerLine[i] = m_frame->linesize[i];
            data.dataSize[i] = m_frame->linesize[i] * (i == 0 ? m_frame->height : (m_frame->height + 1) / 2);
        }
        return data;
    }
private:
    Frame m_frame;
    QVideoFrameFormat m_format;
};
}

struct ScreenStreamEncoder::Private {
    bool preferHardware;
    ScreenStreamProfile profile;
    Codec context;
    Frame converted;
    Frame orientedInput;
    Filter annexBFilter;
    SwsContext* scaler = nullptr;
    QSize size;
    QString backend;
    QByteArray sps, pps;
    qint64 lastTimestamp = -1;
    qint64 lastKeyTimestamp = -30000000;
    qint64 pendingKeyTimestamp = -1;
    int pendingFrames = 0;

    explicit Private(bool hardware, ScreenStreamProfile value = {}) : preferHardware(hardware), profile(std::move(value)) {}
    ~Private() { sws_freeContext(scaler); }

    bool open(QSize output, bool nativeInput, QString& error) {
        QList<QByteArray> candidates;
        if (preferHardware) {
#if defined(Q_OS_MACOS)
            candidates.append("h264_videotoolbox");
#elif defined(Q_OS_WIN)
            candidates.append("h264_nvenc");
            candidates.append("h264_qsv");
            candidates.append("h264_amf");
#endif
        }
        candidates.append("libx264");
        candidates.append("libopenh264");
        QStringList failures;
        for (const auto& name : candidates) {
            const AVCodec* codec = avcodec_find_encoder_by_name(name.constData());
            if (!codec) continue;
            Codec candidate(avcodec_alloc_context3(codec));
            if (!candidate) continue;
            candidate->width = output.width(); candidate->height = output.height();
            candidate->pix_fmt = name == "libopenh264" ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
            if (name == "h264_videotoolbox" && nativeInput) {
                candidate->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
                candidate->sw_pix_fmt = AV_PIX_FMT_NV12;
            }
            candidate->time_base = AVRational{1, 1000000};
            candidate->framerate = AVRational{profile.framesPerSecond, 1};
            candidate->bit_rate = profile.bitrateBps;
            candidate->rc_max_rate = profile.bitrateBps;
            candidate->rc_buffer_size = std::max(32000, profile.bitrateBps / 4);
            candidate->gop_size = std::max(1, profile.framesPerSecond * profile.keyFrameIntervalMs / 1000);
            candidate->max_b_frames = 0;
            candidate->refs = 1;
            candidate->thread_count = 2;
            candidate->flags |= AV_CODEC_FLAG_LOW_DELAY;
            candidate->color_range = AVCOL_RANGE_MPEG;
            candidate->colorspace = AVCOL_SPC_BT709;
            candidate->color_primaries = AVCOL_PRI_BT709;
            candidate->color_trc = AVCOL_TRC_BT709;
            AVDictionary* options = nullptr;
            if (name == "h264_videotoolbox") {
                av_dict_set(&options, "realtime", "1", 0);
                av_dict_set(&options, "allow_sw", "0", 0);
                // Leave the optional speed/quality preference at its default.
                // Unsupported devices warn on every encoder reopen if forced.
            } else if (name == "h264_nvenc") {
                av_dict_set(&options, "preset", "p1", 0);
                av_dict_set(&options, "tune", "ull", 0);
                av_dict_set(&options, "zerolatency", "1", 0);
                av_dict_set(&options, "delay", "0", 0);
                av_dict_set(&options, "forced-idr", "1", 0);
            } else if (name == "h264_qsv") {
                av_dict_set(&options, "preset", "veryfast", 0);
                av_dict_set(&options, "async_depth", "1", 0);
                av_dict_set(&options, "look_ahead", "0", 0);
                av_dict_set(&options, "forced_idr", "1", 0);
            } else if (name == "h264_amf") {
                av_dict_set(&options, "usage", "ultralowlatency", 0);
                av_dict_set(&options, "quality", "speed", 0);
                av_dict_set(&options, "header_insertion_mode", "idr", 0);
            } else if (name == "libx264") {
                av_dict_set(&options, "preset", profile.softwarePreset.toUtf8().constData(), 0);
                av_dict_set(&options, "tune", "zerolatency", 0);
                av_dict_set(&options, "forced-idr", "1", 0);
                av_dict_set(&options, "x264-params", "repeat-headers=1:annexb=1:scenecut=0", 0);
            } else {
                av_dict_set(&options, "usage", "screen", 0);
            }
            const int result = avcodec_open2(candidate.get(), codec, &options);
            av_dict_free(&options);
            if (result < 0) { failures.append(QString::fromLatin1(name) + QStringLiteral(" ") + failure("open", result)); continue; }
            AVBSFContext* filter = nullptr;
            int status = av_bsf_alloc(av_bsf_get_by_name("h264_mp4toannexb"), &filter);
            annexBFilter.reset(filter);
            if (status >= 0) status = avcodec_parameters_from_context(filter->par_in, candidate.get());
            if (status >= 0) { filter->time_base_in = candidate->time_base; status = av_bsf_init(filter); }
            if (status < 0) { error = failure("H.264 Annex B filter", status); return false; }
            converted.reset(av_frame_alloc());
            if (!converted) { error = QStringLiteral("Cannot allocate screen encoder frame"); return false; }
            converted->width = candidate->width; converted->height = candidate->height;
            converted->format = candidate->pix_fmt;
            converted->color_range = candidate->color_range;
            converted->colorspace = candidate->colorspace;
            converted->color_primaries = candidate->color_primaries;
            converted->color_trc = candidate->color_trc;
            status = candidate->pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX ? 0 : av_frame_get_buffer(converted.get(), 32);
            if (status < 0) { error = failure("Allocate screen encoder planes", status); return false; }
            context = std::move(candidate); size = output; backend = QString::fromLatin1(name);
            sps.clear(); pps.clear(); lastKeyTimestamp = -qint64(profile.keyFrameIntervalMs) * 1000;
            pendingKeyTimestamp = -1; pendingFrames = 0;
            if (filter->par_out->extradata_size > 0) {
                const auto headers = inspectAnnexB(QByteArray(reinterpret_cast<const char*>(filter->par_out->extradata), filter->par_out->extradata_size));
                sps = headers.sps; pps = headers.pps;
            }
            return true;
        }
        error = QStringLiteral("No low-latency H.264 encoder is available. Install FFmpeg with the native hardware encoder or libx264/libopenh264.");
        if (!failures.isEmpty()) error += QStringLiteral(" ") + failures.join(QStringLiteral("; "));
        return false;
    }
};

ScreenStreamEncoder::ScreenStreamEncoder(bool preferHardware) : d(std::make_unique<Private>(preferHardware)) {}
ScreenStreamEncoder::~ScreenStreamEncoder() = default;
void ScreenStreamEncoder::reset() { d = std::make_unique<Private>(d->preferHardware, d->profile); }
void ScreenStreamEncoder::setProfile(const ScreenStreamProfile& profile) {
    const auto value = profile.normalized();
    const auto& current = d->profile;
    const bool reopen = current.maximumEdge != value.maximumEdge
        || current.framesPerSecond != value.framesPerSecond || current.bitrateBps != value.bitrateBps
        || current.keyFrameIntervalMs != value.keyFrameIntervalMs || current.softwarePreset != value.softwarePreset;
    if (reopen) d = std::make_unique<Private>(d->preferHardware, value);
    else d->profile = value;
}
QString ScreenStreamEncoder::backendName() const { return d->backend; }
bool ScreenStreamEncoder::hasDelayedKeyFrame() const { return d->pendingKeyTimestamp >= 0; }

QList<ScreenStreamPacket> ScreenStreamEncoder::encode(QVideoFrame frame, bool forceKeyFrame, QString& error) {
    error.clear();
    if (!frame.isValid() || frame.width() < 2 || frame.height() < 2) { error = QStringLiteral("Invalid screen capture frame"); return {}; }
    const auto sourceFormat = captureFormat(frame.pixelFormat());
    if (sourceFormat == AV_PIX_FMT_NONE) { error = QStringLiteral("Unsupported native screen capture pixel format (%1)").arg(int(frame.pixelFormat())); return {}; }
    const auto surface = frame.surfaceFormat();
    const int rotation = (int(surface.rotation()) + (surface.isMirrored() ? -int(frame.rotation()) : int(frame.rotation())) + 360) % 360;
    const bool mirrored = surface.isMirrored() != frame.mirrored();
    const bool transpose = rotation == 90 || rotation == 270;
    const QSize output = boundedSize(transpose ? frame.size().transposed() : frame.size(), d->profile.maximumEdge);
    const QSize scaled = transpose ? output.transposed() : output;
    Frame nativeFrame;
    if (d->preferHardware && rotation == 0 && !mirrored && output == frame.size())
        nativeFrame.reset(screenCaptureNativeFrame(frame));
    if ((!d->context || d->size != output || (d->context->pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX && !nativeFrame)
         || (d->backend == QLatin1String("h264_videotoolbox") && nativeFrame && d->context->pix_fmt != AV_PIX_FMT_VIDEOTOOLBOX))
        && !d->open(output, bool(nativeFrame), error)) return {};
    AVFrame* encodeFrame = d->context->pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX ? nativeFrame.get() : d->converted.get();
    int status = 0;
    if (d->context->pix_fmt != AV_PIX_FMT_VIDEOTOOLBOX) {
        AVFrame* scaleTarget = d->converted.get();
        if (rotation != 0 || mirrored) {
            if (!d->orientedInput || d->orientedInput->width != scaled.width() || d->orientedInput->height != scaled.height()
                || d->orientedInput->format != d->context->pix_fmt) {
                d->orientedInput.reset(av_frame_alloc());
                if (!d->orientedInput) { error = QStringLiteral("Cannot allocate rotated screen frame"); return {}; }
                d->orientedInput->width = scaled.width(); d->orientedInput->height = scaled.height();
                d->orientedInput->format = d->context->pix_fmt;
                const int allocated = av_frame_get_buffer(d->orientedInput.get(), 32);
                if (allocated < 0) { error = failure("Allocate rotated screen planes", allocated); return {}; }
            }
            scaleTarget = d->orientedInput.get();
        }
        if (!frame.map(QVideoFrame::ReadOnly)) { error = QStringLiteral("Cannot map the captured screen video buffer"); return {}; }
        std::array<const uint8_t*, 4> planes{};
        std::array<int, 4> strides{};
        for (int i = 0; i < frame.planeCount() && i < 4; ++i) { planes[i] = frame.bits(i); strides[i] = frame.bytesPerLine(i); }
        if (frame.pixelFormat() == QVideoFrameFormat::Format_YV12) { std::swap(planes[1], planes[2]); std::swap(strides[1], strides[2]); }
        if (frame.surfaceFormat().scanLineDirection() == QVideoFrameFormat::BottomToTop) {
            const auto* descriptor = av_pix_fmt_desc_get(sourceFormat);
            for (int i = 0; i < frame.planeCount() && i < 4; ++i) {
                const int height = i == 0 ? frame.height() : AV_CEIL_RSHIFT(frame.height(), descriptor->log2_chroma_h);
                planes[i] += (height - 1) * strides[i]; strides[i] = -strides[i];
            }
        }
        d->scaler = sws_getCachedContext(d->scaler, frame.width(), frame.height(), sourceFormat,
            scaled.width(), scaled.height(), d->context->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        status = d->scaler ? av_frame_make_writable(d->converted.get()) : AVERROR(ENOMEM);
        if (status >= 0) {
            const bool rgb = (av_pix_fmt_desc_get(sourceFormat)->flags & AV_PIX_FMT_FLAG_RGB) != 0;
            const int inputSpace = frame.surfaceFormat().colorSpace() == QVideoFrameFormat::ColorSpace_BT601 ? SWS_CS_ITU601 : SWS_CS_ITU709;
            const int inputRange = rgb || frame.surfaceFormat().colorRange() == QVideoFrameFormat::ColorRange_Full;
            sws_setColorspaceDetails(d->scaler, sws_getCoefficients(inputSpace), inputRange,
                sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
            status = sws_scale(d->scaler, planes.data(), strides.data(), 0, frame.height(), scaleTarget->data, scaleTarget->linesize);
        }
        frame.unmap();
        if (status < 0) { error = failure("Convert screen capture planes", status); return {}; }
        if (scaleTarget != d->converted.get()) orientPlanes(scaleTarget, d->converted.get(), rotation, mirrored);
    }
    const qint64 timestamp = std::max(d->lastTimestamp + 1, std::max<qint64>(0, frame.startTime()));
    d->lastTimestamp = timestamp;
    forceKeyFrame |= d->pendingKeyTimestamp < 0
        && timestamp - d->lastKeyTimestamp >= qint64(d->profile.keyFrameIntervalMs) * 1000;
    if (forceKeyFrame) d->pendingKeyTimestamp = timestamp;
    encodeFrame->pts = timestamp;
    encodeFrame->pict_type = forceKeyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    status = avcodec_send_frame(d->context.get(), encodeFrame);
    if (status < 0) {
        if (d->preferHardware && d->backend.startsWith(QStringLiteral("h264_"))) {
            d = std::make_unique<Private>(false, d->profile);
            return encode(frame, true, error);
        }
        error = failure("Encode screen", status); return {};
    }
    ++d->pendingFrames;
    QList<ScreenStreamPacket> result;
    Packet packet(av_packet_alloc()), filtered(av_packet_alloc());
    if (!packet || !filtered) { error = QStringLiteral("Cannot allocate screen stream packet"); return {}; }
    while ((status = avcodec_receive_packet(d->context.get(), packet.get())) >= 0) {
        if (d->pendingFrames > 0) --d->pendingFrames;
        status = av_bsf_send_packet(d->annexBFilter.get(), packet.get());
        if (status < 0) { error = failure("Convert screen packet to Annex B", status); return {}; }
        while ((status = av_bsf_receive_packet(d->annexBFilter.get(), filtered.get())) >= 0) {
            ScreenStreamPacket outputPacket;
            outputPacket.annexB = QByteArray(reinterpret_cast<const char*>(filtered->data), filtered->size);
            outputPacket.size = output;
            outputPacket.timestampUs = filtered->pts == AV_NOPTS_VALUE ? timestamp : filtered->pts;
            const auto nals = inspectAnnexB(outputPacket.annexB);
            if (!nals.sps.isEmpty()) d->sps = nals.sps;
            if (!nals.pps.isEmpty()) d->pps = nals.pps;
            outputPacket.keyFrame = nals.idr;
            if (outputPacket.keyFrame) {
                if (d->sps.isEmpty() || d->pps.isEmpty()) { error = QStringLiteral("H.264 keyframe has no parameter sets"); return {}; }
                if (nals.sps.isEmpty() || nals.pps.isEmpty()) outputPacket.annexB.prepend(d->sps + d->pps);
                d->lastKeyTimestamp = outputPacket.timestampUs;
                if (outputPacket.timestampUs >= d->pendingKeyTimestamp) d->pendingKeyTimestamp = -1;
            }
            av_packet_unref(filtered.get());
            if (outputPacket.annexB.size() > MaximumPacketBytes) { error = QStringLiteral("Encoded screen frame exceeds the stream limit"); return {}; }
            result.append(std::move(outputPacket));
        }
        if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) { error = failure("Read Annex B screen packet", status); return {}; }
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) error = failure("Read encoded screen packet", status);
    if (d->pendingFrames > 3 && d->preferHardware && d->backend.startsWith(QStringLiteral("h264_"))) {
        // Do not retain an unbounded sequence of native capture surfaces if a
        // hardware driver stalls. Restart explicitly at an IDR on the CPU.
        d = std::make_unique<Private>(false, d->profile);
        return encode(frame, true, error);
    }
    return result;
}

struct ScreenStreamDecoder::Private {
    Codec context;
};
ScreenStreamDecoder::ScreenStreamDecoder() : d(std::make_unique<Private>()) {}
ScreenStreamDecoder::~ScreenStreamDecoder() = default;
void ScreenStreamDecoder::reset() { d = std::make_unique<Private>(); }

QVideoFrame ScreenStreamDecoder::decode(const QByteArray& annexB, qint64 timestampUs, QString& error) {
    error.clear();
    if (annexB.isEmpty() || annexB.size() > ScreenStreamEncoder::MaximumPacketBytes) {
        error = QStringLiteral("Invalid screen stream packet size"); return {};
    }
    if (!d->context) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) { error = QStringLiteral("H.264 decoder is unavailable"); return {}; }
        d->context.reset(avcodec_alloc_context3(codec));
        if (!d->context) { error = QStringLiteral("Cannot allocate screen decoder"); return {}; }
        d->context->thread_count = 2;
        d->context->thread_type = FF_THREAD_SLICE;
        d->context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        d->context->max_pixels = int64_t(ScreenStreamEncoder::MaximumDecodeEdge) * ScreenStreamEncoder::MaximumDecodeEdge;
        d->context->pkt_timebase = AVRational{1, 1000000};
        d->context->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
        const int status = avcodec_open2(d->context.get(), codec, nullptr);
        if (status < 0) { error = failure("Open screen decoder", status); reset(); return {}; }
    }
    Packet packet(av_packet_alloc());
    Frame frame(av_frame_alloc());
    if (!packet || !frame) { error = QStringLiteral("Cannot allocate screen decoder frame"); return {}; }
    int status = av_new_packet(packet.get(), int(annexB.size()));
    if (status < 0) { error = failure("Allocate encoded screen packet", status); return {}; }
    // av_new_packet supplies the zero padding required by the bitstream reader.
    memcpy(packet->data, annexB.constData(), size_t(annexB.size()));
    packet->pts = packet->dts = timestampUs;
    status = avcodec_send_packet(d->context.get(), packet.get());
    if (status < 0) { error = failure("Decode screen", status); reset(); return {}; }
    QVideoFrame latest;
    while ((status = avcodec_receive_frame(d->context.get(), frame.get())) >= 0) {
        if (frame->width < 2 || frame->height < 2 || frame->width > ScreenStreamEncoder::MaximumDecodeEdge
            || frame->height > ScreenStreamEncoder::MaximumDecodeEdge
            || (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P && frame->format != AV_PIX_FMT_NV12)) {
            error = QStringLiteral("Screen decoder returned an unsupported format or dimensions"); reset(); return {};
        }
        Frame retained(av_frame_clone(frame.get()));
        if (!retained) { error = QStringLiteral("Cannot retain decoded screen frame"); return {}; }
        latest = QVideoFrame(std::make_unique<DecodedBuffer>(std::move(retained)));
        latest.setStartTime(frame->pts == AV_NOPTS_VALUE ? timestampUs : frame->pts);
        latest.setEndTime(latest.startTime() + 1000000 / ScreenStreamEncoder::FramesPerSecond);
        av_frame_unref(frame.get());
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) { error = failure("Read decoded screen frame", status); reset(); return {}; }
    return latest;
}
