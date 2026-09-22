#pragma once
#include <QAudioDevice>
#include <QAudioFormat>
#include <algorithm>
#include <array>

// Select a format before opening the sink. Some devices advertise a mono
// preferred format even though they accept stereo at another sample rate.
// Exhaust the usual stereo choices before accepting a native mono layout.
template<class Supports>
QAudioFormat chooseStereoOutputFormat(const QAudioFormat& preferred, Supports&& supported) {
    const std::array<int, 8> rates{48000, preferred.sampleRate(), 44100, 96000, 88200, 32000, 24000, 192000};
    for (auto rate = rates.begin(); rate != rates.end(); ++rate) {
        if (*rate <= 0 || std::find(rates.begin(), rate, *rate) != rate) continue;
        QAudioFormat stereo;
        stereo.setSampleRate(*rate);
        stereo.setChannelConfig(QAudioFormat::ChannelConfigStereo);
        stereo.setSampleFormat(QAudioFormat::Float);
        if (supported(stereo)) return stereo;
    }
    auto native = preferred;
    native.setSampleFormat(QAudioFormat::Float);
    return native.isValid() && supported(native) ? native : QAudioFormat{};
}

inline QAudioFormat chooseStereoOutputFormat(const QAudioDevice& device) {
    if (device.isNull()) return {};
    auto preferred = device.preferredFormat();
    // Device-level speaker positions are useful when preferredFormat only
    // reports a channel count. Do not replace a known per-format layout.
    if (preferred.channelConfig() == QAudioFormat::ChannelConfigUnknown
        && device.channelConfiguration() != QAudioFormat::ChannelConfigUnknown) {
        auto described = preferred;
        described.setChannelConfig(device.channelConfiguration());
        if (described.channelCount() == preferred.channelCount()) preferred = described;
    }
    return chooseStereoOutputFormat(preferred, [&device](const QAudioFormat& format) {
        return device.isFormatSupported(format);
    });
}

// Canonical stereo remains intact up to the final hardware write. Resolve
// speaker positions once on the control thread, never inside the audio loop.
class AudioStereoOutputMapping {
public:
    AudioStereoOutputMapping() = default;
    explicit AudioStereoOutputMapping(QAudioFormat format) : m_channels(format.channelCount()) {
        if (m_channels <= 0 || m_channels >= QAudioFormat::NChannelPositions) return;
        if (format.channelConfig() == QAudioFormat::ChannelConfigUnknown)
            format.setChannelConfig(QAudioFormat::defaultChannelConfigForChannelCount(m_channels));
        m_left = format.channelOffset(QAudioFormat::FrontLeft);
        m_right = format.channelOffset(QAudioFormat::FrontRight);
        m_valid = m_channels == 1 || (m_left >= 0 && m_left < m_channels && m_right >= 0 && m_right < m_channels);
    }
    int channels() const noexcept { return m_channels; }
    bool isValid() const noexcept { return m_valid; }
    void add(float* deviceFrame, float left, float right) const noexcept {
        if (!m_valid) return;
        if (m_channels == 1) deviceFrame[0] += 0.5f * (left + right);
        else { deviceFrame[m_left] += left; deviceFrame[m_right] += right; }
    }

private:
    int m_channels = 0;
    int m_left = -1, m_right = -1;
    bool m_valid = false;
};
