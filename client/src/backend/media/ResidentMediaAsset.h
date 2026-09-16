#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVideoFrame>
#include <memory>
#include <vector>

// Published only after the entire source has decoded successfully. Copies of
// frames share their read-only CPU planes; occurrences never share a cursor.
struct ResidentVideoFrame {
    QVideoFrame frame;
    qint64 timestampUs = 0;
    qint64 durationUs = 0;
};

struct ResidentAudioChunk {
    QByteArray pcm; // interleaved float32, in audioFormat's channel order
    qint64 timestampUs = 0;
    qint64 sampleFrames = 0;
};

struct ResidentMediaAsset {
    QString sha256;
    QSize displaySize;
    bool video = false;
    QImage image;
    quint64 residentBytes = 0;
    qint64 durationUs = 0;
    std::vector<ResidentVideoFrame> frames;
    QAudioFormat audioFormat;
    std::vector<ResidentAudioChunk> audio;
};
