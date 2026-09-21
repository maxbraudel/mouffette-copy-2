#include "backend/managers/system/SystemVolumeMonitorBackend.h"
#include "backend/config/AppConfig.h"

#include <QTimer>

#include <CoreAudio/CoreAudio.h>
#include <Block.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {
AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                   AudioObjectPropertyElement element = kAudioObjectPropertyElementMain)
{
    return {selector, scope, element};
}

template<typename Value>
bool readProperty(AudioObjectID object, const AudioObjectPropertyAddress& property,
                  Value* value)
{
    UInt32 size = sizeof(Value);
    return AudioObjectGetPropertyData(object, &property, 0, nullptr, &size, value) == noErr
        && size == sizeof(Value);
}

std::vector<UInt32> outputChannels(AudioDeviceID device)
{
    UInt32 stereo[2]{};
    if (readProperty(device, address(kAudioDevicePropertyPreferredChannelsForStereo,
                                     kAudioObjectPropertyScopeOutput), &stereo)
        && stereo[0] != 0 && stereo[1] != 0) {
        if (stereo[0] == stereo[1]) return {stereo[0]};
        return {stereo[0], stereo[1]};
    }

    const auto property = address(kAudioDevicePropertyStreamConfiguration,
                                  kAudioObjectPropertyScopeOutput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &property, 0, nullptr, &size) != noErr
        || size < offsetof(AudioBufferList, mBuffers) || size > 1024 * 1024) {
        return {};
    }
    // AudioBufferList has variable length. Explicitly aligned storage also
    // accommodates devices that expose more than one interleaved stream.
    std::vector<std::max_align_t> storage(
        (size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    auto* buffers = reinterpret_cast<AudioBufferList*>(storage.data());
    const UInt32 capacity = size;
    if (AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, buffers) != noErr
        || size > capacity || size < offsetof(AudioBufferList, mBuffers)
        || buffers->mNumberBuffers
            > (size - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer)) {
        return {};
    }
    UInt32 channelCount = 0;
    for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
        if (buffers->mBuffers[index].mNumberChannels > 4096 - channelCount) return {};
        channelCount += buffers->mBuffers[index].mNumberChannels;
    }
    std::vector<UInt32> channels;
    channels.reserve(channelCount);
    for (UInt32 channel = 1; channel <= channelCount; ++channel) channels.push_back(channel);
    return channels;
}

class MacSystemVolumeMonitorBackend final : public SystemVolumeMonitorBackend
{
public:
    explicit MacSystemVolumeMonitorBackend(Publish publish)
        : SystemVolumeMonitorBackend(std::move(publish))
    {
        m_events.setInterval(16);
        m_events.setTimerType(Qt::PreciseTimer);
        connect(&m_events, &QTimer::timeout, this, [this]() {
            const bool rebind = m_mailbox->rebind.exchange(false);
            if (m_mailbox->changed.exchange(false) || rebind) refresh(rebind);
        });
        connect(&m_recovery, &QTimer::timeout, this, [this]() { refresh(false); });
    }

    ~MacSystemVolumeMonitorBackend() override { stop(); }

    void start() override
    {
        if (m_started) return;
        m_started = true;
        m_mailbox = std::make_shared<Mailbox>();
        const auto mailbox = m_mailbox;
        // CoreAudio copies the blocks, including their shared mailbox. A queued
        // callback remains safe after listener removal or backend destruction;
        // it never captures this or calls Qt from an audio/dispatch thread.
        m_volumeListener = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) {
            mailbox->changed.store(true);
        });
        m_topologyListener = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) {
            mailbox->rebind.store(true);
        });
        m_events.start();
        m_recovery.start(AppConfig::instance().systemVolumePollIntervalMs());
        refresh(false);
    }

    void stop() override
    {
        if (!m_started) return;
        m_started = false;
        m_events.stop();
        m_recovery.stop();
        removeListeners(false);
        Block_release(m_volumeListener);
        Block_release(m_topologyListener);
        m_volumeListener = nullptr;
        m_topologyListener = nullptr;
        m_mailbox.reset();
        m_device = kAudioObjectUnknown;
    }

