#include "SceneAudioTap.h"
#include <algorithm>
#include <cstring>
extern "C" {
#include <libswresample/swresample.h>
}

struct SceneAudioTap::Converter {
    SwrContext* context = nullptr;
    int rate = 0, channels = 0;
    qint64 endUs = -1;
    ~Converter() { swr_free(&context); }
};
SceneAudioTap::SceneAudioTap() : converter(std::make_unique<Converter>()) {}
SceneAudioTap::~SceneAudioTap() = default;
void SceneAudioTap::setEnabled(bool enabled) {
    epoch.store(enabled ? ++serial : 0, std::memory_order_release);
    converter = std::make_unique<Converter>();
}
void SceneAudioTap::push(const float* pcm, int frames, int channels, int rate, qint64 timestampUs) {
    const auto generation = epoch.load(std::memory_order_acquire);
    if (!generation || !pcm || channels < 1 || channels > MaximumChannels || rate < 8000) return;
    for (int offset = 0; offset < frames; offset += BlockFrames) {
        const auto index = write.load(std::memory_order_relaxed);
        if (index - read.load(std::memory_order_acquire) >= Blocks) return;
        auto& block = blocks[size_t(index % Blocks)];
        block.frames = std::min(BlockFrames, frames - offset);
        block.channels = channels; block.rate = rate; block.epoch = generation;
        block.timestampUs = timestampUs + qint64(offset) * 1000000 / rate;
        std::memcpy(block.pcm.data(), pcm + offset * channels, size_t(block.frames * channels) * sizeof(float));
        write.store(index + 1, std::memory_order_release);
    }
}
bool SceneAudioTap::take(QByteArray& stereo, qint64& timestampUs) {
    while (true) {
        const auto index = read.load(std::memory_order_relaxed);
        if (index == write.load(std::memory_order_acquire)) return false;
        const auto& block = blocks[size_t(index % Blocks)];
        if (block.epoch != epoch.load(std::memory_order_acquire) || !block.epoch) {
            read.store(index + 1, std::memory_order_release); continue;
        }
        timestampUs = block.timestampUs;
        if (block.rate == 48000 && block.channels == 2) {
            stereo = QByteArray(reinterpret_cast<const char*>(block.pcm.data()), block.frames * 2 * sizeof(float));
        } else {
            auto& c = *converter;
            if (!c.context || c.rate != block.rate || c.channels != block.channels
                || (c.endUs >= 0 && std::abs(block.timestampUs - c.endUs) > 2000)) {
                swr_free(&c.context);
                AVChannelLayout input{}, output = AV_CHANNEL_LAYOUT_STEREO;
                av_channel_layout_default(&input, block.channels);
                const int result = swr_alloc_set_opts2(&c.context, &output, AV_SAMPLE_FMT_FLT, 48000,
                    &input, AV_SAMPLE_FMT_FLT, block.rate, 0, nullptr);
                av_channel_layout_uninit(&input);
                if (result < 0 || swr_init(c.context) < 0) swr_free(&c.context);
                c.rate = block.rate; c.channels = block.channels;
            }
            stereo.clear();
            if (c.context) {
                timestampUs -= swr_get_delay(c.context, 1000000);
                stereo.resize(swr_get_out_samples(c.context, block.frames) * 2 * sizeof(float));
                auto* output = reinterpret_cast<uint8_t*>(stereo.data());
                const auto* input = reinterpret_cast<const uint8_t*>(block.pcm.data());
                const int frames = swr_convert(c.context, &output, int(stereo.size() / (2 * sizeof(float))), &input, block.frames);
                stereo.resize(std::max(0, frames) * 2 * sizeof(float));
            }
            c.endUs = block.timestampUs + qint64(block.frames) * 1000000 / block.rate;
        }
        read.store(index + 1, std::memory_order_release);
        if (!stereo.isEmpty()) return true;
    }
}
SceneAudioBus& SceneAudioBus::instance() { static SceneAudioBus bus; return bus; }
std::shared_ptr<SceneAudioTap> SceneAudioBus::createTap() {
    sources();
    auto tap = std::make_shared<SceneAudioTap>(); tap->setEnabled(enabled);
    taps.push_back(tap); return tap;
}
void SceneAudioBus::setEnabled(bool next) {
    enabled = next;
    for (const auto& tap : sources()) tap->setEnabled(enabled);
}
std::vector<std::shared_ptr<SceneAudioTap>> SceneAudioBus::sources() {
    std::vector<std::shared_ptr<SceneAudioTap>> result;
    for (auto it = taps.begin(); it != taps.end();) {
        if (auto tap = it->lock()) { result.push_back(std::move(tap)); ++it; }
        else it = taps.erase(it);
    }
    return result;
}
