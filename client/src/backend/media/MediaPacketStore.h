#pragma once

#include <QByteArray>
#include <QSize>
#include <QVector>
#include <memory>

struct AVCodecParameters;
struct AVFrame;
class QVideoFrame;
class QString;

// Runtime data only. Project identities and transfers always use the source SHA.
struct MediaPacket {
    qint64 offset = 0;
    int size = 0;
    qint64 pts = 0, dts = 0, duration = 0;
    int flags = 0;
    QByteArray skipSamples;
};
struct MediaFrameIndex {
    qint64 timestampUs = 0, durationUs = 0;
};
struct MediaPacketStore {
    QByteArray bytes;
    QVector<MediaPacket> packets;
    std::shared_ptr<AVCodecParameters> codec;
    int timeBaseNum = 1, timeBaseDen = 1000000;
    qint64 originUs = 0;
    // Updated on append/compaction: budget checks run for every packet and
    // must not rescan an ever-growing packet index during long imports.
    quint64 packetSideDataBytes = 0;
    quint64 allocatedBytes() const;
    void compact();
};

// Immutable native planes, with Qt's colour, orientation and presentation metadata.
QVideoFrame mediaFrameFromAv(const AVFrame* source, QSize displaySize, int rotation,
                            qint64 startUs, qint64 endUs, QString& error,
                            quint64* allocatedBytes = nullptr);

quint64 mediaFrameCpuBytes();
quint64 mediaFrameAllocationBytes(const QVideoFrame& frame);