private:
    struct Mailbox {
        std::atomic<bool> changed{false};
        std::atomic<bool> rebind{false};
    };
    struct Listener {
        AudioObjectID object;
        AudioObjectPropertyAddress property;
        bool topology;
    };

    void listen(AudioObjectID object, const AudioObjectPropertyAddress& property,
                bool topology)
    {
        if (!AudioObjectHasProperty(object, &property)) return;
        const auto existing = std::find_if(m_listeners.begin(), m_listeners.end(),
            [&](const Listener& listener) {
                return listener.object == object
                    && listener.property.mSelector == property.mSelector
                    && listener.property.mScope == property.mScope
                    && listener.property.mElement == property.mElement;
            });
        if (existing != m_listeners.end()) return;
        if (AudioObjectAddPropertyListenerBlock(object, &property, callbackQueue(),
                topology ? m_topologyListener : m_volumeListener) == noErr) {
            m_listeners.push_back({object, property, topology});
        }
        // Failed registrations remain absent and are retried by the watchdog.
    }

    void removeListeners(bool deviceOnly)
    {
        for (auto it = m_listeners.begin(); it != m_listeners.end();) {
            if (deviceOnly && it->object == kAudioObjectSystemObject) {
                ++it;
                continue;
            }
            AudioObjectRemovePropertyListenerBlock(it->object, &it->property, callbackQueue(),
                it->topology ? m_topologyListener : m_volumeListener);
            it = m_listeners.erase(it);
        }
    }

    static dispatch_queue_t callbackQueue()
    {
        return dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);
    }

    void refresh(bool rebind)
    {
        if (!m_started) return;
        if (rebind) removeListeners(false);
        listen(kAudioObjectSystemObject, address(kAudioHardwarePropertyDefaultOutputDevice), true);
        listen(kAudioObjectSystemObject, address(kAudioHardwarePropertyDevices), true);
        listen(kAudioObjectSystemObject, address(kAudioHardwarePropertyServiceRestarted), true);

        AudioDeviceID device = kAudioObjectUnknown;
        if (!readProperty(kAudioObjectSystemObject,
                          address(kAudioHardwarePropertyDefaultOutputDevice), &device)) {
            device = kAudioObjectUnknown;
        }
        if (device != m_device) {
            removeListeners(true);
            m_device = device;
        }
        if (m_device == kAudioObjectUnknown) {
            publish(-1);
            return;
        }

        listen(m_device, address(kAudioDevicePropertyDeviceIsAlive), true);
        listen(m_device, address(kAudioObjectPropertyControlList), true);
        listen(m_device, address(kAudioDevicePropertyStreamConfiguration,
                                  kAudioObjectPropertyScopeOutput), true);
        listen(m_device, address(kAudioDevicePropertyPreferredChannelsForStereo,
                                  kAudioObjectPropertyScopeOutput), true);
        UInt32 alive = 0;
        if (!readProperty(m_device, address(kAudioDevicePropertyDeviceIsAlive), &alive) || !alive) {
            publish(-1);
            return;
        }

        const auto master = address(kAudioDevicePropertyVolumeScalar,
                                    kAudioObjectPropertyScopeOutput);
        listen(m_device, master, false);
        Float32 scalar = -1;
        if (!readProperty(m_device, master, &scalar) || !std::isfinite(scalar)) {
            // Devices without a master control expose separate channel volumes.
            // The louder stereo channel gives the slider level independently
            // of balance (averaging would incorrectly lower a panned volume).
            scalar = -1;
            for (UInt32 channel : outputChannels(m_device)) {
                const auto property = address(kAudioDevicePropertyVolumeScalar,
                                              kAudioObjectPropertyScopeOutput, channel);
                listen(m_device, property, false);
                Float32 channelScalar = -1;
                if (readProperty(m_device, property, &channelScalar)
                    && std::isfinite(channelScalar) && channelScalar >= 0) {
                    scalar = std::max(scalar, channelScalar);
                }
            }
        }
        // Preserve the existing slider-level schema: mute is not a volume
        // change, and fixed-volume outputs without a readable control are -1.
        publish(scalar < 0 ? -1 : static_cast<int>(
            std::lround(std::clamp(scalar, Float32{0}, Float32{1}) * 100)));
    }

    QTimer m_events;
    QTimer m_recovery;
    bool m_started = false;
    AudioDeviceID m_device = kAudioObjectUnknown;
    std::shared_ptr<Mailbox> m_mailbox;
    AudioObjectPropertyListenerBlock m_volumeListener = nullptr;
    AudioObjectPropertyListenerBlock m_topologyListener = nullptr;
    std::vector<Listener> m_listeners;
};
}

std::unique_ptr<SystemVolumeMonitorBackend>
createSystemVolumeMonitorBackend(SystemVolumeMonitorBackend::Publish publish)
{
    return std::make_unique<MacSystemVolumeMonitorBackend>(std::move(publish));
}
