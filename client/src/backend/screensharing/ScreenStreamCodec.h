#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QSize>
#include <QString>
#include <QVideoFrame>
#include <memory>

struct ScreenStreamPacket {
    QByteArray annexB;
    QSize size;
    qint64 timestampUs = 0;
    bool keyFrame = false;
};
Q_DECLARE_METATYPE(ScreenStreamPacket)

// These contexts belong to one worker thread. They never allocate RGB images.
// Every key packet contains its SPS/PPS and can initialize a fresh decoder.
class ScreenStreamEncoder final {
public:
    static constexpr int MaximumEdge = 1920;
    static constexpr int FramesPerSecond = 30;
    static constexpr int MaximumPacketBytes = 2 * 1024 * 1024;
    explicit ScreenStreamEncoder(bool preferHardware = true);
    ~ScreenStreamEncoder();
    QList<ScreenStreamPacket> encode(QVideoFrame frame, bool forceKeyFrame, QString& error);
    QString backendName() const;
    // Native encoders may emit a packet only on the next submitted frame.
    // Request another sample promptly while an explicit recovery IDR is queued.
    bool hasDelayedKeyFrame() const;
    void reset();
private:
    struct Private;
    std::unique_ptr<Private> d;
};

class ScreenStreamDecoder final {
public:
    ScreenStreamDecoder();
    ~ScreenStreamDecoder();
    QVideoFrame decode(const QByteArray& annexB, qint64 timestampUs, QString& error);
    void reset();
private:
    struct Private;
    std::unique_ptr<Private> d;
};
