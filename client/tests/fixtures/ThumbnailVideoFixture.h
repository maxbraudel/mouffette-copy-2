#pragma once

#include <QFile>
#include <QSize>
#include <QString>
#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

// Four static frames with distinguishable horizontal bands. Y is limited-range
// 32 / 128 / 224, with neutral chroma; RGB conversion yields approximately
// 19 / 130 / 242. This makes vertical cropping/stretching visible for landscape,
// portrait and extreme-aspect-ratio thumbnail tests without external fixtures.
inline bool writeThumbnailVideoFixture(const QString& path, QSize size) {
    if (path.isEmpty() || size.isEmpty() || size.width() % 2 || size.height() % 2)
        return false;

    const QByteArray filename = QFile::encodeName(path);
    AVFormatContext* rawFormat = nullptr;
    if (avformat_alloc_output_context2(&rawFormat, nullptr, "mp4", filename.constData()) < 0
        || !rawFormat) return false;
    const auto freeFormat = [](AVFormatContext* value) {
        if (value->pb) avio_closep(&value->pb);
        avformat_free_context(value);
    };
    std::unique_ptr<AVFormatContext, decltype(freeFormat)> format(rawFormat, freeFormat);

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!encoder) return false;
    const auto freeCodec = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> codec(avcodec_alloc_context3(encoder), freeCodec);
    if (!codec) return false;
    AVStream* stream = avformat_new_stream(format.get(), nullptr);
    if (!stream) return false;

    codec->width = size.width();
    codec->height = size.height();
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = AVRational{1, 25};
    codec->framerate = AVRational{25, 1};
    codec->sample_aspect_ratio = AVRational{1, 1};
    codec->gop_size = 1;
    codec->max_b_frames = 0;
    codec->thread_count = 1;
    codec->flags |= AV_CODEC_FLAG_QSCALE;
    codec->global_quality = 2 * FF_QP2LAMBDA;
    codec->color_range = AVCOL_RANGE_MPEG;
    codec->colorspace = AVCOL_SPC_BT709;
    codec->color_primaries = AVCOL_PRI_BT709;
    codec->color_trc = AVCOL_TRC_BT709;
    if (format->oformat->flags & AVFMT_GLOBALHEADER)
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(codec.get(), encoder, nullptr) < 0
        || avcodec_parameters_from_context(stream->codecpar, codec.get()) < 0) return false;
    stream->time_base = codec->time_base;
    stream->avg_frame_rate = codec->framerate;
    stream->sample_aspect_ratio = codec->sample_aspect_ratio;

    const auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    const auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    if (!frame || !packet) return false;
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    frame->sample_aspect_ratio = codec->sample_aspect_ratio;
    frame->color_range = codec->color_range;
    frame->colorspace = codec->colorspace;
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;
    frame->quality = codec->global_quality;
    if (av_frame_get_buffer(frame.get(), 32) < 0) return false;
    if (avio_open(&format->pb, filename.constData(), AVIO_FLAG_WRITE) < 0
        || avformat_write_header(format.get(), nullptr) < 0) return false;

    const auto encode = [&](AVFrame* input) {
        if (avcodec_send_frame(codec.get(), input) < 0) return false;
        int result = 0;
        while ((result = avcodec_receive_packet(codec.get(), packet.get())) >= 0) {
            if (packet->duration <= 0) packet->duration = 1;
            av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int written = av_interleaved_write_frame(format.get(), packet.get());
            av_packet_unref(packet.get());
            if (written < 0) return false;
        }
        return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };

    const int bandHeight = size.height() / 6;
    for (int index = 0; index < 4; ++index) {
        if (av_frame_make_writable(frame.get()) < 0) return false;
        for (int y = 0; y < size.height(); ++y) {
            const int luma = y < bandHeight ? 32 : y >= size.height() - bandHeight ? 224 : 128;
            std::memset(frame->data[0] + y * frame->linesize[0], luma, size.width());
        }
        for (int plane = 1; plane <= 2; ++plane)
            for (int y = 0; y < size.height() / 2; ++y)
                std::memset(frame->data[plane] + y * frame->linesize[plane], 128, size.width() / 2);
        frame->pts = index;
        frame->duration = 1;
        if (!encode(frame.get())) return false;
    }
    return encode(nullptr) && av_write_trailer(format.get()) >= 0;
}
