#pragma once

#include "backend/media/MediaDecoder.h"

struct AVFrame;

// FFmpeg contexts live exclusively on a scheduler worker, never on a cursor.
class IndexedMediaDecoder final {
public:
    IndexedMediaDecoder();
    ~IndexedMediaDecoder();
    IndexedMediaDecoder(const IndexedMediaDecoder&) = delete;
    IndexedMediaDecoder& operator=(const IndexedMediaDecoder&) = delete;
    static bool build(ResidentMediaAsset& asset, const MediaDecoder::DecodeCallbacks& callbacks,
                      quint64 scratchBytes, QString& error);
    static int frameAt(const ResidentMediaAsset& asset, qint64 timestampUs);
    using Cancelled = std::function<bool()>;
    QVideoFrame videoFrame(const ResidentMediaAsset& asset, int index, QString& error,
                          const Cancelled& cancelled = {});
    QImage thumbnail(const ResidentMediaAsset& asset, int index, QString& error,
                     const Cancelled& cancelled = {});
    // Worker-only, bounded output conversion. The decoded reference frames never
    // make a full-resolution RGB round trip just to produce a small preview.
    QImage previewImage(const ResidentMediaAsset& asset, int index, QSize bounds,
                        QString& error, const Cancelled& cancelled = {});
    // Interleaved float samples at the device rate. Timestamp includes source edit lists.
    QByteArray audio(const ResidentMediaAsset& asset, qint64 startUs, int frames,
                     int sampleRate, int channels, QString& error);
private:
    AVFrame* decodeFrame(const ResidentMediaAsset& asset, int index, QString& error,
                         const Cancelled& cancelled);
    struct Impl;
    std::unique_ptr<Impl> d;
};
