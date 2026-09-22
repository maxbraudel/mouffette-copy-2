#pragma once
#include <QByteArray>
#include <QString>
#include <QPointer>
#include <atomic>
#include <array>
#include <memory>

class QSharedMemory;
class AudioWorkerClient;
inline constexpr int AudioPreviewBlockFrames = 2400;
inline constexpr int AudioPreviewBlockCount = 4;
struct AudioPreviewBlock {
    // The decoder claims 0 -> 1, publishes 2; only the audio consumer frees 2.
    std::atomic<int> state{0};
    std::atomic<quint64> generation{0};
    std::atomic<qint64> startUs{0};
    std::array<float, AudioPreviewBlockFrames * 2> samples{};
};
struct AudioPreviewState {
    // Private ABI between the application and audio worker from the same build. Bump Version when
    // changing the layout or meaning of shared fields. The immutable header is
    // initialized before preview-add; OS allocation size is only a capacity.
    static constexpr quint32 Magic = 0x4d415031;
    static constexpr quint32 Version = 1;
    quint32 magic = Magic;
    quint32 version = Version;
    quint32 byteSize = sizeof(AudioPreviewState);
    std::atomic<quint64> generation{1};
    std::atomic<bool> playing{false};
    std::atomic<bool> presented{false};
    std::atomic<bool> consumerAvailable{false};
    std::atomic<float> gain{1};
    std::atomic<qint64> clockOffsetUs{0};
    std::array<AudioPreviewBlock, AudioPreviewBlockCount> blocks;
};
static_assert(std::atomic<qint64>::is_always_lock_free && std::atomic<quint64>::is_always_lock_free
    && std::atomic<bool>::is_always_lock_free && std::atomic<float>::is_always_lock_free
    && std::atomic<int>::is_always_lock_free, "Shared audio state needs process-shared lock-free atomics");
class AudioPreviewChannel {
public:
    ~AudioPreviewChannel();
    AudioPreviewState* state() const;
    QString key() const;
    // Main-thread control state, distinct from availability of an audio device.
    bool isAttachedToWorker() const { return workerAttached; }
    QByteArray deviceId;
private:
    friend class AudioWorkerClient;
    QPointer<AudioWorkerClient> owner;
    bool workerAttached = false;
    std::unique_ptr<QSharedMemory> memory;
};
