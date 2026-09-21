#include "backend/media/MediaDecoder.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "backend/media/DecodeScheduler.h"
#include "backend/media/MediaPreviewStore.h"
#include "backend/media/ResidentVideoPlayer.h"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVideoSink>
#include <QtTest>
#include <algorithm>
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
                int videoDelayFrames = 0, int audioFrames = 23, int audioTrimSamples = 0, int frameCount = 12) {
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
    for (int i = 0; i < frameCount; ++i) {
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
        frame->pts = i * frame->nb_samples - audioTrimSamples;
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
    void interactiveImportUsesValidatedOriginal_data() {
        QTest::addColumn<bool>("vfr");
        QTest::addColumn<bool>("rotated");
        QTest::newRow("b-frames") << false << false;
        QTest::newRow("vfr-rotation-sar") << true << true;
    }
    void interactiveImportUsesValidatedOriginal() {
        QFETCH(bool, vfr);
        QFETCH(bool, rotated);
        QTemporaryDir directory;
        const auto path = directory.filePath("original.mp4");
        QVERIFY(writeVideo(path, vfr, rotated, 2));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll();
        file.close();
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.retainOriginalVideo = true;
        double lastProgress = 0;
        callbacks.progress = [&](double progress) { QVERIFY(progress >= lastProgress); lastProgress = progress; };
        QString error;
        const auto asset = MediaDecoder::decode(path, callbacks, &error);
        QVERIFY2(asset, qPrintable(error));
        QVERIFY(!asset->allIntra);
        QCOMPARE(asset->compressedVideo, original);
        QVERIFY(asset->videoPackets.bytes.isEmpty());
        QCOMPARE(asset->frameIndex.size(), qsizetype(asset->videoFrameCount));
        QCOMPARE(asset->memoryBreakdown().totalBytes(), asset->residentBytes);
        QCOMPARE(asset->audioPackets.bytes.constData(), asset->compressedVideo.constData());
        QCOMPARE(lastProgress, 1.0);
        QVERIFY(QFile::remove(path)); // All random access must now use RAM only.
        IndexedMediaDecoder decoder;
        for (int index : {0, int(asset->frameIndex.size()) - 1, 2, 1, 0}) {
            const auto frame = decoder.videoFrame(*asset, index, error);
            QVERIFY2(frame.isValid(), qPrintable(error));
            QCOMPARE(frame.startTime(), asset->frameIndex[index].timestampUs);
            QCOMPARE(frame.endTime(), asset->frameIndex[index].timestampUs + asset->frameIndex[index].durationUs);
            QCOMPARE(frame.size(), asset->firstFrame.frame.size());
            QCOMPARE(frame.rotation(), asset->firstFrame.frame.rotation());
        }
        ResidentVideoPlayer player;
        player.setAsset(asset);
        for (const qint64 position : {qint64(0), qint64(200), asset->durationUs / 1000 - 1, qint64(0)}) {
            player.prepare(position);
            QTRY_VERIFY2_WITH_TIMEOUT(player.preparedAt(position), qPrintable(player.errorString()), 5000);
        }
    }
    void interactiveImportStillRejectsCorruptionAndCancellation() {
        QTemporaryDir directory;
        const auto path = directory.filePath("original.mp4");
        QVERIFY(writeVideo(path));
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.retainOriginalVideo = true;
        bool cancelled = false;
        callbacks.cancelled = [&] { return cancelled; };
        callbacks.progress = [&](double progress) { if (progress > 0.3) cancelled = true; };
        QString error;
        QVERIFY(!MediaDecoder::decode(path, callbacks, &error));
        QCOMPARE(error, QStringLiteral("cancelled"));
        callbacks.cancelled = {};
        callbacks.progress = {};
        QVERIFY(corruptLastVideoPacket(path));
        QVERIFY(!MediaDecoder::decode(path, callbacks, &error));
        QVERIFY(!error.isEmpty());
    }
    void failedThumbnailCompletesRequest() {
        QTemporaryDir directory;
        const auto path = directory.filePath("thumbnail-failure.mp4");
        QVERIFY(writeVideo(path));
        auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        asset->sha256 += QStringLiteral("-damaged-thumbnail");
        asset->videoPackets.packets.last().offset = asset->videoPackets.bytes.size() + 1;
        int completions = 0;
        DecodeScheduler::instance().requestThumbnail(this, asset, asset->frameIndex.last().timestampUs,
            [&](auto image) { QVERIFY(!image); ++completions; });
        QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 3000);
    }
    void cancelledSeekReplacementKeepsItsCompletion() {
        QTemporaryDir directory;
        const auto path = directory.filePath("replacement.mp4");
        QVERIFY(writeVideo(path));
        auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        auto& scheduler = DecodeScheduler::instance();
        QTRY_COMPARE(scheduler.pendingJobs(), 0);
        scheduler.evictOptionalCaches();
        QObject cursor;
        int cancelledCompletions = 0, replacements = 0;
        const auto target = asset->frameIndex[8].timestampUs;
        scheduler.request(&cursor, 1, asset, target, DecodeScheduler::Scrub, 0,
            [&](auto, const QString&) { ++cancelledCompletions; });
        scheduler.cancel(&cursor, 1);
        // This has the identical key while the cancelled worker can still be
        // running. Its completion must not erase the replacement from the map.
        scheduler.request(&cursor, 2, asset, target, DecodeScheduler::Scrub, 0,
            [&](auto frame, const QString& error) {
                QVERIFY2(frame && frame->frame.isValid(), qPrintable(error));
                QCOMPARE(frame->frame.startTime(), target);
                ++replacements;
            });
        QTRY_COMPARE_WITH_TIMEOUT(replacements, 1, 3000);
        QTRY_COMPARE(scheduler.pendingJobs(), 0);
        QCOMPARE(cancelledCompletions, 0);
    }
    void thumbnailViewportRetainsOnlyUsefulSubscriptions() {
        QTemporaryDir directory;
        const auto path = directory.filePath("viewport.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        auto& scheduler = DecodeScheduler::instance();
        QTRY_COMPARE(scheduler.pendingJobs(), 0);
        scheduler.evictOptionalCaches();
        QObject strip, secondStrip;
        int obsolete = 0, useful = 0, shared = 0;
        for (int index = 1; index < 10; ++index)
            scheduler.requestThumbnail(&strip, asset, asset->frameIndex[index].timestampUs,
                [&](auto) { ++obsolete; }, false);
        QVERIFY(scheduler.runningThumbnailJobs() <= std::max(1, scheduler.workerCount() - 1));
        scheduler.retainThumbnailRequests(&strip, *asset, QSet<int>{6});
        // Promote the retained neighbour and replace its callback exactly once.
        scheduler.requestThumbnail(&strip, asset, asset->frameIndex[6].timestampUs,
            [&](auto image) { QVERIFY(image && !image->isNull()); ++useful; }, true);
        scheduler.requestThumbnail(&secondStrip, asset, asset->frameIndex[6].timestampUs,
            [&](auto image) { QVERIFY(image && !image->isNull()); ++shared; }, true);
        QTRY_COMPARE_WITH_TIMEOUT(useful, 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(shared, 1, 3000);
        QTRY_COMPARE(scheduler.pendingJobs(), 0);
        QCOMPARE(obsolete, 0);
    }
    void thumbnailReusesPreparedPosterWithoutDecodingItsPacket() {
        QTemporaryDir directory;
        const auto path = directory.filePath("poster.mp4");
        QVERIFY(writeVideo(path));
        auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        asset->sha256 += QStringLiteral("-poster-reuse");
        asset->videoPackets.packets.first().offset = asset->videoPackets.bytes.size() + 1;
        QObject strip;
        QImage result;
        DecodeScheduler::instance().requestThumbnail(&strip, asset, 0,
            [&](auto image) { if (image) result = *image; });
        QTRY_VERIFY_WITH_TIMEOUT(!result.isNull(), 3000);
        QVERIFY(result.width() <= MediaThumbnails::Width);
        QVERIFY(result.height() <= MediaThumbnails::Height);
    }
    void cancelledOriginalDecodeCanBeReused() {
        QTemporaryDir directory;
        const auto path = directory.filePath("cancel-original.mp4");
        QVERIFY(writeVideo(path, false, false, 0, 60, 0, 60));
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.retainOriginalVideo = true;
        QString error;
        const auto asset = MediaDecoder::decode(path, callbacks, &error);
        QVERIFY2(asset, qPrintable(error));
        IndexedMediaDecoder decoder;
        int cancellationChecks = 0;
        const auto cancelled = decoder.videoFrame(*asset, 59, error,
            [&] { return ++cancellationChecks > 2; });
        QVERIFY(!cancelled.isValid());
        QVERIFY(cancellationChecks > 2);
        error.clear();
        const auto recovered = decoder.videoFrame(*asset, 3, error);
        QVERIFY2(recovered.isValid(), qPrintable(error));
        QCOMPARE(recovered.startTime(), asset->frameIndex[3].timestampUs);
    }
    void visibleThumbnailAllocationSurvivesOptionalCacheEviction() {
        QTemporaryDir directory;
        const auto path = directory.filePath("thumbnail-lifetime.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        auto& scheduler = DecodeScheduler::instance();
        QTRY_COMPARE(scheduler.pendingJobs(), 0);
        scheduler.evictOptionalCaches();
        const quint64 baseline = scheduler.trackedThumbnailBytes();
        QObject strip;
        std::shared_ptr<const QImage> visible;
        scheduler.requestThumbnail(&strip, asset, asset->frameIndex[4].timestampUs,
            [&](auto image) { visible = std::move(image); });
        QTRY_VERIFY(visible && !visible->isNull());
        QTRY_COMPARE(scheduler.pendingJobs(), 0);
        QCOMPARE(scheduler.trackedThumbnailBytes(), baseline + quint64(visible->sizeInBytes()));
        scheduler.evictOptionalCaches();
        QCOMPARE(scheduler.thumbnailBytes(), quint64(0));
        QCOMPARE(scheduler.trackedThumbnailBytes(), baseline + quint64(visible->sizeInBytes()));
        visible.reset();
        QTRY_COMPARE(scheduler.trackedThumbnailBytes(), baseline);
    }
    void coldScrubProgressRejectsOldDirectionsWithoutSourceDistanceStarvation() {
        ResidentVideoPlayer player;
        player.m_scrubbing = true;
        player.m_positionMs = 60000;
        player.m_scrubDirection = 1;
        player.m_scrubDirectionEpoch = 7;
        player.m_scrubPresentationEpoch = 7;
        player.m_lastScrubPresentationMs = 0;
        auto displayed = std::make_shared<SharedMediaFrame>();
        displayed->frame = QVideoFrame(QVideoFrameFormat(QSize(2, 2), QVideoFrameFormat::Format_RGBA8888));
        displayed->frame.setStartTime(5000000);
        player.m_cursor.frame = displayed;

        // A 625 ms cold decode while the pointer runs at tens of source seconds
        // per real second still makes useful progress. The previous one-second
        // source-distance / 350 ms age filters rejected every such result.
        QVERIFY(player.acceptsIntermediateScrubFrame(10000, 0, 7, 625));
        QVERIFY(!player.acceptsIntermediateScrubFrame(4000, 0, 7, 625));
        QVERIFY(!player.acceptsIntermediateScrubFrame(10000, 0, 7, 1001));
        QVERIFY(!player.acceptsIntermediateScrubFrame(10000, 0, 6, 625));
        player.m_lastScrubPresentationMs = 600;
        QVERIFY(!player.acceptsIntermediateScrubFrame(10000, 0, 7, 625));

        // On reversal, reject the previous trajectory, but do not compare the
        // first useful backwards result to an image that lagged on the forwards
        // trajectory: that was another source of permanent cold-drag freezing.
        player.m_lastScrubPresentationMs = 0;
        player.m_positionMs = 20000;
        player.m_scrubDirection = -1;
        player.m_scrubDirectionEpoch = 8;
        QVERIFY(!player.acceptsIntermediateScrubFrame(30000, 0, 7, 625));
        QVERIFY(player.acceptsIntermediateScrubFrame(30000, 0, 8, 625));
        player.m_scrubPresentationEpoch = 8;
        displayed->frame.setStartTime(35000000);
        QVERIFY(player.acceptsIntermediateScrubFrame(30000, 0, 8, 625));
        QVERIFY(!player.acceptsIntermediateScrubFrame(36000, 0, 8, 625));
    }
    void scrubBatchesTargetsAndRefinesOnRelease() {
        QTemporaryDir directory;
        const auto path = directory.filePath("scrub-targets.mp4");
        QVERIFY(writeVideo(path, false, false, 0, 60, 0, 40));
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.retainOriginalVideo = true;
        const auto asset = MediaDecoder::decode(path, callbacks, nullptr);
        QVERIFY(asset);
        ResidentVideoPlayer player;
        QVideoSink sink;
        QAudioOutput audio; audio.setMuted(true);
        player.setVideoSink(&sink); player.setAudioOutput(&audio); player.setAsset(asset);
        QTRY_VERIFY(player.preparedAt(0));
        player.setScrubbing(true);
        const quint64 audioRequestsBeforeDrag = PlaybackAudio::decodeRequestCount();
        QSignalSpy images(&sink, &QVideoSink::videoFrameChanged);
        for (qint64 position : {80, 240, 400, 320}) player.prepare(position);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(320), 3000);
        QVERIFY(!images.isEmpty());
        for (const auto& event : images) {
            const auto frame = qvariant_cast<QVideoFrame>(event.first());
            QVERIFY(frame.startTime() <= 320000 && frame.endTime() > 320000);
        }
        // Sustained pointer input must not extend a debounce timer forever.
        const int beforeDrag = images.size();
        for (int i = 0; i < 30; ++i) {
            player.pause(); // QuickCanvasHost applies pause before every target.
            player.prepare((i * 37) % 1200);
            QTest::qWait(5);
        }
        QVERIFY(images.size() > beforeDrag);
        player.prepare(1000);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(1000), 3000);
        QCOMPARE(PlaybackAudio::decodeRequestCount(), audioRequestsBeforeDrag);
        player.setScrubbing(false);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(1000), 3000);
        QCOMPARE(player.preparedFrame(1000).size(), asset->firstFrame.frame.size());
        player.play();
        QTRY_VERIFY_WITH_TIMEOUT(player.isPlaying() && player.position() > 1050, 3000);
        QCOMPARE(player.error(), QMediaPlayer::NoError);
        player.clearAsset();
    }
    void indexedFramesReplacePermanentThumbnails() {
        QTemporaryDir directory;
        const auto path = directory.filePath("long.mp4");
        QVERIFY(writeVideo(path, false, false, 0, 23, 0, 600));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QVERIFY(asset->thumbnails.isEmpty());
        QCOMPARE(asset->frameIndex.size(), 600);
        QCOMPARE(asset->frameIndex.first().timestampUs, qint64(0));
        QCOMPARE(asset->frameIndex.last().timestampUs, qint64(599 * 40000));
        QVERIFY(asset->allIntra);
        QVERIFY(asset->compressedVideo.isEmpty());
        QCOMPARE(asset->videoPackets.bytes.size(), asset->videoPackets.bytes.capacity());
        QCOMPARE(asset->audioPackets.bytes.size(), asset->audioPackets.bytes.capacity());
        QCOMPARE(asset->memoryBreakdown().thumbnailBytes, quint64(0));
        QCOMPARE(asset->memoryBreakdown().totalBytes(), asset->residentBytes);
        auto& scheduler = DecodeScheduler::instance();
        QImage first, last;
        scheduler.requestThumbnail(this, asset, 0, [&](auto image) { if (image) first = *image; });
        scheduler.requestThumbnail(this, asset, asset->frameIndex.last().timestampUs, [&](auto image) { if (image) last = *image; });
        QTRY_VERIFY(!first.isNull() && !last.isNull());
        QVERIFY(first.size().width() <= MediaThumbnails::Width);
        QVERIFY(first != last);
        QVERIFY(scheduler.thumbnailBytes() <= DecodeScheduler::ThumbnailLimit);
        scheduler.evictOptionalCaches();
        QCOMPARE(scheduler.thumbnailBytes(), quint64(0));
    }


    void intraPacketsPreserveVfrAndDecodeIndependently() {
        QTemporaryDir directory;
        const auto path = directory.filePath("rotated-vfr.mp4");
        QVERIFY(writeVideo(path, true, true, 2));
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->frameIndex.size(), qsizetype(asset->videoFrameCount));
        QCOMPARE(asset->videoPackets.packets.size(), asset->frameIndex.size());
        QVERIFY(asset->allIntra);
        qint64 previous = -1;
        for (int i = 0; i < asset->frameIndex.size(); ++i) {
            const auto& index = asset->frameIndex[i];
            QVERIFY(index.timestampUs > previous); previous = index.timestampUs;
            QVERIFY(asset->videoPackets.packets[i].flags & AV_PKT_FLAG_KEY);
            IndexedMediaDecoder fresh;
            auto frame = fresh.videoFrame(*asset, i, error);
            QVERIFY2(frame.isValid(), qPrintable(error));
            QCOMPARE(frame.startTime(), index.timestampUs);
            QCOMPARE(frame.endTime(), index.timestampUs + index.durationUs);
            QCOMPARE(frame.size(), QSize(128, 48));
            QCOMPARE(frame.rotation(), asset->firstFrame.frame.rotation());
        }
        QCOMPARE(asset->memoryBreakdown().totalBytes(), asset->residentBytes);
    }


    void fullResolutionScrubbingAndExactRelease() {
        const QString path = QString::fromUtf8(TEST_SAMPLE_VIDEO_FILE);
        if (!QFile::exists(path)) QSKIP("Optional repository sample is not installed");
        QElapsedTimer loading;
        loading.start();
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        qInfo() << "Intra preparation including full validation (ms):" << loading.elapsed()
                << "intra bytes:" << asset->videoPackets.bytes.size();
        QVERIFY(asset->allIntra);
        QVideoSink sink;
        ResidentVideoPlayer player;
        player.setVideoSink(&sink);
        player.setAsset(asset);
        player.prepare(0);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(0), 10000);
        QTest::qWait(50);
        const QList<qint64> targets = {2678, 789, 12345, 20000, 1200, 28000, 600, 17890, 10000, 5000};
        QList<qint64> nativeUs, proxyUs;
        auto await = [&](auto ready, QList<qint64>& samples, QElapsedTimer& clock) {
            while (!ready() && clock.elapsed() < 5000) QTest::qWait(1);
            samples.append(clock.nsecsElapsed() / 1000);
            return ready();
        };
        for (qint64 target : targets) {
            QElapsedTimer clock; clock.start();
            player.setPosition(target);
            QVERIFY(await([&] { return player.preparedAt(target); }, nativeUs, clock));
        }
        QSignalSpy nativeFrames(&player, &ResidentVideoPlayer::frameReady);
        player.setScrubbing(true);
        for (qint64 target : targets) {
            QElapsedTimer clock; clock.start();
            player.setPosition(target);
            QVERIFY(await([&] {
                const auto frame = sink.videoFrame();
                return frame.startTime() <= target * 1000 && frame.endTime() > target * 1000
                    && frame.size() == asset->firstFrame.frame.size();
            }, proxyUs, clock));
        }
        QVERIFY(nativeFrames.count() > 0); // the same native frames serve scrubbing and Play
        std::sort(nativeUs.begin(), nativeUs.end());
        std::sort(proxyUs.begin(), proxyUs.end());
        qInfo() << "1080p random seek latency in us: native median/max" << nativeUs[5] << nativeUs.last()
                << "proxy median/max" << proxyUs[5] << proxyUs.last();
        QSignalSpy previews(&sink, &QVideoSink::videoFrameChanged);
        int moves = 0;
        QTimer drag;
        drag.setTimerType(Qt::PreciseTimer);
        drag.setInterval(16);
        connect(&drag, &QTimer::timeout, &player, [&] {
            player.setPosition((++moves * 367) % 29000);
        });
        drag.start();
        QTest::qWait(1000);
        drag.stop();
        QVERIFY(previews.count() > 1);
        QVERIFY(nativeFrames.count() > 0);
        qInfo() << "One-second continuous drag: pointer targets" << moves
                << "presented proxy frames" << previews.count();
        // Rapid reverse/forward requests, then release before the worker completes.
        player.setPosition(21000);
        player.setPosition(1500);
        player.setScrubbing(false);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(1500), 5000);
        QCOMPARE(sink.videoFrame().size(), asset->firstFrame.frame.size());
        QTest::qWait(100);
        QCOMPARE(sink.videoFrame().size(), asset->firstFrame.frame.size());
        QVERIFY(!player.isPlaying());
        // Deleting an asset while a JPEG is in flight cannot republish its image.
        player.setScrubbing(true);
        player.setPosition(21000);
        player.clearAsset();
        QTest::qWait(50);
        QVERIFY(!sink.videoFrame().isValid());
    }

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
        QCOMPARE(geometry.durationUs, 0);
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
        QCOMPARE(asset->thumbnails.size(), 1);
        QCOMPARE(asset->thumbnails.first().image, source);
        QCOMPARE(asset->residentBytes, quint64(source.sizeInBytes()) + sizeof(ResidentThumbnail));
        const auto memory = asset->memoryBreakdown();
        QCOMPARE(memory.imageBytes, quint64(source.sizeInBytes()));
        QCOMPARE(memory.thumbnailBytes, quint64(sizeof(ResidentThumbnail))); // Pixels are shared with the image.
        QCOMPARE(memory.videoBytes, quint64(0));
        QCOMPARE(memory.posterBytes, quint64(0));
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
        QCOMPARE(asset->sha256, QString::fromLatin1(QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256).toHex()));
        QVERIFY(asset->compressedVideo.isEmpty());
        QVERIFY(asset->allIntra);
        QCOMPARE(asset->frameIndex.last().timestampUs, qint64(440000));
        IndexedMediaDecoder decoder;
        const auto last = decoder.videoFrame(*asset, asset->frameIndex.size()-1, error);
        QVERIFY(asset->firstFrame.frame.toImage().pixelColor(20,20) != last.toImage().pixelColor(20,20));
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
        QVERIFY(asset->compressedVideo.isEmpty());
        QVERIFY(asset->allIntra);
        QVERIFY(asset->thumbnails.isEmpty());
        QCOMPARE(retained, asset->residentBytes);
        QCOMPARE(asset->videoPackets.bytes.capacity(), asset->videoPackets.bytes.size());
        QVERIFY(peakBudget > retained);
        QVERIFY(peakBudget >= asset->conversionPeakBytes);
        QVERIFY(peakBudget < 512ULL * 1024 * 1024);
        qInfo() << "1080p retained bytes:" << retained << "preparation budget:" << peakBudget;
        // Exact original bitstream, shared between independent occurrences.
        QVideoSink sink;
        ResidentVideoPlayer one, two;
        one.setVideoSink(&sink);
        QElapsedTimer preparationTime;
        preparationTime.start();
        one.setAsset(asset); two.setAsset(asset);
        QCOMPARE(one.asset()->videoPackets.bytes.constData(), two.asset()->videoPackets.bytes.constData());
        QCOMPARE(one.asset()->videoPackets.bytes.constData(), asset->videoPackets.bytes.constData());
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
        QVERIFY(geometry.durationUs > 0);
        QCOMPARE(geometry.durationUs, probe.durationUs);
        QString error;
        const auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->displaySize, probe.displaySize);
        QCOMPARE(asset->firstFrame.frame.size(), QSize(128, 48));
        QVERIFY(asset->firstFrame.frame.rotation() != QtVideo::Rotation::None);
        QImage thumbnail;
        DecodeScheduler::instance().requestThumbnail(this, asset, 0, [&](auto image) { if (image) thumbnail = *image; });
        QTRY_VERIFY(!thumbnail.isNull());
        QVERIFY(thumbnail.height() <= MediaThumbnails::Height);
        QVERIFY(qAbs(qreal(thumbnail.width()) / thumbnail.height() - 48.0 / 128) < 0.02);
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
        bool announcedAudiovisualPreparation = false;
        connect(&player, &ResidentVideoPlayer::preparationChanged, &player, [&] {
            if (!player.hasPreparedPlayback()) return;
            QVERIFY(player.preparedAt(player.position()));
            announcedAudiovisualPreparation = true;
        });
        player.setAsset(asset);
        QCOMPARE(attempts, 1);
        QCOMPARE(releases, 0);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(player.error(), QMediaPlayer::NoError);
        QCOMPARE(player.mediaStatus(), QMediaPlayer::LoadedMedia);
        QCOMPARE(player.playbackState(), QMediaPlayer::StoppedState);
        QVERIFY(sink.videoFrame().isValid());
        QVERIFY(!player.preparedAt(0));
        QVERIFY(player.waitingForMemory());
        QVERIFY(!announcedAudiovisualPreparation);

        // A sampled-memory retry may still refuse without turning loading into
        // a terminal error. Recovery prepares the cursor without starting it.
        player.retryPreparation();
        QCOMPARE(attempts, 2);
        QCOMPARE(errors.size(), 0);
        QVERIFY(player.waitingForMemory());
        admitted = true;
        player.retryPreparation();
        QTRY_VERIFY_WITH_TIMEOUT(announcedAudiovisualPreparation, 3000);
        QVERIFY(player.hasPreparedPlayback());
        QVERIFY(!player.waitingForMemory());
        QVERIFY(!player.isPlaying());
        QCOMPARE(attempts, 3);
        player.play();
        QTRY_VERIFY2_WITH_TIMEOUT(player.position() > 50, qPrintable(player.errorString()), 3000);
        QCOMPARE(attempts, 3);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(player.error(), QMediaPlayer::NoError);
        player.clearAsset();
        QVERIFY(!player.hasPreparedPlayback());
        QVERIFY(!player.waitingForMemory());
        QCOMPARE(releases, 1);
    }

    void scrubReleaseDuringInitialPreparationDoesNotStrandLookahead() {
        QTemporaryDir directory;
        const QString path = directory.filePath("initial-scrub.mp4");
        QVERIFY(writeVideo(path));
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset && asset->frameIndex.size() > 2);
        DecodeScheduler::instance().evictOptionalCaches();
        ResidentVideoPlayer player;
        player.setAsset(asset);
        // Release before the queued entry-frame lookahead has completed, with
        // no cursor movement. Cancelled requests must be scheduled again.
        player.setScrubbing(true);
        player.setScrubbing(false);
        player.prepare(0);
        QTRY_VERIFY2_WITH_TIMEOUT(player.preparedAt(0), qPrintable(player.errorString()), 3000);
        player.play();
        QTRY_VERIFY_WITH_TIMEOUT(player.isPlaying() && player.position() > 50, 3000);
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
        // A poster alone cannot retire the lookahead/audio preparation budget.
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(0), 3000);
        QCOMPARE(preparations, 1);
        player.clearAsset();
        QCOMPARE(releasedPrepared.size(), 1);
        QVERIFY(releasedPrepared.last());

        player.setAsset(asset);
        QTRY_VERIFY2_WITH_TIMEOUT(player.preparedAt(0), qPrintable(player.errorString()), 3000);
        QCOMPARE(reservations, 2);
        QCOMPARE(preparations, 2);
        player.prepare(200);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(200), 3000);
        QCOMPARE(preparations, 2); // seek frames cannot retire the budget twice
        player.clearAsset();
        QCOMPARE(releasedPrepared.size(), 2);
        QVERIFY(releasedPrepared.last());

        // Residency observers can retire a player synchronously as its first
        // frame transfers the budget into the measured process allocation.
        asset->playbackPrepared = [&] { ++preparations; player.clearAsset(); };
        player.setAsset(asset);
        QTRY_COMPARE_WITH_TIMEOUT(preparations, 3, 3000);
        QCOMPARE(reservations, 3);
        QVERIFY(!player.asset());
        QCOMPARE(releasedPrepared.size(), 3);
        QVERIFY(releasedPrepared.last());

        asset->reservePlayback = [&] { ++reservations; player.clearAsset(); return true; };
        player.setAsset(asset);
        QCOMPARE(reservations, 4);
        QCOMPARE(preparations, 3);
        QVERIFY(!player.asset());
        QCOMPARE(releasedPrepared.size(), 4);
        QVERIFY(!releasedPrepared.last());
    }

    void invalidPacketFailureReleasesCursorAndAllowsRecovery() {
        QTemporaryDir directory;
        const QString path = directory.filePath("failed-preparation.mp4");
        QVERIFY(writeVideo(path));
        const auto valid = MediaDecoder::decode(path);
        QVERIFY(valid);
        auto damaged = std::make_shared<ResidentMediaAsset>(*valid);
        damaged->sha256 += QStringLiteral("-damaged");
        damaged->videoPackets.packets[5].offset = damaged->videoPackets.bytes.size() + 1;
        int releases = 0;
        damaged->releasePlayback = [&](bool prepared) { QVERIFY(prepared); ++releases; };
        ResidentVideoPlayer player;
        QSignalSpy errors(&player, &ResidentVideoPlayer::errorOccurred);
        player.setAsset(damaged);
        QTRY_VERIFY_WITH_TIMEOUT(player.preparedAt(0), 3000);
        player.prepare(201);
        QTRY_COMPARE(errors.size(), 1);
        QTRY_COMPARE(releases, 1);
        QVERIFY(!player.preparedAt(201));
        QCOMPARE(player.mediaStatus(), QMediaPlayer::InvalidMedia);
        player.setAsset(valid);
        QTRY_VERIFY(player.preparedAt(201));
        player.clearAsset();
        QCOMPARE(releases, 1);
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
        QTRY_VERIFY(player.preparedAt(0)); // validated first frame and initial audio are reusable
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

    void sharedDecodeDeduplicatesAndRejectsRetiredGenerations() {
        QTemporaryDir directory;
        const auto path = directory.filePath("shared.mp4");
        QVERIFY(writeVideo(path));
        auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        auto& scheduler = DecodeScheduler::instance();
        scheduler.evictOptionalCaches();
        std::vector<SharedMediaFramePtr> frames(10);
        for (int i = 0; i < 10; ++i)
            scheduler.request(this, 71, asset, 201000, DecodeScheduler::Prepare, 0,
                [&, i](auto frame, const QString& error) { QVERIFY2(error.isEmpty(), qPrintable(error)); frames[i] = frame; });
        QTRY_VERIFY(std::all_of(frames.begin(), frames.end(), [](const auto& f) { return bool(f); }));
        for (const auto& frame : frames) QCOMPARE(frame.get(), frames.front().get());
        QCOMPARE(scheduler.workerCount(), std::max(1, std::min(4, QThread::idealThreadCount()/2)));
        bool retiredCalled = false;
        scheduler.request(this, 72, asset, 401000, DecodeScheduler::Scrub, 0,
            [&](auto, const QString&) { retiredCalled = true; });
        scheduler.cancel(this, 72);
        QTRY_COMPARE(scheduler.pendingJobs(), 0);
        QVERIFY(!retiredCalled);
        frames.clear(); scheduler.evictOptionalCaches();
        QTRY_COMPARE(scheduler.trackedFrameBytes(), asset->posterBytes);
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

    void difficultSourceRepresentations_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<bool>("intra");
        QTest::newRow("4k") << QStringLiteral("4k.mp4") << true;
        QTest::newRow("hdr10") << QStringLiteral("hdr.mp4") << false;
        QTest::newRow("alpha") << QStringLiteral("alpha.mp4") << false;
    }
    void difficultSourceRepresentations() {
        QFETCH(QString, name); QFETCH(bool, intra);
        const auto directory = qEnvironmentVariable("MOUFFETTE_ENGINE_FIXTURES");
        if (directory.isEmpty()) QSKIP("Set MOUFFETTE_ENGINE_FIXTURES to the benchmark fixture directory");
        QString error;
        std::shared_ptr<const ResidentMediaAsset> asset;
        if (name == QLatin1String("alpha.mp4")) {
            // The import contract remains MP4-only. Exercise the engine's alpha
            // fallback with a lossless MOV fixture independently of admission.
            auto source = std::make_shared<ResidentMediaAsset>();
            QFile file(QDir(directory).filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            source->compressedVideo = file.readAll(); source->compressedVideo.squeeze();
            source->residentBytes = source->compressedVideo.capacity();
            source->sha256 = QStringLiteral("alpha-fixture"); source->video = true;
            source->displaySize = QSize(128,96); source->durationUs = 1000000;
            source->firstFrame.frame = QVideoFrame(QVideoFrameFormat(source->displaySize, QVideoFrameFormat::Format_ARGB8888));
            source->firstFrame.durationUs = 100000;
            QVERIFY2(IndexedMediaDecoder::build(*source, {}, 0, error), qPrintable(error));
            asset = source;
        } else asset = MediaDecoder::decode(QDir(directory).filePath(name), {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QCOMPARE(asset->allIntra, intra);
        QCOMPARE(asset->compressedVideo.isEmpty(), intra);
        QVERIFY(asset->videoPackets.bytes.isEmpty() == !intra);
        // The converted all-intra representation needs no editing proxy;
        // HDR and alpha must not pass through the lossy SDR JPEG path.
        QVERIFY(!MediaPreviewStore::supportsScrubProxy(*asset));
        IndexedMediaDecoder decoder;
        for (int index : {0, 1, 2, int(asset->frameIndex.size()/2), int(asset->frameIndex.size()-1), 0}) {
            auto frame = decoder.videoFrame(*asset, index, error);
            QVERIFY2(frame.isValid(), qPrintable(error));
            QCOMPARE(frame.startTime(), asset->frameIndex[index].timestampUs);
            if (name == QLatin1String("4k.mp4")) QCOMPARE(frame.size(), QSize(3840,2160));
            if (name == QLatin1String("hdr.mp4")) {
                QCOMPARE(frame.pixelFormat(), QVideoFrameFormat::Format_YUV420P10);
                QCOMPARE(frame.surfaceFormat().colorTransfer(), QVideoFrameFormat::ColorTransfer_ST2084);
                QCOMPARE(frame.surfaceFormat().colorSpace(), QVideoFrameFormat::ColorSpace_BT2020);
            }
            if (name == QLatin1String("alpha.mp4")) {
                const int alpha = ResidentVideoPlayer::presentationFrame(frame).toImage().pixelColor(20,20).alpha();
                QVERIFY(alpha > 120 && alpha < 136);
            }
        }
    }

    void repeatedCursorCyclesReleaseSharedAllocations() {
        auto& scheduler = DecodeScheduler::instance();
        QTRY_COMPARE(scheduler.pendingJobs(), 0);
        scheduler.evictOptionalCaches();
        const auto baseline = mediaFrameCpuBytes();
        QTemporaryDir directory;
        const auto path = directory.filePath("cycles.mp4");
        QVERIFY(writeVideo(path, false, false, 0, 40));
        auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        std::weak_ptr<const ResidentMediaAsset> lifetime = asset;
        for (int cycle = 0; cycle < 30; ++cycle) {
            std::vector<std::unique_ptr<ResidentVideoPlayer>> players;
            for (int occurrence = 0; occurrence < 6; ++occurrence) {
                auto player = std::make_unique<ResidentVideoPlayer>();
                player->setAsset(asset);
                player->setScrubbing(true);
                player->prepare((cycle * 37 + occurrence * 53) % 900);
                players.push_back(std::move(player));
            }
            // Delete subscribers while jobs are pending, then drain late results.
            players.erase(players.begin(), players.begin() + 3);
            QTRY_COMPARE(scheduler.pendingJobs(), 0);
            // Releasing a scrub schedules lookahead. Delete all subscribers
            // before completion: late work must not repopulate the evicted cache.
            for (auto& player : players) player->setScrubbing(false);
            players.clear(); scheduler.evictOptionalCaches();
            QTRY_COMPARE(scheduler.pendingJobs(), 0);
            QTRY_COMPARE(mediaFrameCpuBytes(), baseline + asset->posterBytes);
            QVERIFY(scheduler.optionalFrameBytes() <= DecodeScheduler::FrameLimit);
        }
        asset.reset();
        QTRY_VERIFY(lifetime.expired());
        QTRY_COMPARE(mediaFrameCpuBytes(), baseline);
    }

    void repeatedPlayAfterConsumedAudioRepreparesSameCursor() {
        QTemporaryDir directory;
        const auto path = directory.filePath("repeat-audio.mp4");
        QVERIFY(writeVideo(path, false, false, 0, 80, 0, 40));
        auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        ResidentVideoPlayer player;
        QAudioOutput output; output.setMuted(true); player.setAudioOutput(&output);
        player.setAsset(asset);
        for (int repetition = 0; repetition < 5; ++repetition) {
            player.prepare(500);
            QTRY_VERIFY2(player.preparedAt(500), qPrintable(player.errorString()));
            player.play();
            QTRY_VERIFY(player.position() > 620);
            player.pause();
        }
    }

    void indexedAudioPreservesTrimAndSeekSamples() {
        QTemporaryDir directory;
        const QString path = directory.filePath("indexed-audio.mp4");
        QVERIFY(writeVideo(path, false, false, 0, 80, 512));
        QString error;
        auto asset = MediaDecoder::decode(path, {}, &error);
        QVERIFY2(asset, qPrintable(error));
        QVERIFY(QFile::remove(path));
        for (int rate : {48000, 44100}) for (qint64 target : {qint64(0), qint64(123000), qint64(1000000)}) {
            const int frames = rate / 5;
            IndexedMediaDecoder decoder;
            const auto pcm = decoder.audio(*asset, target, frames, rate, 1, error);
            QVERIFY2(!pcm.isEmpty(), qPrintable(error));
            QCOMPARE(pcm.size(), qsizetype(frames * sizeof(float)));
            const auto* samples = reinterpret_cast<const float*>(pcm.constData());
            double squaredError = 0;
            for (int sample = 256; sample < frames; ++sample) {
                const double time = target/1000000.0 + 512.0/48000 + double(sample)/rate;
                const double expected = .1 * std::sin(time*440.0*6.283185307179586);
                squaredError += std::pow(samples[sample]-expected, 2);
            }
            const double rms = std::sqrt(squaredError/(frames-256));
            QVERIFY2(rms < .008, qPrintable(QStringLiteral("Audio phase/trim mismatch at %1 us / %2 Hz: RMS %3").arg(target).arg(rate).arg(rms)));
        }
        IndexedMediaDecoder decoder;
        const auto silence = decoder.audio(*asset, asset->durationUs + 100000, 9600, 48000, 2, error);
        QCOMPARE(silence, QByteArray(9600*2*sizeof(float), '\0'));
    }

    void aacPrimingKeepsAudioTimestampsAcrossReload() {
        QTemporaryDir directory;
        const QString path = directory.filePath("trimmed-audio.mp4");
        // Trim part of an AAC frame, in addition to the encoder's whole-frame
        // delay. FFmpeg must advance its timestamp along with the samples.
        QVERIFY(writeVideo(path, false, false, 0, 23, 512));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        file.close();
        QVERIFY(QFile::remove(path));

        for (int load = 0; load < 2; ++load) {
            QBuffer source;
            source.setData(bytes);
            QVERIFY(source.open(QIODevice::ReadOnly));
            QAudioBufferOutput audio;
            QMediaPlayer player;
            player.setAudioBufferOutput(&audio);
            QSignalSpy buffers(&audio, &QAudioBufferOutput::audioBufferReceived);
            player.setSourceDevice(&source, QUrl(QStringLiteral("resident:///video.mp4")));
            player.play();
            QTRY_VERIFY2_WITH_TIMEOUT(!buffers.isEmpty(), qPrintable(player.errorString()), 5000);
            const auto first = qvariant_cast<QAudioBuffer>(buffers.first().first());
            QVERIFY(first.isValid());
            QCOMPARE(first.startTime(), qint64(0));
            QTRY_COMPARE_WITH_TIMEOUT(player.mediaStatus(), QMediaPlayer::EndOfMedia, 5000);
            QCOMPARE(player.error(), QMediaPlayer::NoError);
            // Destruction joins the decoder before the memory source goes away;
            // the next iteration reopens the same bytes with a fresh decoder.
        }
    }

    void playingAudioSeekPreservesRequestedTarget_data() {
        QTest::addColumn<qint64>("targetUs");
        QTest::newRow("backward") << qint64(300000);
        QTest::newRow("forward") << qint64(1800000);
    }

    void playingAudioSeekPreservesRequestedTarget() {
        QFETCH(qint64, targetUs);
        QTemporaryDir directory;
        const QString path = directory.filePath("playing-audio-seek.mp4");
        QVERIFY(writeVideo(path, false, false, 0, 120, 0, 64));
        auto asset = MediaDecoder::decode(path);
        QVERIFY(asset);
        QAudioOutput output;
        output.setMuted(true);
        PlaybackAudio audio;
        audio.setOutput(&output);
        audio.setAsset(asset);
        if (!PlaybackAudio::deviceCount()) QSKIP("No audio callback device is available");
        QSignalSpy failures(&audio, &PlaybackAudio::failed);
        audio.prepare(1000000);
        QTRY_VERIFY_WITH_TIMEOUT(audio.preparedAt(1000000), 3000);
        audio.play(1000000);
        // Suspend GUI refills while the independent callback drains the PCM.
        // Empty slots make the next play() prepare immediately, before its new
        // clock offset is installed; this reproduced lost backward seeks.
        QTest::qSleep(350);
        QVERIFY(audio.presentedSincePlay());
        QVERIFY(!audio.preparedAt(1150000));
        audio.play(targetUs);
        audio.pause(); // Keep the newly prepared target available for inspection.
        QTRY_VERIFY_WITH_TIMEOUT(audio.preparedAt(targetUs), 1000);
        audio.play(targetUs);
        QTRY_VERIFY_WITH_TIMEOUT(audio.presentedSincePlay(), 1000);
        QVERIFY(failures.isEmpty());
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
        QTRY_VERIFY_WITH_TIMEOUT(retained.expired(), 3000);
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
