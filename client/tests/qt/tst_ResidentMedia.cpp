#include "backend/media/MediaDecoder.h"
#include "backend/media/ResidentVideoPlayer.h"

#include <QAudioOutput>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVideoSink>
#include <QtTest>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

namespace {
// Generate a bounded, reproducible MP4 with delayed video frames and an AAC
// tail. Tests need neither the repository's large sample nor an ffmpeg CLI.
bool writeVideo(const QString& path, bool rotated = false, bool variableRate = false) {
    AVFormatContext* format = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = av_packet_alloc();
    bool ok = false;
    auto cleanup = qScopeGuard([&] {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        if (format && format->pb) avio_closep(&format->pb);
        avformat_free_context(format);
    });
    const QByteArray name = QFile::encodeName(path);
    if (avformat_alloc_output_context2(&format, nullptr, "mp4", name.constData()) < 0 || !format) return false;
    auto createStream = [&](AVCodecID id, bool isVideo, AVCodecContext*& codec) -> AVStream* {
        const AVCodec* encoder = avcodec_find_encoder(id);
        if (!encoder) return nullptr;
        codec = avcodec_alloc_context3(encoder);
        AVStream* stream = avformat_new_stream(format, nullptr);
        if (!codec || !stream) return nullptr;
        if (isVideo) {
            codec->width = 64;
            codec->height = 48;
            codec->pix_fmt = AV_PIX_FMT_YUV420P;
            codec->time_base = AVRational{1, 25};
            codec->framerate = AVRational{25, 1};
            codec->gop_size = 12;
            codec->max_b_frames = 2;
            codec->sample_aspect_ratio = rotated ? AVRational{2, 1} : AVRational{1, 1};
        } else {
            codec->sample_rate = 48000;
            codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
            codec->time_base = AVRational{1, 48000};
            av_channel_layout_default(&codec->ch_layout, 1);
        }
        codec->bit_rate = isVideo ? 100000 : 64000;
        if (format->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(codec, encoder, nullptr) < 0
            || avcodec_parameters_from_context(stream->codecpar, codec) < 0) return nullptr;
        stream->time_base = codec->time_base;
        stream->sample_aspect_ratio = codec->sample_aspect_ratio;
        if (isVideo && rotated) {
            AVPacketSideData* matrix = av_packet_side_data_new(&stream->codecpar->coded_side_data,
                &stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(int32_t), 0);
            if (!matrix) return nullptr;
            av_display_rotation_set(reinterpret_cast<int32_t*>(matrix->data), 90);
        }
        return stream;
    };
    AVStream* vs = createStream(AV_CODEC_ID_MPEG4, true, video);
    AVStream* as = createStream(AV_CODEC_ID_AAC, false, audio);
    if (!vs || !as || !packet || avio_open(&format->pb, name.constData(), AVIO_FLAG_WRITE) < 0
        || avformat_write_header(format, nullptr) < 0) return false;
    auto encode = [&](AVCodecContext* codec, AVStream* stream, AVFrame* input) {
        if (avcodec_send_frame(codec, input) < 0) return false;
        int result = 0;
        while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
            if (packet->duration <= 0) packet->duration = codec->codec_type == AVMEDIA_TYPE_VIDEO ? 1 : codec->frame_size;
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int written = av_interleaved_write_frame(format, packet);
            av_packet_unref(packet);
            if (written < 0) return false;
        }
        return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };
    frame = av_frame_alloc();
    frame->format = video->pix_fmt;
    frame->width = video->width;
    frame->height = video->height;
    if (av_frame_get_buffer(frame, 32) < 0) return false;
    for (int i = 0; i < 12; ++i) {
        if (av_frame_make_writable(frame) < 0) return false;
        for (int y = 0; y < frame->height; ++y)
            std::memset(frame->data[0] + y * frame->linesize[0], 32 + i * 15, frame->width);
        for (int p = 1; p <= 2; ++p)
            for (int y = 0; y < frame->height / 2; ++y)
                std::memset(frame->data[p] + y * frame->linesize[p], p == 1 ? 90 : 180, frame->width / 2);
        const int variablePts[12] = {0, 1, 3, 4, 7, 8, 9, 11, 12, 13, 14, 16};
        frame->pts = variableRate ? variablePts[i] : i;
        frame->duration = 1;
        if (!encode(video, vs, frame)) return false;
    }
    if (!encode(video, vs, nullptr)) return false;
    av_frame_free(&frame);
    frame = av_frame_alloc();
    frame->format = audio->sample_fmt;
    frame->sample_rate = audio->sample_rate;
    frame->nb_samples = audio->frame_size;
    av_channel_layout_copy(&frame->ch_layout, &audio->ch_layout);
    if (av_frame_get_buffer(frame, 0) < 0) return false;
    for (int i = 0; i < 23; ++i) {
        if (av_frame_make_writable(frame) < 0) return false;
        auto* samples = reinterpret_cast<float*>(frame->data[0]);
        for (int j = 0; j < frame->nb_samples; ++j)
            samples[j] = 0.1f * std::sin((i * frame->nb_samples + j) * 440.0 * 6.283185307179586 / 48000.0);
        frame->pts = i * frame->nb_samples;
        if (!encode(audio, as, frame)) return false;
    }
    ok = encode(audio, as, nullptr) && av_write_trailer(format) >= 0;
    return ok;
}

