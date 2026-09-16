#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVideoFrame>
#include <memory>
#include <functional>

// Published only after validation to EOF. Video bytes retain the original
// compression; only the poster is decoded permanently. Players share the bytes,
// but own independent, bounded decoder queues and cursors.
struct ResidentVideoFrame {
    QVideoFrame frame;
    qint64 timestampUs = 0;
    qint64 durationUs = 0;
};

struct ResidentMediaAsset {
    QString sha256;
    QSize displaySize;
    bool video = false;
    QImage image;
    quint64 residentBytes = 0;
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
