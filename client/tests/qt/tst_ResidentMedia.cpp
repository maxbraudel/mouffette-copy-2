#include "backend/media/MediaDecoder.h"
#include "backend/media/ResidentVideoPlayer.h"

#include <QAudioOutput>
#include <QElapsedTimer>
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
bool writeVideo(const QString& path, bool rotated = false, bool variableRate = false,
                int videoDelayFrames = 0, int audioFrames = 23) {
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
        frame->pts = (variableRate ? variablePts[i] : i) + videoDelayFrames;
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
    for (int i = 0; i < audioFrames; ++i) {
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
        const auto geometry = MediaDecoder::inspectGeometry(path);
        QVERIFY2(geometry.accepted(), qPrintable(geometry.error));
        QCOMPARE(geometry.displaySize, source.size());
        QVERIFY(!geometry.video);
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

    void importGeometryPreservesExifOrientation() {
        QTemporaryDir directory;
        const QString path = directory.filePath("rotated.jpg");
        QImage source(80, 40, QImage::Format_RGB32);
        source.fill(Qt::cyan);
        QVERIFY(source.save(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QByteArray bytes = file.readAll();
        QVERIFY(bytes.startsWith(QByteArray::fromHex("ffd8")));
        // JPEG APP1 containing a little-endian EXIF Orientation=6 (90 degrees).
        // Encoded pixels remain 80x40; displayed dimensions must be 40x80.
        bytes.insert(2, QByteArray::fromHex(
            "ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
        QVERIFY(file.seek(0));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        const auto geometry = MediaDecoder::inspectGeometry(path);
        QVERIFY2(geometry.accepted(), qPrintable(geometry.error));
        QVERIFY(!geometry.video);
        QCOMPARE(geometry.displaySize, QSize(40, 80));
        const auto probe = MediaDecoder::probe(path);
        QVERIFY2(probe.accepted(), qPrintable(probe.error));
        QCOMPARE(geometry.displaySize, probe.displaySize);
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->displaySize, geometry.displaySize);
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
        const auto geometry = MediaDecoder::inspectGeometry(path, callbacks.cancelled);
        QVERIFY(!geometry.accepted());
        QCOMPARE(geometry.error, QStringLiteral("cancelled"));
    }

    void decodeDelayedFramesAndAudioTail() {
        QTemporaryDir directory;
        const QString path = directory.filePath("video.mp4");
        QVERIFY(writeVideo(path));
        const auto geometry = MediaDecoder::inspectGeometry(path);
        QVERIFY2(geometry.accepted(), qPrintable(geometry.error));
        QVERIFY(geometry.video);
        QCOMPARE(geometry.displaySize, QSize(64, 48));
        const auto probe = MediaDecoder::probe(path);
        QVERIFY2(probe.accepted(), qPrintable(probe.error));
        QVERIFY(probe.video);
        QCOMPARE(probe.displaySize, QSize(64, 48));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->videoFrameCount, quint64(12));
        QVERIFY(asset->durationUs >= 480000);
        QCOMPARE(asset->audioFormat.sampleFormat(), QAudioFormat::Float);
        QCOMPARE(asset->audioFormat.sampleRate(), 48000);
        QCOMPARE(asset->audioFormat.channelCount(), 1);
        QVERIFY(asset->audioSampleCount >= 23 * 1024);
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(asset->compressedVideo, original.readAll());
        QVERIFY(asset->residentBytes < quint64(asset->compressedVideo.size()) + 8192);
        QVideoFrame first = asset->firstFrame.frame;
        QCOMPARE(first.pixelFormat(), QVideoFrameFormat::Format_YUV420P);
        QVERIFY(!first.map(QVideoFrame::WriteOnly));
        QVERIFY(first.map(QVideoFrame::ReadOnly));
        QCOMPARE(first.planeCount(), 3);
        QVERIFY(first.bits(0));
        first.unmap();
        QVERIFY(!first.toImage().isNull());
    }

    void original1080VideoHasBoundedPreparationMemory() {
        const QString path = QString::fromUtf8(TEST_SAMPLE_VIDEO_FILE);
        if (!QFile::exists(path)) QSKIP("Optional repository sample is not installed");
        quint64 peakBudget = 0, retained = 0;
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.reserve = [&](quint64 bytes) { peakBudget = std::max(peakBudget, bytes); return true; };
        callbacks.allocated = [&](quint64 bytes) { retained = bytes; };
        QString error;
        const auto asset = MediaDecoder::decode(path, callbacks, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->videoFrameCount, quint64(921));
        QCOMPARE(asset->compressedVideo.size(), QFileInfo(path).size());
        QVERIFY(asset->residentBytes < quint64(QFileInfo(path).size()) + 4 * 1024 * 1024);
        QCOMPARE(retained, asset->residentBytes);
        QVERIFY(peakBudget < quint64(QFileInfo(path).size()) + 128 * 1024 * 1024);
        QVERIFY(peakBudget > retained);
        const auto probe = MediaDecoder::probe(path);
        QVERIFY(probe.estimatedBytes < 20 * 1024 * 1024);
        qInfo() << "1080p retained bytes:" << retained << "preparation budget:" << peakBudget;
        // Exact original bitstream, shared between independent occurrences.
        QVideoSink sink;
        ResidentVideoPlayer one, two;
        one.setVideoSink(&sink);
        QElapsedTimer preparationTime;
        preparationTime.start();
        one.setAsset(asset); two.setAsset(asset);
        QCOMPARE(one.asset()->compressedVideo.constData(), two.asset()->compressedVideo.constData());
        QCOMPARE(one.asset()->compressedVideo.constData(), asset->compressedVideo.constData());
        QTRY_VERIFY2_WITH_TIMEOUT(one.preparedAt(0), qPrintable(one.errorString()), 5000);
        QTRY_VERIFY2_WITH_TIMEOUT(two.preparedAt(0), qPrintable(two.errorString()), 5000);
        qInfo() << "1080p automatic preparation ms:" << preparationTime.elapsed();
        QCOMPARE(one.playbackState(), QMediaPlayer::StoppedState);
        QCOMPARE(two.playbackState(), QMediaPlayer::StoppedState);

        // The sample has a keyframe at 23.466 s. A 24.567 s seek should
        // decode from there, not restart the preceding GOP at 15.133 s.
        // Observe the delivered frame interval: position() changes optimistically
        // before decoding and cannot demonstrate that a seek has completed.
        constexpr qint64 targetMs = 24567;
        constexpr qint64 targetUs = targetMs * 1000;
        for (const bool playing : {false, true}) {
            one.pause();
            one.setPosition(0);
            QTRY_VERIFY2_WITH_TIMEOUT(one.preparedAt(0), qPrintable(one.errorString()), 5000);
            if (playing) {
                one.play();
                QTRY_VERIFY2_WITH_TIMEOUT(one.position() > 100, qPrintable(one.errorString()), 5000);
            }
            qint64 deliveredAfterMs = -1;
            qint64 deliveredStartUs = -1;
            qint64 deliveredEndUs = -1;
            QElapsedTimer seekTime;
            QObject seekObserver;
            connect(&sink, &QVideoSink::videoFrameChanged, &seekObserver,
                    [&](const QVideoFrame& frame) {
                if (deliveredAfterMs >= 0 || !frame.isValid() || frame.startTime() < 0
                    || frame.startTime() > targetUs || frame.endTime() <= targetUs) return;
                deliveredAfterMs = seekTime.elapsed();
                deliveredStartUs = frame.startTime();
                deliveredEndUs = frame.endTime();
            });
            seekTime.start();
            one.setPosition(targetMs);
            QTRY_VERIFY2_WITH_TIMEOUT(deliveredAfterMs >= 0, qPrintable(one.errorString()), 5000);
            qInfo() << "1080p" << (playing ? "playing" : "paused")
                    << "seek ms:" << deliveredAfterMs
                    << "delivered frame us:" << deliveredStartUs << deliveredEndUs;
            QVERIFY2(deliveredAfterMs < 500,
                     qPrintable(QStringLiteral("%1 seek delivered the requested frame after %2 ms")
                         .arg(playing ? QStringLiteral("Playing") : QStringLiteral("Paused"))
                         .arg(deliveredAfterMs)));
            QCOMPARE(one.isPlaying(), playing);
            one.pause();
        }
    }

    void presentationCachesDoNotAccumulateOnResidentFrames() {
        QTemporaryDir directory;
        const QString path = directory.filePath("cache.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        const QVideoFrame original = asset->firstFrame.frame;
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
        callbacks.reserve = [](quint64 bytes) { return bytes <= 16ULL * 1024 * 1024; };
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
        const auto geometry = MediaDecoder::inspectGeometry(path);
        QVERIFY2(geometry.accepted(), qPrintable(geometry.error));
        QCOMPARE(geometry.displaySize, QSize(64, 48));
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
        const auto geometry = MediaDecoder::inspectGeometry(path);
        QVERIFY2(geometry.accepted(), qPrintable(geometry.error));
        QVERIFY(geometry.video);
        QCOMPARE(geometry.displaySize, QSize(48, 128));
        const auto probe = MediaDecoder::probe(path);
        QVERIFY2(probe.accepted(), qPrintable(probe.error));
        QCOMPARE(probe.displaySize, QSize(48, 128));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->displaySize, probe.displaySize);
        QCOMPARE(asset->firstFrame.frame.size(), QSize(128, 48));
        QVERIFY(asset->firstFrame.frame.rotation() != QtVideo::Rotation::None);
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
        QTRY_VERIFY_WITH_TIMEOUT(first.preparedAt(0), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(second.preparedAt(0), 3000);
        QCOMPARE(first.playbackState(), QMediaPlayer::StoppedState);
        QCOMPARE(second.playbackState(), QMediaPlayer::StoppedState);
        first.setPosition(200);
        QCOMPARE(first.position(), qint64(200));
        QCOMPARE(second.position(), qint64(0));
        QTRY_COMPARE_WITH_TIMEOUT(sink.videoFrame().startTime(), qint64(200000), 3000);
        first.play();
        QTRY_VERIFY_WITH_TIMEOUT(first.position() > 230, 2000);
        first.pause();
        const qint64 retained = first.position();
        first.clearAsset();
        QVERIFY(!sink.videoFrame().isValid());
        QVERIFY(!first.isSeekable());
        QCOMPARE(first.position(), retained);
        first.setAsset(asset);
        QCOMPARE(first.position(), retained);
        QTRY_VERIFY_WITH_TIMEOUT(first.preparedAt(retained), 3000);
        QVERIFY(!first.isPlaying());
        QCOMPARE(first.position(), retained);
        first.setPosition(first.duration() - 30);
        first.play();
        QTRY_COMPARE_WITH_TIMEOUT(first.mediaStatus(), QMediaPlayer::EndOfMedia, 2000);
        QCOMPARE(first.playbackState(), QMediaPlayer::StoppedState);
        first.setPosition(0);
        first.setLoops(2);
        first.play();
        QTest::qWait(int(first.duration() + 60));
        QCOMPARE(first.playbackState(), QMediaPlayer::PlayingState);
        QTRY_COMPARE_WITH_TIMEOUT(first.mediaStatus(), QMediaPlayer::EndOfMedia, 1000);
        first.stop();
        QCOMPARE(first.position(), qint64(0));
        first.setPosition(200);
        QTRY_VERIFY_WITH_TIMEOUT(first.preparedAt(200), 3000);
        QCOMPARE(sink.videoFrame().startTime(), qint64(200000));
        QCOMPARE(first.playbackState(), QMediaPlayer::StoppedState);
    }

    void automaticPreparationDefersWhenPlaybackBudgetIsUnavailable() {
        QTemporaryDir directory;
        const QString path = directory.filePath("budget.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        bool admitted = false;
        int attempts = 0;
        int releases = 0;
        asset->reservePlayback = [&] { ++attempts; return admitted; };
        asset->releasePlayback = [&](bool) { ++releases; };
        QVideoSink sink;
        ResidentVideoPlayer player;
        player.setVideoSink(&sink);
        QSignalSpy errors(&player, &ResidentVideoPlayer::errorOccurred);
        player.setAsset(asset);
        QCOMPARE(attempts, 1);
        QCOMPARE(releases, 0);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(player.error(), QMediaPlayer::NoError);
        QCOMPARE(player.mediaStatus(), QMediaPlayer::LoadedMedia);
        QCOMPARE(player.playbackState(), QMediaPlayer::StoppedState);
        QVERIFY(sink.videoFrame().isValid());
        QVERIFY(!player.preparedAt(0));

        // An explicit request can retry the same resident asset once capacity
        // becomes available; refusal must not poison its cached poster/state.
        admitted = true;
        player.play();
        QTRY_VERIFY2_WITH_TIMEOUT(player.position() > 50, qPrintable(player.errorString()), 3000);
        QCOMPARE(attempts, 2);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(player.error(), QMediaPlayer::NoError);
        player.clearAsset();
        QCOMPARE(releases, 1);
    }

    void playbackReservationsFollowNativePreparation() {
        QTemporaryDir directory;
        const QString path = directory.filePath("reservations.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        int reservations = 0;
        int preparations = 0;
        QList<bool> releasedPrepared;
        asset->reservePlayback = [&] { ++reservations; return true; };
        asset->playbackPrepared = [&] { ++preparations; };
        asset->releasePlayback = [&](bool prepared) { releasedPrepared.append(prepared); };
        ResidentVideoPlayer player;

        player.setAsset(asset);
        QCOMPARE(reservations, 1);
        // A cached poster does not consume the future native decoder budget.
        QCOMPARE(preparations, 0);
        player.clearAsset();
        QCOMPARE(releasedPrepared.size(), 1);
        QVERIFY(!releasedPrepared.last());

        player.setAsset(asset);
        QTRY_VERIFY2_WITH_TIMEOUT(player.preparedAt(0), qPrintable(player.errorString()), 3000);
        QCOMPARE(reservations, 2);
        QCOMPARE(preparations, 1);
        player.prepare(200);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(200), 3000);
        QCOMPARE(preparations, 1); // seek frames cannot retire the budget twice
        player.clearAsset();
        QCOMPARE(releasedPrepared.size(), 2);
        QVERIFY(releasedPrepared.last());

        // Residency observers can retire a player synchronously as its first
        // frame transfers the budget into the measured process allocation.
        asset->playbackPrepared = [&] { ++preparations; player.clearAsset(); };
        player.setAsset(asset);
        QTRY_COMPARE_WITH_TIMEOUT(preparations, 2, 3000);
        QCOMPARE(reservations, 3);
        QVERIFY(!player.asset());
        QCOMPARE(releasedPrepared.size(), 3);
        QVERIFY(releasedPrepared.last());

        asset->reservePlayback = [&] { ++reservations; player.clearAsset(); return true; };
        player.setAsset(asset);
        QCOMPARE(reservations, 4);
        QCOMPARE(preparations, 2);
        QVERIFY(!player.asset());
        QCOMPARE(releasedPrepared.size(), 4);
        QVERIFY(!releasedPrepared.last());
    }

    void failedNativePreparationReleasesPendingReservation() {
        QTemporaryDir directory;
        const QString path = directory.filePath("failed-preparation.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        // Retain a valid cached poster, but force native source initialization
        // to fail before a decoded frame can consume its pending reservation.
        asset->compressedVideo = QByteArrayLiteral("invalid MP4 stream");
        int reservations = 0;
        int preparations = 0;
        QList<bool> releasedPrepared;
        asset->reservePlayback = [&] { ++reservations; return true; };
        asset->playbackPrepared = [&] { ++preparations; };
        asset->releasePlayback = [&](bool prepared) { releasedPrepared.append(prepared); };
        QVideoSink sink;
        ResidentVideoPlayer player;
        player.setVideoSink(&sink);
        QSignalSpy errors(&player, &ResidentVideoPlayer::errorOccurred);
        player.setAsset(asset);
        QTRY_COMPARE_WITH_TIMEOUT(releasedPrepared.size(), 1, 3000);
        QCOMPARE(reservations, 1);
        QCOMPARE(preparations, 0);
        QVERIFY(!releasedPrepared.last());
        QVERIFY(!errors.isEmpty());
        QVERIFY(player.error() != QMediaPlayer::NoError);
        QVERIFY(!player.errorString().isEmpty());
        QCOMPARE(player.mediaStatus(), QMediaPlayer::InvalidMedia);
        QCOMPARE(player.asset(), asset);
        QVERIFY(sink.videoFrame().isValid());
        player.clearAsset();
        QCOMPARE(releasedPrepared.size(), 1); // teardown cannot release twice
    }

    void delayedVideoAndAudioTailPrepareWithoutMovingTheClock_data() {
        QTest::addColumn<int>("delayFrames");
        QTest::newRow("one-frame-late") << 1;
        QTest::newRow("one-second-late") << 25;
        QTest::newRow("later-than-preparation-deadline") << 250;
    }
    void delayedVideoAndAudioTailPrepareWithoutMovingTheClock() {
        QFETCH(int, delayFrames);
        QTemporaryDir directory;
        const QString path = directory.filePath("delayed.mp4");
        QVERIFY(writeVideo(path, false, false, delayFrames, 80));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        const qint64 firstUs = delayFrames * 40000LL;
        QCOMPARE(asset->firstFrame.timestampUs, firstUs);
        ResidentVideoPlayer player;
        QAudioOutput audio;
        audio.setMuted(true);
        player.setAudioOutput(&audio);
        player.setAsset(asset);
        QVERIFY(!player.preparedAt(0)); // the poster is not native readiness
        const qint64 lastUs = firstUs + 440000;
        for (qint64 target : {qint64(0), firstUs / 2000, player.duration() - 1,
                             firstUs / 1000 + 201, qint64(0)}) {
            player.prepare(target);
            QTRY_VERIFY2_WITH_TIMEOUT(player.preparedAt(target), qPrintable(player.errorString()), 3000);
            QCOMPARE(player.position(), target);
            QVERIFY(!player.isPlaying());
            const auto frame = player.preparedFrame(target);
            QVERIFY(!frame.toImage().isNull());
            if (target * 1000 < firstUs) QCOMPARE(frame.startTime(), firstUs);
            if (target == player.duration() - 1) QCOMPARE(frame.startTime(), lastUs);
        }
        player.play();
        QTRY_VERIFY_WITH_TIMEOUT(player.position() > 50, 1500);
        player.pause();
        const qint64 stoppedAt = player.position();
        player.clearAsset();
        player.setAsset(asset);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(stoppedAt), 3000);
        QCOMPARE(player.position(), stoppedAt);
        player.stop();
        player.prepare(0);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(0), 3000);
        QCOMPARE(player.position(), qint64(0));
    }

    void preparationTimeoutReleasesDecoderAndAllowsRecovery() {
        QTemporaryDir directory;
        const QString path = directory.filePath("unreachable.mp4");
        QVERIFY(writeVideo(path, false, false, 25, 80));
        const auto valid = MediaDecoder::decode(path);
        QVERIFY(valid);
        auto inconsistent = std::make_shared<ResidentMediaAsset>(*valid);
        // A cached poster claims a frame at zero, but native decoding proves
        // there is none there. This must time out without poisoning a retry.
        inconsistent->firstFrame.timestampUs = 0;
        int released = 0;
        inconsistent->releasePlayback = [&](bool) { ++released; };
        ResidentVideoPlayer player;
        QSignalSpy errors(&player, &ResidentVideoPlayer::errorOccurred);
        player.setAsset(inconsistent);
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 6500);
        QTRY_COMPARE(released, 1);
        QVERIFY(player.errorString().contains("0 ms"));
        QVERIFY(!player.preparedAt(0));
        player.setAsset(valid);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(0), 3000);
        QCOMPARE(player.error(), QMediaPlayer::NoError);
        QCOMPARE(errors.size(), 1);
    }

    void variableFrameRateUsesPresentationTimestamps() {
        QTemporaryDir directory;
        const QString path = directory.filePath("variable.mp4");
        QVERIFY(writeVideo(path, false, true));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->videoFrameCount, quint64(12));
        ResidentVideoPlayer player;
        QVideoSink sink;
        player.setVideoSink(&sink);
        player.setAsset(asset);
        player.setPosition(200);
        QTRY_COMPARE_WITH_TIMEOUT(sink.videoFrame().startTime(), qint64(160000), 3000);
        player.setPosition(281);
        QTRY_COMPARE_WITH_TIMEOUT(sink.videoFrame().startTime(), qint64(280000), 3000);
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
        QTRY_VERIFY2_WITH_TIMEOUT(player.position() > 50, qPrintable(player.errorString()), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(player.mediaStatus(), QMediaPlayer::EndOfMedia, 5000);
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
        QTRY_VERIFY2_WITH_TIMEOUT(player.position() > 50, qPrintable(player.errorString()), 3000);
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
        QTRY_COMPARE_WITH_TIMEOUT(repeats, 2, 2000);
        QVERIFY(player.isPlaying());
        player.stop();
    }
};

QTEST_MAIN(ResidentMediaTest)
#include "tst_ResidentMedia.moc"
