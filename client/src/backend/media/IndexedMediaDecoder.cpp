#include "backend/media/IndexedMediaDecoder.h"
#include <QVideoFrame>
#include <QTransform>
#include <algorithm>
#include <cstring>
#include <limits>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
namespace {
constexpr AVRational us{1, 1000000};
constexpr quint64 MiB = 1024ULL * 1024;
struct CodecDelete { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using Codec = std::unique_ptr<AVCodecContext, CodecDelete>;
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
using Packet = std::unique_ptr<AVPacket, PacketDelete>;
QString message(int result) { char buffer[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(result, buffer, sizeof(buffer)); return QString::fromUtf8(buffer); }
std::shared_ptr<AVCodecParameters> parameters(const AVCodecParameters* source) {
    std::shared_ptr<AVCodecParameters> result(avcodec_parameters_alloc(), [](auto* p) { avcodec_parameters_free(&p); });
    if (!result || avcodec_parameters_copy(result.get(), source) < 0) return {};
    return result;
}
Codec decoder(const AVCodecParameters* parameters, AVRational timeBase, QString& error) {
    auto* codec = avcodec_find_decoder(parameters->codec_id);
    Codec result(avcodec_alloc_context3(codec));
    if (!codec || !result) { error = QStringLiteral("Decoder unavailable"); return {}; }
    int status = avcodec_parameters_to_context(result.get(), parameters);
    result->thread_count = 1;
    result->pkt_timebase = timeBase;
    result->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
    if (status >= 0) status = avcodec_open2(result.get(), codec, nullptr);
    if (status < 0) { error = message(status); return {}; }
    return result;
}
struct Input {
    QByteArray bytes;
    qint64 position = 0;
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;
    std::function<bool()> cancelled;
    explicit Input(const QByteArray& data) : bytes(data) {}
    ~Input() { avformat_close_input(&format); if (io) { av_freep(&io->buffer); avio_context_free(&io); } }
    static int read(void* opaque, uint8_t* target, int size) {
        auto& self = *static_cast<Input*>(opaque);
        if (self.cancelled && self.cancelled()) return AVERROR_EXIT;
        int count = int(std::min<qint64>(size, self.bytes.size() - self.position));
        if (count <= 0) return AVERROR_EOF;
        memcpy(target, self.bytes.constData() + self.position, size_t(count)); self.position += count; return count;
    }
    static int64_t seek(void* opaque, int64_t offset, int mode) {
        auto& self = *static_cast<Input*>(opaque);
        if (mode == AVSEEK_SIZE) return self.bytes.size();
        mode &= ~AVSEEK_FORCE;
        qint64 base = mode == SEEK_SET ? 0 : mode == SEEK_CUR ? self.position : mode == SEEK_END ? self.bytes.size() : -1;
        if (base < 0 || offset < -base || offset > self.bytes.size() - base) return AVERROR(EINVAL);
        return self.position = base + offset;
    }
    bool open(QString& error) {
        auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
        if (buffer) io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
        if (!io) { av_free(buffer); error = QStringLiteral("memory_unavailable"); return false; }
        format = avformat_alloc_context();
        if (!format) { error = QStringLiteral("memory_unavailable"); return false; }
        format->pb = io; format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->interrupt_callback = {[](void* opaque) -> int {
            auto& self = *static_cast<Input*>(opaque);
            return self.cancelled && self.cancelled();
        }, this};
        int status = avformat_open_input(&format, nullptr, nullptr, nullptr);
        if (status >= 0) status = avformat_find_stream_info(format, nullptr);
        if (status < 0) { error = message(status); return false; }
        return true;
    }
};
MediaPacket appendPacket(MediaPacketStore& store, const AVPacket* packet) {
    MediaPacket index{store.bytes.size(), packet->size, packet->pts, packet->dts, packet->duration, packet->flags, {}};
    size_t skipSize = 0;
    const auto* skip = av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &skipSize);
    if (skip) index.skipSamples = QByteArray(reinterpret_cast<const char*>(skip), qsizetype(skipSize));
    store.packetSideDataBytes += index.skipSamples.capacity();
    store.bytes.append(reinterpret_cast<const char*>(packet->data), packet->size);
    store.bytes.append(AV_INPUT_BUFFER_PADDING_SIZE, '\0');
    store.packets.append(index);
    return index;
}
Packet packetAt(const MediaPacketStore& store, const QByteArray& bytes, int index) {
    if (index < 0 || index >= store.packets.size()) return {};
    const auto& entry = store.packets[index];
    if (entry.offset < 0 || entry.size < 0 || entry.offset + entry.size > bytes.size()) return {};
    Packet result(av_packet_alloc());
    if (!result || av_new_packet(result.get(), entry.size) < 0) return {};
    memcpy(result->data, bytes.constData() + entry.offset, size_t(entry.size));
    result->pts = entry.pts; result->dts = entry.dts; result->duration = entry.duration; result->flags = entry.flags;
    if (!entry.skipSamples.isEmpty()) {
        auto* side = av_packet_new_side_data(result.get(), AV_PKT_DATA_SKIP_SAMPLES, size_t(entry.skipSamples.size()));
        if (!side) return {};
        memcpy(side, entry.skipSamples.constData(), size_t(entry.skipSamples.size()));
    }
    return result;
}
}
quint64 MediaPacketStore::allocatedBytes() const {
    quint64 result = bytes.capacity() + quint64(packets.capacity()) * sizeof(MediaPacket);
    if (codec) result += sizeof(AVCodecParameters) + codec->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE;
    return result + packetSideDataBytes;
}
void MediaPacketStore::compact() {
    bytes.squeeze(); packets.squeeze(); packetSideDataBytes = 0;
    for (auto& p : packets) { p.skipSamples.squeeze(); packetSideDataBytes += p.skipSamples.capacity(); }
}

bool IndexedMediaDecoder::build(ResidentMediaAsset& asset, const MediaDecoder::DecodeCallbacks& callbacks,
                               quint64 scratch, QString& error) {
    Input input(asset.compressedVideo);
    if (!input.open(error)) return false;
    auto* format = input.format;
    int vi = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int ai = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);
    if (vi < 0) { error = QStringLiteral("No video stream"); return false; }
    auto* stream = format->streams[vi];
    const bool validatedOriginal = callbacks.retainOriginalVideo;
    const quint64 validatedIndexBytes = validatedOriginal
        ? quint64(asset.frameIndex.capacity()) * sizeof(MediaFrameIndex) : 0;
    Codec decode;
    if (!validatedOriginal) {
        decode = decoder(stream->codecpar, stream->time_base, error);
        if (!decode) return false;
    } else {
        if (asset.frameIndex.isEmpty()) { error = QStringLiteral("Missing validated frame index"); return false; }
        asset.representationReason = QStringLiteral("Original: validated source retained for interactive import");
    }
    asset.sourceFileBytes = asset.compressedVideo.size();
    asset.sourceOriginUs = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
    asset.codedDisplaySize = asset.firstFrame.frame.size();
    asset.rotation = int(asset.firstFrame.frame.rotation());
    asset.videoPackets.codec = parameters(stream->codecpar);
    if (ai >= 0) {
        auto* audio = format->streams[ai];
        asset.audioPackets.codec = parameters(audio->codecpar);
        asset.audioPackets.timeBaseNum = audio->time_base.num;
        asset.audioPackets.timeBaseDen = audio->time_base.den;
        asset.audioPackets.originUs = asset.sourceOriginUs;
    }
    QVector<qint64> originalAudioOffsets;
    bool audioOffsetsValid = true;
    Codec encode;
    bool conversionAttempted = false, conversionDisabled = false;
    double publishedProgress = -1;
    const auto publishProgress = [&](double progress) {
        if (callbacks.progress && progress - publishedProgress >= 0.005) {
            publishedProgress = progress;
            callbacks.progress(progress);
        }
    };
    Frame frame(av_frame_alloc()); Packet packet(av_packet_alloc()), encoded(av_packet_alloc());
    if (!frame || !packet || !encoded) { error = QStringLiteral("memory_unavailable"); return false; }
    auto check = [&](quint64 growth = 0) {
        quint64 retained = asset.residentBytes + asset.videoPackets.allocatedBytes() + asset.audioPackets.allocatedBytes()
            - (asset.audioUsesVideoBytes ? asset.audioPackets.bytes.capacity() : 0)
            + quint64(asset.frameIndex.capacity()) * sizeof(MediaFrameIndex) - validatedIndexBytes;
        asset.conversionPeakBytes = std::max(asset.conversionPeakBytes, retained + scratch + growth);
        if (callbacks.cancelled && callbacks.cancelled()) { error = QStringLiteral("cancelled"); return false; }
        if (callbacks.reserve && !callbacks.reserve(retained + scratch + growth)) { error = QStringLiteral("memory_unavailable"); return false; }
        if (callbacks.allocated) callbacks.allocated(retained);
        if (callbacks.allocatedBreakdown) { auto memory = asset.memoryBreakdown(); memory.videoBytes += retained - asset.residentBytes; callbacks.allocatedBreakdown(memory); }
        return true;
    };
    auto disable = [&] (const QString& reason) {
        conversionDisabled = true; encode.reset(); asset.videoPackets.bytes.clear(); asset.videoPackets.packets.clear();
        asset.videoPackets.packetSideDataBytes = 0;
        asset.representationReason = reason;
    };
    auto encodedPackets = [&] {
        for (;;) {
            int status = avcodec_receive_packet(encode.get(), encoded.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return true;
            if (status < 0) { error = message(status); return false; }
            if (!(encoded->flags & AV_PKT_FLAG_KEY)) { error = QStringLiteral("Non-independent internal video frame"); return false; }
            // QByteArray growth can temporarily retain the old allocation as well.
            if (!check(quint64(asset.videoPackets.bytes.capacity()) + quint64(encoded->size) * 2 + 4096)) return false;
            appendPacket(asset.videoPackets, encoded.get()); av_packet_unref(encoded.get());
        }
    };
    auto consume = [&] {
        for (;;) {
            int status = avcodec_receive_frame(decode.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return true;
            if (status < 0) { error = message(status); return false; }
            if (!check(sizeof(MediaFrameIndex) * (asset.frameIndex.capacity() + 2))) return false;
            qint64 pts = frame->best_effort_timestamp == AV_NOPTS_VALUE
                ? (asset.frameIndex.isEmpty() ? 0 : asset.frameIndex.last().timestampUs + asset.frameIndex.last().durationUs)
                : av_rescale_q(frame->best_effort_timestamp, stream->time_base, us) - asset.sourceOriginUs;
            qint64 duration = frame->duration > 0 ? av_rescale_q(frame->duration, stream->time_base, us)
                : asset.firstFrame.durationUs;
            asset.frameIndex.append({std::max<qint64>(0, pts), std::max<qint64>(1, duration)});
            const auto* pixel = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
            bool supported = pixel && !(pixel->flags & (AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_RGB))
                && pixel->comp[0].depth == 8 && frame->color_trc != AVCOL_TRC_SMPTE2084
                && frame->color_trc != AVCOL_TRC_ARIB_STD_B67
                && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUV422P || frame->format == AV_PIX_FMT_YUV444P);
            if (!supported && !conversionDisabled) disable(QStringLiteral("Original: HDR, alpha or unsupported pixel format"));
            if (!conversionAttempted && !conversionDisabled) {
                conversionAttempted = true;
                const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
                encode.reset(avcodec_alloc_context3(codec));
                if (!codec || !encode) disable(QStringLiteral("Original: H.264 intra encoder unavailable"));
                else {
                    encode->width = frame->width; encode->height = frame->height;
                    encode->pix_fmt = AVPixelFormat(frame->format); encode->time_base = us;
                    encode->framerate = av_guess_frame_rate(format, stream, frame.get());
                    encode->sample_aspect_ratio = frame->sample_aspect_ratio;
                    encode->color_range = frame->color_range; encode->colorspace = frame->colorspace;
                    encode->color_primaries = frame->color_primaries; encode->color_trc = frame->color_trc;
                    encode->chroma_sample_location = frame->chroma_location;
                    encode->gop_size = 1; encode->max_b_frames = 0; encode->thread_count = 2;
                    AVDictionary* options = nullptr;
                    av_dict_set(&options, "preset", "veryfast", 0); av_dict_set(&options, "crf", "18", 0);
                    av_dict_set(&options, "tune", "zerolatency", 0);
                    av_dict_set(&options, "x264-params", "keyint=1:min-keyint=1:scenecut=0:bframes=0:repeat-headers=1", 0);
                    status = avcodec_open2(encode.get(), codec, &options); av_dict_free(&options);
                    if (status < 0) disable(QStringLiteral("Original: %1").arg(message(status)));
                }
            }
            if (encode && (frame->width != encode->width || frame->height != encode->height || frame->format != encode->pix_fmt))
                disable(QStringLiteral("Original: changing video format"));
            if (encode) {
                frame->pts = asset.frameIndex.last().timestampUs; frame->duration = asset.frameIndex.last().durationUs;
                frame->pict_type = AV_PICTURE_TYPE_I;
                status = avcodec_send_frame(encode.get(), frame.get());
                if (status < 0) { error = message(status); return false; }
                if (!encodedPackets()) return false;
            }
            av_frame_unref(frame.get());
            if (asset.durationUs > 0) publishProgress(0.2 + 0.6 * std::min(1.0, double(pts) / asset.durationUs));
        }
    };
    int status = 0;
    while ((status = av_read_frame(format, packet.get())) >= 0) {
        if (!check()) return false;
        if (packet->stream_index == ai) {
            if (!check(quint64(asset.audioPackets.bytes.capacity()) + quint64(packet->size) * 2 + 4096)) return false;
            originalAudioOffsets.append(packet->pos);
            audioOffsetsValid = audioOffsetsValid && packet->pos >= 0 && packet->pos + packet->size <= asset.compressedVideo.size()
                && std::memcmp(asset.compressedVideo.constData() + packet->pos, packet->data, size_t(packet->size)) == 0;
            appendPacket(asset.audioPackets, packet.get());
        }
        if (packet->stream_index == vi && decode) {
            int result = avcodec_send_packet(decode.get(), packet.get());
            if (result == AVERROR(EAGAIN)) { if (!consume()) return false; result = avcodec_send_packet(decode.get(), packet.get()); }
            if (result < 0) { error = message(result); return false; }
            if (!consume()) return false;
        }
        if (validatedOriginal && packet->pos >= 0 && asset.sourceFileBytes > 0)
            publishProgress(0.9 + 0.09 * std::min(1.0, double(packet->pos) / asset.sourceFileBytes));
        av_packet_unref(packet.get());
    }
    if (status != AVERROR_EOF) { error = message(status); return false; }
    if (decode) {
        status = avcodec_send_packet(decode.get(), nullptr);
        if (status < 0 && status != AVERROR_EOF) { error = message(status); return false; }
        if (!consume()) return false;
    }
    if (encode) {
        status = avcodec_send_frame(encode.get(), nullptr);
        if (status < 0 && status != AVERROR_EOF) { error = message(status); return false; }
        if (!encodedPackets()) return false;
        if (asset.videoPackets.packets.size() != asset.frameIndex.size()) { error = QStringLiteral("Incomplete all-intra conversion"); return false; }
        asset.videoPackets.codec.reset(avcodec_parameters_alloc(), [](auto* p) { avcodec_parameters_free(&p); });
        if (!asset.videoPackets.codec || avcodec_parameters_from_context(asset.videoPackets.codec.get(), encode.get()) < 0) return false;
        asset.allIntra = true;
    }
    for (qsizetype i = 1; i < asset.frameIndex.size(); ++i) {
        if (asset.frameIndex[i].timestampUs <= asset.frameIndex[i-1].timestampUs) {
            error = QStringLiteral("Non-monotonic presentation timestamps"); return false;
        }
        asset.frameIndex[i-1].durationUs = asset.frameIndex[i].timestampUs - asset.frameIndex[i-1].timestampUs;
    }
    if (validatedOriginal) {
        asset.firstFrame.durationUs = asset.frameIndex.first().durationUs;
        asset.firstFrame.frame.setEndTime(asset.firstFrame.timestampUs + asset.firstFrame.durationUs);
    }
    if (!check(quint64(asset.videoPackets.bytes.size() + asset.audioPackets.bytes.size()) * 2)) return false;
    if (asset.allIntra) {
        const qint64 base = asset.videoPackets.bytes.size();
        asset.videoPackets.bytes.append(asset.audioPackets.bytes);
        for (auto& packet : asset.audioPackets.packets) packet.offset += base;
        asset.audioPackets.bytes.clear();
        asset.videoPackets.compact();
        asset.audioPackets.bytes = asset.videoPackets.bytes;
        asset.audioUsesVideoBytes = true;
    } else if (audioOffsetsValid) {
        for (int i = 0; i < asset.audioPackets.packets.size(); ++i) asset.audioPackets.packets[i].offset = originalAudioOffsets[i];
        asset.audioPackets.bytes = asset.compressedVideo;
        asset.audioUsesVideoBytes = true;
    }
    asset.videoPackets.packets.squeeze(); asset.audioPackets.packets.squeeze(); asset.frameIndex.squeeze();
    if (!asset.audioUsesVideoBytes) asset.audioPackets.compact();
    // Validate every internal packet independently before retiring original bytes.
    if (asset.allIntra) {
        IndexedMediaDecoder validation;
        for (int i = 0; i < asset.frameIndex.size(); ++i) {
            if (!check()) return false;
            auto verified = validation.videoFrame(asset, i, error);
            if (!verified.isValid()) return false;
            if (i == 0) asset.firstFrame = {verified, verified.startTime(), verified.endTime() - verified.startTime()};
            // Verification decodes the complete internal representation too.
            // It must advance progress instead of appearing stuck at 99%.
            publishProgress(0.8 + 0.19 * double(i + 1) / asset.frameIndex.size());
        }
    }
    if (asset.audioPackets.codec) {
        if (!check(200 * 48 * 2 * sizeof(float))) return false;
        IndexedMediaDecoder validation;
        asset.firstAudioPcm = validation.audio(asset, 0, 9600, 48000, 2, error);
        if (asset.firstAudioPcm.isEmpty()) return false;
        asset.firstAudioPcm.squeeze();
    }
    // Input's AVIO is not used after this point, and the disk source remains intact.
    if (asset.allIntra) asset.compressedVideo.clear();
    if (!asset.allIntra) asset.playbackBudgetBytes = std::max(asset.playbackBudgetBytes, 8 * MiB + asset.posterBytes * 16);
    asset.residentBytes = asset.compressedVideo.capacity() + asset.videoPackets.allocatedBytes()
        + asset.audioPackets.allocatedBytes() - (asset.audioUsesVideoBytes ? asset.audioPackets.bytes.capacity() : 0)
        + quint64(asset.frameIndex.capacity()) * sizeof(MediaFrameIndex) + asset.posterBytes + asset.thumbnailBytes + asset.firstAudioPcm.capacity();
    if (callbacks.allocated) callbacks.allocated(asset.residentBytes);
    if (callbacks.allocatedBreakdown) callbacks.allocatedBreakdown(asset.memoryBreakdown());
    return true;
}

struct IndexedMediaDecoder::Impl {
    std::shared_ptr<AVCodecParameters> identity;
    Codec video;
    // At most one inter-frame session per worker. Input owns a shared reference
    // to the compressed allocation, never a second copy of the source bytes.
    std::unique_ptr<Input> original;
    QString originalIdentity;
    int streamIndex = -1;
    qint64 lastPts = AV_NOPTS_VALUE;
    bool draining = false;
};
IndexedMediaDecoder::IndexedMediaDecoder() : d(std::make_unique<Impl>()) {}
IndexedMediaDecoder::~IndexedMediaDecoder() = default;
int IndexedMediaDecoder::frameAt(const ResidentMediaAsset& asset, qint64 timestampUs) {
    auto it = std::upper_bound(asset.frameIndex.cbegin(), asset.frameIndex.cend(), timestampUs,
        [](qint64 time, const MediaFrameIndex& frame) { return time < frame.timestampUs; });
    return asset.frameIndex.isEmpty() ? -1 : int(std::max<qsizetype>(0, it - asset.frameIndex.cbegin() - 1));
}
AVFrame* IndexedMediaDecoder::decodeFrame(const ResidentMediaAsset& asset, int index, QString& error,
                                         const Cancelled& cancelled) {
    auto interrupted = [&] {
        if (!cancelled || !cancelled()) return false;
        error = QStringLiteral("cancelled");
        // An interrupted demux read may set an AVIO error. Reopen on the next
        // request instead of leaving a poisoned cached session behind.
        d->original.reset(); d->video.reset(); d->identity.reset();
        d->originalIdentity.clear(); d->lastPts = AV_NOPTS_VALUE;
        return true;
    };
    if (interrupted()) return {};
    if (index < 0 || index >= asset.frameIndex.size()) { error = QStringLiteral("Invalid frame index"); return {}; }
    Frame frame(av_frame_alloc());
    if (!frame) { error = QStringLiteral("memory_unavailable"); return {}; }
    const auto timing = asset.frameIndex[index];
    if (asset.allIntra) {
        if (d->original) { d->video.reset(); d->original.reset(); d->originalIdentity.clear(); }
        if (!d->video || !d->identity || d->identity->codec_id != asset.videoPackets.codec->codec_id) {
            d->video = decoder(asset.videoPackets.codec.get(), us, error);
            d->identity = d->video ? asset.videoPackets.codec : nullptr;
        }
        if (!d->video) return {};
        auto packet = packetAt(asset.videoPackets, asset.videoPackets.bytes, index);
        if (!packet) { error = QStringLiteral("Invalid resident packet"); return {}; }
        int status = avcodec_send_packet(d->video.get(), packet.get());
        if (status >= 0) status = avcodec_receive_frame(d->video.get(), frame.get());
        if (status < 0 || frame->decode_error_flags) { error = status < 0 ? message(status) : QStringLiteral("Invalid intra frame"); d->identity = nullptr; return {}; }
    } else {
        if (!d->original || d->originalIdentity != asset.sha256) {
            d->video.reset(); d->identity.reset(); d->original.reset();
            auto input = std::make_unique<Input>(asset.compressedVideo);
            input->cancelled = cancelled;
            if (!input->open(error)) { interrupted(); return {}; }
            d->streamIndex = av_find_best_stream(input->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (d->streamIndex < 0) { error = QStringLiteral("Missing original video stream"); return {}; }
            auto* stream = input->format->streams[d->streamIndex];
            d->video = decoder(stream->codecpar, stream->time_base, error);
            if (!d->video) return {};
            d->original = std::move(input); d->originalIdentity = asset.sha256;
            d->lastPts = AV_NOPTS_VALUE; d->draining = false;
        }
        auto* format = d->original->format;
        d->original->cancelled = cancelled;
        auto* stream = format->streams[d->streamIndex];
        const qint64 target = av_rescale_q(timing.timestampUs + asset.sourceOriginUs, us, stream->time_base);
        // Continue sequential requests without reopening/demuxing the file. A
        // reverse or distant seek resets only this bounded worker's session.
        if (d->lastPts == AV_NOPTS_VALUE || target <= d->lastPts
            || target - d->lastPts > av_rescale_q(1000000, us, stream->time_base)) {
            int status = av_seek_frame(format, d->streamIndex, target, AVSEEK_FLAG_BACKWARD);
            if (interrupted()) return {};
            if (status < 0) { error = message(status); return {}; }
            avcodec_flush_buffers(d->video.get()); d->draining = false;
        }
        Packet packet(av_packet_alloc());
        if (!packet) { error = QStringLiteral("memory_unavailable"); return {}; }
        for (;;) {
            if (interrupted()) return {};
            int status = avcodec_receive_frame(d->video.get(), frame.get());
            if (status >= 0) {
                d->lastPts = frame->best_effort_timestamp;
                if (d->lastPts >= target) break;
                av_frame_unref(frame.get());
                continue;
            }
            if (status != AVERROR(EAGAIN)) { error = QStringLiteral("No original frame at requested timestamp: %1").arg(message(status)); return {}; }
            if (d->draining) { error = QStringLiteral("Incomplete original video drain"); return {}; }
            do {
                if (interrupted()) return {};
                av_packet_unref(packet.get());
                status = av_read_frame(format, packet.get());
                if (interrupted()) return {};
            } while (status >= 0 && packet->stream_index != d->streamIndex);
            if (status < 0 && status != AVERROR_EOF) { error = message(status); return {}; }
            d->draining = status == AVERROR_EOF;
            status = avcodec_send_packet(d->video.get(), d->draining ? nullptr : packet.get());
            if (status < 0) { error = message(status); return {}; }
        }
    }
    if (interrupted()) return {};
    return frame.release();
}
QVideoFrame IndexedMediaDecoder::videoFrame(const ResidentMediaAsset& asset, int index, QString& error,
                                           const Cancelled& cancelled) {
    Frame frame(decodeFrame(asset, index, error, cancelled));
    if (!frame) return {};
    const auto timing = asset.frameIndex[index];
    return mediaFrameFromAv(frame.get(), asset.codedDisplaySize, asset.rotation,
                            timing.timestampUs, timing.timestampUs + timing.durationUs, error);
}

QImage IndexedMediaDecoder::thumbnail(const ResidentMediaAsset& asset, int index, QString& error,
                                      const Cancelled& cancelled) {
    return previewImage(asset, index, QSize(MediaThumbnails::Width, MediaThumbnails::Height), error, cancelled);
}
QImage IndexedMediaDecoder::previewImage(const ResidentMediaAsset& asset, int index, QSize bounds,
                                         QString& error, const Cancelled& cancelled) {
    if (!bounds.isValid() || bounds.width() > 2048 || bounds.height() > 2048) return {};
    Frame frame(decodeFrame(asset, index, error, cancelled));
    if (!frame) return {};
    const bool quarterTurn = asset.rotation % 180 != 0;
    QSize unrotated = asset.codedDisplaySize.isValid() ? asset.codedDisplaySize : QSize(frame->width, frame->height);
    const QSize target = unrotated.scaled(quarterTurn ? bounds.transposed() : bounds, Qt::KeepAspectRatio);
    // Keep FFmpeg/Qt color handling (including HDR metadata) at preview size.
    // No full-size QVideoFrame or RGB image is allocated on this path.
    auto small = mediaFrameFromAv(frame.get(), target, 0, 0, 1, error);
    if (!small.isValid() || (cancelled && cancelled())) return {};
    QImage image = small.toImage();
    if (asset.rotation) image = image.transformed(QTransform().rotate(asset.rotation));
    return image;
}

QByteArray IndexedMediaDecoder::audio(const ResidentMediaAsset& asset, qint64 startUs, int frames,
                                    int sampleRate, int channels, QString& error) {
    if (frames <= 0 || sampleRate <= 0 || channels <= 0 || channels > 8) return {};
    QByteArray result(qsizetype(frames) * channels * sizeof(float), '\0');
    const auto& store = asset.audioPackets;
    if (!store.codec || store.packets.isEmpty()) return result;
    AVRational timeBase{store.timeBaseNum, store.timeBaseDen};
    auto codec = decoder(store.codec.get(), timeBase, error);
    if (!codec) return {};
    AVChannelLayout layout{}; av_channel_layout_default(&layout, channels);
    SwrContext* raw = nullptr;
    int status = swr_alloc_set_opts2(&raw, &layout, AV_SAMPLE_FMT_FLT, sampleRate,
        &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&layout);
    std::unique_ptr<SwrContext, void(*)(SwrContext*)> resampler(raw, [](auto* p) { swr_free(&p); });
    if (status < 0 || !raw || swr_init(raw) < 0) { error = QStringLiteral("Audio resampling failed"); return {}; }
    qint64 target = av_rescale_q(startUs + store.originUs, us, timeBase);
    auto it = std::lower_bound(store.packets.cbegin(), store.packets.cend(), target,
        [](const MediaPacket& packet, qint64 pts) { return packet.pts < pts; });
    int begin = int(std::max<qsizetype>(0, it - store.packets.cbegin() - 16));
    Frame frame(av_frame_alloc());
    qint64 trackEndUs = asset.durationUs;
    const auto& tail = store.packets.last();
    if (tail.duration > 0 && tail.pts != AV_NOPTS_VALUE)
        trackEndUs = std::min(trackEndUs, av_rescale_q(tail.pts + tail.duration, timeBase, us) - store.originUs);
    const int validFrames = int(std::clamp<qint64>(av_rescale_q(trackEndUs-startUs, us, AVRational{1, sampleRate}), 0, frames));
    if (!validFrames) return result;
    const qint64 endUs = startUs + av_rescale_q(validFrames, AVRational{1, sampleRate}, us);
    qint64 nextUs = startUs;
    bool complete = false;
    for (int i = begin; i <= store.packets.size() && !complete; ++i) {
        auto packet = packetAt(store, store.bytes, i);
        status = avcodec_send_packet(codec.get(), packet.get());
        if (status < 0 && status != AVERROR_EOF) { error = message(status); return {}; }
        while ((status = avcodec_receive_frame(codec.get(), frame.get())) >= 0) {
            qint64 pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? nextUs
                : av_rescale_q(frame->best_effort_timestamp, timeBase, us) - store.originUs;
            int capacity = swr_get_out_samples(raw, frame->nb_samples);
            QByteArray converted(qsizetype(capacity) * channels * sizeof(float), Qt::Uninitialized);
            auto* data = reinterpret_cast<uint8_t*>(converted.data());
            qint64 delay = av_rescale_q(swr_get_delay(raw, codec->sample_rate), AVRational{1, codec->sample_rate}, us);
            int count = swr_convert(raw, &data, capacity, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (count < 0) { error = message(count); return {}; }
            pts -= delay;
            qint64 destination = av_rescale_q(pts - startUs, us, AVRational{1, sampleRate});
            qint64 first = std::max<qint64>(0, -destination);
            qint64 last = std::min<qint64>(count, validFrames - destination);
            if (last > first) memcpy(result.data() + (destination + first) * channels * sizeof(float),
                converted.constData() + first * channels * sizeof(float), size_t(last-first) * channels * sizeof(float));
            nextUs = pts + av_rescale_q(count, AVRational{1, sampleRate}, us);
            complete = nextUs >= endUs;
            av_frame_unref(frame.get());
        }
        if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) { error = message(status); return {}; }
    }
    // Rate conversion can retain a short filter tail after the codec reaches
    // EOF. Flush it without extending the original track/clip boundary.
    if (!complete) {
        const int capacity = swr_get_out_samples(raw, 0);
        if (capacity > 0) {
            QByteArray tail(qsizetype(capacity) * channels * sizeof(float), Qt::Uninitialized);
            auto* data = reinterpret_cast<uint8_t*>(tail.data());
            const int count = swr_convert(raw, &data, capacity, nullptr, 0);
            if (count < 0) { error = message(count); return {}; }
            const qint64 destination = av_rescale_q(nextUs - startUs, us, AVRational{1, sampleRate});
            const qint64 first = std::max<qint64>(0, -destination);
            const qint64 last = std::min<qint64>(count, validFrames - destination);
            if (last > first) memcpy(result.data() + (destination + first) * channels * sizeof(float),
                tail.constData() + first * channels * sizeof(float), size_t(last-first) * channels * sizeof(float));
        }
    }
    return result;
}