bool corruptLastVideoPacket(const QString& path, AVMediaType mediaType = AVMEDIA_TYPE_VIDEO) {
    AVFormatContext* format = nullptr;
    const QByteArray name = QFile::encodeName(path);
    if (avformat_open_input(&format, name.constData(), nullptr, nullptr) < 0) return false;
    auto cleanup = qScopeGuard([&] { avformat_close_input(&format); });
    if (avformat_find_stream_info(format, nullptr) < 0) return false;
    const int video = av_find_best_stream(format, mediaType, -1, -1, nullptr, 0);
    AVPacket* packet = av_packet_alloc();
    auto packetCleanup = qScopeGuard([&] { av_packet_free(&packet); });
    qint64 offset = -1;
    int length = 0;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video) { offset = packet->pos; length = packet->size; }
        av_packet_unref(packet);
    }
    avformat_close_input(&format);
    QFile file(path);
    return offset >= 0 && length > 0 && file.open(QIODevice::ReadWrite)
        && file.seek(offset) && file.write(QByteArray(length, '\0')) == length;
}
}

class ResidentMediaTest final : public QObject {
    Q_OBJECT
private slots:
    void imageDecodeAndReservations() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("image.png");
        QImage source(128, 64, QImage::Format_RGBA8888);
        source.fill(Qt::magenta);
        QVERIFY(source.save(path));
        const auto probe = MediaDecoder::probe(path);
        QVERIFY2(probe.accepted(), qPrintable(probe.error));
        QCOMPARE(probe.displaySize, source.size());
        QVERIFY(!probe.video);
        quint64 largestReservation = 0;
        double lastProgress = 0;
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.reserve = [&](quint64 bytes) { largestReservation = std::max(largestReservation, bytes); return true; };
        callbacks.progress = [&](double progress) { lastProgress = progress; };
        QString error;
        const auto asset = MediaDecoder::decode(path, callbacks, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->image, source);
        QCOMPARE(asset->sha256.size(), 64);
        QCOMPARE(asset->residentBytes, quint64(source.sizeInBytes()));
        QVERIFY(largestReservation > asset->residentBytes);
        QCOMPARE(lastProgress, 1.0);
        QVERIFY(QFile::remove(path));
        QCOMPARE(asset->image.pixelColor(5, 5), QColor(Qt::magenta));
    }

    void reservationRefusalAndCancellationNeverPublish() {
        QTemporaryDir directory;
        const QString path = directory.filePath("image.png");
        QImage source(16, 16, QImage::Format_RGBA8888);
        source.fill(Qt::red);
        QVERIFY(source.save(path));
        QString error;
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.reserve = [](quint64) { return false; };
        QVERIFY(!MediaDecoder::decode(path, callbacks, &error));
        QCOMPARE(error, QStringLiteral("memory_unavailable"));
        callbacks.reserve = {};
        callbacks.cancelled = [] { return true; };
        QVERIFY(!MediaDecoder::decode(path, callbacks, &error));
        QCOMPARE(error, QStringLiteral("cancelled"));
    }

    void decodeDelayedFramesAndAudioTail() {
        QTemporaryDir directory;
        const QString path = directory.filePath("video.mp4");
        QVERIFY(writeVideo(path));
        const auto probe = MediaDecoder::probe(path);
        QVERIFY2(probe.accepted(), qPrintable(probe.error));
        QVERIFY(probe.video);
        QCOMPARE(probe.displaySize, QSize(64, 48));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->frames.size(), size_t(12));
        QVERIFY(asset->durationUs >= 480000);
        QCOMPARE(asset->audioFormat.sampleFormat(), QAudioFormat::Float);
        QCOMPARE(asset->audioFormat.sampleRate(), 48000);
        QCOMPARE(asset->audioFormat.channelCount(), 1);
        qint64 samples = 0;
        for (const auto& chunk : asset->audio) {
            samples += chunk.sampleFrames;
            QCOMPARE(chunk.pcm.size(), chunk.sampleFrames * qint64(sizeof(float)));
        }
        QVERIFY(samples >= 23 * 1024);
        for (size_t i = 0; i < asset->frames.size(); ++i) {
            QCOMPARE(asset->frames[i].timestampUs, qint64(i) * 40000);
            QCOMPARE(asset->frames[i].frame.pixelFormat(), QVideoFrameFormat::Format_YUV420P);
        }
        QVideoFrame first = asset->frames.front().frame;
        QVERIFY(!first.map(QVideoFrame::WriteOnly));
        QVERIFY(first.map(QVideoFrame::ReadOnly));
        QCOMPARE(first.planeCount(), 3);
        QVERIFY(first.bits(0));
        first.unmap();
        QVERIFY(!first.toImage().isNull());
    }

    void presentationCachesDoNotAccumulateOnResidentFrames() {
        QTemporaryDir directory;
        const QString path = directory.filePath("cache.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        const QVideoFrame original = asset->frames.front().frame;
        auto first = ResidentVideoPlayer::presentationFrame(original);
        auto second = ResidentVideoPlayer::presentationFrame(original);
        QVERIFY(first != original);
        QVERIFY(second != original);
        QVERIFY(first != second);
        QVERIFY(first.map(QVideoFrame::ReadOnly));
        QVideoFrame source = original;
        QVERIFY(source.map(QVideoFrame::ReadOnly));
        QCOMPARE(first.bits(0), source.bits(0)); // zero-copy native planes
        first.unmap();
        source.unmap();
        const QImage converted = first.toImage();
        QVERIFY(!converted.isNull());
        QCOMPARE(first.toImage().cacheKey(), converted.cacheKey());
        QVERIFY(second.toImage().cacheKey() != converted.cacheKey());
        QVERIFY(original.toImage().cacheKey() != converted.cacheKey());
    }

    void decodingGrowthAndMidStreamCancellation() {
        QTemporaryDir directory;
        const QString path = directory.filePath("growth.mp4");
        QVERIFY(writeVideo(path));
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.reserve = [](quint64 bytes) { return bytes <= 64ULL * 1024 * 1024 + 16000; };
        QString error;
        QVERIFY(!MediaDecoder::decode(path, callbacks, &error));
        QCOMPARE(error, QStringLiteral("memory_unavailable"));
        bool cancelled = false;
        callbacks.reserve = {};
        callbacks.progress = [&](double progress) { if (progress > 0.3) cancelled = true; };
        callbacks.cancelled = [&] { return cancelled; };
        QVERIFY(!MediaDecoder::decode(path, callbacks, &error));
        QVERIFY(cancelled);
        QCOMPARE(error, QStringLiteral("cancelled"));
    }

    void corruptionInFinalPacketRejectsWholeAsset() {
        QTemporaryDir directory;
        const QString path = directory.filePath("corrupt.mp4");
        QVERIFY(writeVideo(path));
        QVERIFY(corruptLastVideoPacket(path));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY(!asset);
        QVERIFY(!error.isEmpty());
    }

    void corruptionInAudioTailRejectsWholeAsset() {
        QTemporaryDir directory;
        const QString path = directory.filePath("corrupt-audio.mp4");
        QVERIFY(writeVideo(path));
        QVERIFY(corruptLastVideoPacket(path, AVMEDIA_TYPE_AUDIO));
        QString error;
        QVERIFY(!MediaDecoder::decode(path, {}, &error));
        QVERIFY(!error.isEmpty());
    }

    void rotationAndSampleAspectRatio() {
        QTemporaryDir directory;
        const QString path = directory.filePath("rotated.mp4");
        QVERIFY(writeVideo(path, true));
        const auto probe = MediaDecoder::probe(path);
        QVERIFY2(probe.accepted(), qPrintable(probe.error));
        QCOMPARE(probe.displaySize, QSize(48, 128));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->displaySize, probe.displaySize);
        QCOMPARE(asset->frames.front().frame.size(), QSize(128, 48));
        QVERIFY(asset->frames.front().frame.rotation() != QtVideo::Rotation::None);
    }

    void playbackSurvivesSourceRemovalAndRetainsEvictedCursor() {
        QTemporaryDir directory;
        const QString path = directory.filePath("playback.mp4");
        QVERIFY(writeVideo(path));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QVERIFY(QFile::remove(path));
        ResidentVideoPlayer first;
        ResidentVideoPlayer second;
        QVideoSink sink;
        first.setVideoSink(&sink);
        first.setAsset(asset);
        second.setAsset(asset);
        QVERIFY(sink.videoFrame().isValid());
        first.setPosition(200);
        QCOMPARE(first.position(), qint64(200));
        QCOMPARE(second.position(), qint64(0));
        QCOMPARE(sink.videoFrame().startTime(), qint64(200000));
        first.play();
        QTRY_VERIFY_WITH_TIMEOUT(first.position() > 230, 500);
        first.pause();
        const qint64 retained = first.position();
        first.clearAsset();
        QVERIFY(!sink.videoFrame().isValid());
        QVERIFY(!first.isSeekable());
        QCOMPARE(first.position(), retained);
        first.setAsset(asset);
        QCOMPARE(first.position(), retained);
        first.setPosition(first.duration() - 30);
        first.play();
        QTRY_COMPARE_WITH_TIMEOUT(first.mediaStatus(), QMediaPlayer::EndOfMedia, 500);
        QCOMPARE(first.playbackState(), QMediaPlayer::StoppedState);
        first.setPosition(0);
        first.setLoops(2);
        first.play();
        QTest::qWait(int(first.duration() + 60));
        QCOMPARE(first.playbackState(), QMediaPlayer::PlayingState);
        QTRY_COMPARE_WITH_TIMEOUT(first.mediaStatus(), QMediaPlayer::EndOfMedia, 1000);
        first.stop();
        QCOMPARE(first.position(), qint64(0));
    }

    void variableFrameRateUsesPresentationTimestamps() {
        QTemporaryDir directory;
        const QString path = directory.filePath("variable.mp4");
        QVERIFY(writeVideo(path, false, true));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->frames.size(), size_t(12));
        QCOMPARE(asset->frames[2].timestampUs, qint64(120000));
        QCOMPARE(asset->frames[4].timestampUs, qint64(280000));
        QCOMPARE(asset->frames.back().timestampUs, qint64(640000));
        ResidentVideoPlayer player;
        QVideoSink sink;
        player.setVideoSink(&sink);
        player.setAsset(asset);
        player.setPosition(200);
        QCOMPARE(sink.videoFrame().startTime(), qint64(160000));
        player.setPosition(281);
        QCOMPARE(sink.videoFrame().startTime(), qint64(280000));
    }

    void audioPresentationAfterSourceRemoval() {
        QTemporaryDir directory;
        const QString path = directory.filePath("audio.mp4");
        QVERIFY(writeVideo(path));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QVERIFY(QFile::remove(path));
        ResidentVideoPlayer player;
        QAudioOutput audio;
        audio.setMuted(true);
        player.setAudioOutput(&audio);
        player.setAsset(asset);
        player.play();
        QTRY_VERIFY_WITH_TIMEOUT(player.position() > 50, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(player.mediaStatus(), QMediaPlayer::EndOfMedia, 2000);
        QCOMPARE(player.error(), QMediaPlayer::NoError);
    }

    void audioSeekAndClearReleaseTheResidentAsset() {
        QTemporaryDir directory;
        const QString path = directory.filePath("audio-seek.mp4");
        QVERIFY(writeVideo(path));
        auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        std::weak_ptr<const ResidentMediaAsset> retained = asset;
        ResidentVideoPlayer player;
        QAudioOutput audio;
        audio.setMuted(true);
        player.setAudioOutput(&audio);
        player.setAsset(asset);
        asset.reset();
        QVERIFY(QFile::remove(path));
        player.play();
        QTRY_VERIFY_WITH_TIMEOUT(player.position() > 50, 1500);
        player.setPosition(200);
        QCOMPARE(player.position(), qint64(200));
        QVERIFY(player.isPlaying());
        player.pause();
        const qint64 cursor = player.position();
        player.clearAsset();
        QVERIFY(retained.expired());
        QCOMPARE(player.position(), cursor);
        QTest::qWait(60);
        QCOMPARE(player.position(), cursor);
        QCOMPARE(player.mediaStatus(), QMediaPlayer::NoMedia);
    }

    void seekFromPositionCallbackDoesNotEndPlayback() {
        QTemporaryDir directory;
        const QString path = directory.filePath("marker.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        ResidentVideoPlayer player;
        player.setAsset(asset);
        int repeats = 0;
        connect(&player, &ResidentVideoPlayer::positionChanged, &player, [&](qint64 pos) {
            if (pos >= 150 && repeats < 2) { ++repeats; player.setPosition(50); }
        });
        player.play();
        QTRY_COMPARE_WITH_TIMEOUT(repeats, 2, 600);
        QVERIFY(player.isPlaying());
        player.stop();
    }
};

QTEST_MAIN(ResidentMediaTest)
#include "tst_ResidentMedia.moc"
