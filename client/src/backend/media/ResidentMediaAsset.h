#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVideoFrame>
#include <QVector>
#include <memory>
#include <functional>
#include <vector>

// Published only after validation to EOF. Video bytes retain the original
// compression; editing proxies are optional, independently compressed images. Players share the bytes,
// but own independent, bounded decoder queues and cursors.
struct ResidentVideoFrame {
    QVideoFrame frame;
    qint64 timestampUs = 0;
    qint64 durationUs = 0;
};

struct ResidentThumbnail {
    QImage image;
    qint64 timestampUs = 0;
};

namespace MediaThumbnails {
constexpr int MaxCount = 96;
constexpr int Width = 192;
constexpr int Height = 108;
constexpr qint64 MinimumIntervalUs = 125000;
constexpr quint64 MaxBytes = quint64(MaxCount) * (Width * Height * 4 + sizeof(ResidentThumbnail));
}

// One independently decodable image per source frame, never sparse timeline tiles.
struct ResidentScrubFrame {
    QByteArray jpeg;
    qint64 timestampUs = 0;
    qint64 durationUs = 0;
};
namespace MediaScrubProxy {
constexpr int Width = 640;
constexpr int Height = 360;
constexpr quint64 MaxBytes = 64ULL * 1024 * 1024;
}

// Disjoint allocations owned by an asset; excludes decoder/GPU estimates.
struct ResidentMediaMemory {
    quint64 videoBytes = 0;
    quint64 imageBytes = 0;
    quint64 posterBytes = 0;
    quint64 thumbnailBytes = 0;
    quint64 scrubProxyBytes = 0;
    quint64 totalBytes() const { return videoBytes + imageBytes + posterBytes + thumbnailBytes + scrubProxyBytes; }
    ResidentMediaMemory& operator+=(const ResidentMediaMemory& other) {
        videoBytes += other.videoBytes; imageBytes += other.imageBytes;
        posterBytes += other.posterBytes; thumbnailBytes += other.thumbnailBytes;
        scrubProxyBytes += other.scrubProxyBytes;
        return *this;
    }
};

struct ResidentMediaAsset {
    QString sha256;
    QSize displaySize;
    bool video = false;
    QImage image;
    QVector<ResidentThumbnail> thumbnails;
    quint64 residentBytes = 0;
    quint64 posterBytes = 0;
    quint64 thumbnailBytes = 0;
    std::vector<ResidentScrubFrame> scrubFrames;
    quint64 scrubProxyBytes = 0;
    ResidentMediaMemory memoryBreakdown() const {
        const quint64 source = residentBytes - posterBytes - thumbnailBytes - scrubProxyBytes;
        return {video ? source : 0, video ? 0 : source, posterBytes, thumbnailBytes, scrubProxyBytes};
    }
    qint64 durationUs = 0;
    QByteArray compressedVideo;
    ResidentVideoFrame firstFrame;
    quint64 videoFrameCount = 0;
    quint64 audioSampleCount = 0;
    quint64 playbackBudgetBytes = 0; // estimate, never reported as allocated bytes
    int videoTrack = 0;
    int audioTrack = -1;
    QAudioFormat audioFormat;
    // Main-thread admission, installed by the owning residency manager. The
    // future decoder budget is held until its first decoded frame: subsequent
    // allocations are represented by the OS memory measurement. Release must
    // identify whether preparation consumed the outstanding reservation.
    std::function<bool()> reservePlayback;
    std::function<void()> playbackPrepared;
    std::function<void(bool wasPrepared)> releasePlayback;
};
