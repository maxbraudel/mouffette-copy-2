#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QSize>
#include <QString>
#include <QVideoFrame>
#include <algorithm>
#include <memory>

// The session controller chooses a profile; capture and codec apply the same
// bounded values. Units are explicit so network budgets remain in bits/second.
struct ScreenStreamProfile {
    int maximumEdge = 1920;
    int framesPerSecond = 30;
    int bitrateBps = 4000000;
    int idleIntervalMs = 1000;
    int keyFrameIntervalMs = 5000;
    int minimumKeyFrameIntervalMs = 500;
    QString softwarePreset = QStringLiteral("veryfast");

    ScreenStreamProfile normalized() const {
        auto value = *this;
        value.maximumEdge = std::clamp(value.maximumEdge, 160, 3840) & ~1;
        value.framesPerSecond = std::clamp(value.framesPerSecond, 1, 60);
        value.bitrateBps = std::clamp(value.bitrateBps, 32000, 100000000);
        value.idleIntervalMs = std::clamp(value.idleIntervalMs, 250, 4000);
        value.keyFrameIntervalMs = std::clamp(value.keyFrameIntervalMs, 500, 30000);
        value.minimumKeyFrameIntervalMs = std::clamp(value.minimumKeyFrameIntervalMs, 100, 2000);
        if (value.softwarePreset != QLatin1String("ultrafast")
            && value.softwarePreset != QLatin1String("superfast")
            && value.softwarePreset != QLatin1String("veryfast")
            && value.softwarePreset != QLatin1String("faster")
            && value.softwarePreset != QLatin1String("fast"))
            value.softwarePreset = QStringLiteral("veryfast");
        return value;
    }
    bool operator==(const ScreenStreamProfile& other) const {
        return maximumEdge == other.maximumEdge && framesPerSecond == other.framesPerSecond
            && bitrateBps == other.bitrateBps && idleIntervalMs == other.idleIntervalMs
            && keyFrameIntervalMs == other.keyFrameIntervalMs
            && minimumKeyFrameIntervalMs == other.minimumKeyFrameIntervalMs
            && softwarePreset == other.softwarePreset;
    }
    bool operator!=(const ScreenStreamProfile& other) const { return !(*this == other); }
};

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
    static constexpr int MaximumDecodeEdge = 3840;
    static constexpr int FramesPerSecond = 30;
    static constexpr int MaximumPacketBytes = 2 * 1024 * 1024;
    explicit ScreenStreamEncoder(bool preferHardware = true);
    ~ScreenStreamEncoder();
    // Reconfiguration starts a fresh self-contained stream on the next frame.
    // Hardware backends need reopening to apply new rate-control parameters.
    void setProfile(const ScreenStreamProfile& profile);
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
