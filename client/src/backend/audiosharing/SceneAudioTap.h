#pragma once
#include <QByteArray>
#include <QString>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

// One SPSC queue per local output mixer. The real-time producer neither locks
// nor allocates. Only rendered ReceivedScene samples may enter this queue.
class SceneAudioTap {
public:
    static constexpr int BlockFrames = 1024, Blocks = 32, MaximumChannels = 8;
    SceneAudioTap();
    ~SceneAudioTap();
    void setEnabled(bool enabled); // Consumer / application thread only.
    void push(const float* pcm, int frames, int channels, int rate, qint64 timestampUs);
    bool take(QByteArray& stereo, qint64& timestampUs); // Converts to 48 kHz off the audio callback.
    bool takeForCapture(QByteArray& stereo, qint64& timestampUs, bool& discontinuity, QString& error);
private:
    struct Block {
        std::array<float, BlockFrames * MaximumChannels> pcm;
        quint64 epoch;
        qint64 timestampUs;
        int frames, channels, rate;
    };
    std::array<Block, Blocks> blocks;
    std::atomic<quint64> write{0}, read{0}, epoch{0};
    quint64 serial = 0;
    struct Converter;
    std::unique_ptr<Converter> converter;
};

// Registry access is on the application thread; callbacks retain their tap.
class SceneAudioBus {
public:
    static SceneAudioBus& instance();
    std::shared_ptr<SceneAudioTap> createTap();
    void setEnabled(bool enabled);
    std::vector<std::shared_ptr<SceneAudioTap>> sources();
private:
    bool enabled = false;
    std::vector<std::weak_ptr<SceneAudioTap>> taps;
};
