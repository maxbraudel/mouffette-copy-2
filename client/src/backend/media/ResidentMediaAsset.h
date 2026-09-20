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
#include "backend/media/MediaPacketStore.h"

// Published only after complete validation and preparation. Runtime packets
// and decoded entry frames are shared by file identity; occurrences are cursors.
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
constexpr int Width = 192;
constexpr int Height = 108;
constexpr int PreviewCount = 16;
}

// An immutable, display-only snapshot. It never authorizes playback: the source
// can still fail validation after these images were decoded. Pixel allocations
// are shared with the importing asset's first frame and thumbnails, not copied
// per snapshot. They remain counted once in the asset/job memory breakdown.
struct ResidentMediaPreview {
    QString sha256;
    QSize displaySize;
    qint64 durationUs = 0;
    QVideoFrame poster;
    QVector<ResidentThumbnail> thumbnails;
    // Referenced poster + thumbnail storage; not an additional allocation.
    quint64 residentBytes = 0;
};

// Disjoint allocations owned by an asset; excludes decoder/GPU estimates.
struct ResidentMediaMemory {
    quint64 videoBytes = 0;
    quint64 imageBytes = 0;
    quint64 posterBytes = 0;
    quint64 thumbnailBytes = 0;
    quint64 audioPreviewBytes = 0;
    quint64 totalBytes() const { return videoBytes + imageBytes + posterBytes + thumbnailBytes + audioPreviewBytes; }
    ResidentMediaMemory& operator+=(const ResidentMediaMemory& other) {
        videoBytes += other.videoBytes; imageBytes += other.imageBytes;
        posterBytes += other.posterBytes; thumbnailBytes += other.thumbnailBytes;
        audioPreviewBytes += other.audioPreviewBytes;
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
    ResidentMediaMemory memoryBreakdown() const {
        const quint64 source = residentBytes - posterBytes - thumbnailBytes - quint64(firstAudioPcm.capacity());
        return {video ? source : 0, video ? 0 : source, posterBytes, thumbnailBytes, quint64(firstAudioPcm.capacity())};
    }
    qint64 durationUs = 0;
    QByteArray compressedVideo;
    // All-intra sources release compressedVideo after complete validation.
    bool allIntra = false;
    QString representationReason;
    MediaPacketStore videoPackets;
    MediaPacketStore audioPackets;
    bool audioUsesVideoBytes = false;
    QVector<MediaFrameIndex> frameIndex;
    QSize codedDisplaySize;
    int rotation = 0;
    qint64 sourceOriginUs = 0;
    quint64 conversionPeakBytes = 0;
    quint64 sourceFileBytes = 0;
    ResidentVideoFrame firstFrame;
    quint64 videoFrameCount = 0;
    quint64 audioSampleCount = 0;
    quint64 playbackBudgetBytes = 0; // estimate, never reported as allocated bytes
    int videoTrack = 0;
    int audioTrack = -1;
    QAudioFormat audioFormat;
    QByteArray firstAudioPcm; // 200 ms, float stereo at 48 kHz, shared preparation only
    // Main-thread admission, installed by the owning residency manager. The
    // future decoder budget is held until the cursor's image, lookahead and
    // audio are prepared; allocations then enter the OS measurement. Release must
    // identify whether preparation consumed the outstanding reservation.
    std::function<bool()> reservePlayback;
    std::function<void()> playbackPrepared;
    std::function<void(bool wasPrepared)> releasePlayback;
};
