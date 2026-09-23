#include "AudioCaptureResampler.h"
#include <algorithm>
#include <cmath>
extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

struct AudioCaptureResampler::Private {
    SwrContext* context = nullptr;
    QString error;
    qint64 anchorUs = -1, anchorFrame = 0, outputFrames = 0;
    qint64 lastStartUs = -1, lastEndUs = -1;
    double filteredErrorFrames = 0;
    int framesSinceCorrection = 0;
    bool transition = true;
    ~Private() { swr_free(&context); }
    void clear() {
        swr_free(&context);
        anchorUs = lastStartUs = lastEndUs = -1;
        anchorFrame = outputFrames = 0; filteredErrorFrames = 0; framesSinceCorrection = 0;
        transition = true;
    }
    bool check(int result) {
        if (result >= 0) return true;
        char message[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, message, sizeof(message));
        error = QStringLiteral("Audio capture resampling failed: %1").arg(QString::fromUtf8(message));
        clear();
        return false;
    }
    bool initialize(qint64 timestampUs) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (!check(swr_alloc_set_opts2(&context, &stereo, AV_SAMPLE_FMT_FLT, Rate,
                &stereo, AV_SAMPLE_FMT_FLT, Rate, 0, nullptr))) return false;
        // Initialize the interpolation filter before its first sample. Enabling
        // compensation later would otherwise reinitialize a pass-through Swr.
        if (!check(av_opt_set_int(context, "flags", SWR_FLAG_RESAMPLE, 0))
            || !check(swr_init(context))) return false;
        anchorUs = timestampUs;
        // Quantize only the initial epoch onto the common 48 kHz grid. Adding
        // separately rounded microsecond durations to an arbitrary epoch could
        // otherwise alternate between adjacent frames in the capture buffer.
        anchorFrame = (timestampUs / 1000) * 48 + ((timestampUs % 1000) * 48 + 500) / 1000;
        return true;
    }
};

AudioCaptureResampler::AudioCaptureResampler() : d(std::make_unique<Private>()) {}
AudioCaptureResampler::~AudioCaptureResampler() = default;
void AudioCaptureResampler::reset() { d->clear(); d->error.clear(); }
QString AudioCaptureResampler::errorString() const { return d->error; }

std::optional<AudioCaptureResampler::Result> AudioCaptureResampler::append(
    QByteArray pcm, qint64 timestampUs, bool discontinuity) {
    constexpr int frameBytes = Channels * sizeof(float);
    constexpr qint64 jitterToleranceUs = 2000, clockRestartUs = 250000;
    d->error.clear();
    if (pcm.isEmpty() || pcm.size() % frameBytes || timestampUs < 0) return {};
    // A stalled producer never makes this stage buffer a stale unbounded block.
    if (pcm.size() / frameBytes > MaximumFrames) {
        const auto discarded = pcm.size() / frameBytes - MaximumFrames;
        pcm = pcm.right(MaximumFrames * frameBytes);
        timestampUs += discarded * 1000000 / Rate;
        discontinuity = true;
    }
    int frames = int(pcm.size() / frameBytes);
    if (!discontinuity && d->lastStartUs >= 0) {
        if (timestampUs < d->lastStartUs - clockRestartUs) discontinuity = true;
        else if (timestampUs <= d->lastStartUs) return {}; // Duplicate/stale block.
        else if (d->lastEndUs - timestampUs > jitterToleranceUs) {
            // A partially redelivered native buffer keeps only its new suffix.
            const qint64 overlap = ((d->lastEndUs - timestampUs) * Rate + 500000) / 1000000;
            if (overlap >= frames) return {};
            pcm.remove(0, qsizetype(overlap) * frameBytes);
            frames -= int(overlap);
            timestampUs += overlap * 1000000 / Rate;
        }
        if (!discontinuity && timestampUs - d->lastEndUs > jitterToleranceUs) discontinuity = true;
    }
    if (discontinuity) d->clear();
    if (!d->context && !d->initialize(timestampUs)) return {};

    if (d->lastStartUs >= 0) {
        // The next input sample follows produced output plus the filter's
        // retained tail. Ignoring that tail incorrectly makes the clock servo
        // believe every block is late by the resampler latency.
        const double desiredFrames = double(timestampUs - d->anchorUs) * Rate / 1000000.0;
        const double actualFrames = double(d->outputFrames) + swr_get_delay(d->context, Rate);
        const double alpha = 1.0 - std::exp(-double(frames) / (Rate * 0.5));
        d->filteredErrorFrames += alpha * (desiredFrames - actualFrames - d->filteredErrorFrames);
        d->framesSinceCorrection += frames;
        if (d->framesSinceCorrection >= Rate / 10) {
            // At most 1000 ppm: smoothly stretch/compress real samples rather
            // than periodically dropping/duplicating a sample or inserting zero.
            // One second of compensation also keeps the correction quantization
            // at 21 ppm; refresh every 100 ms without resetting filter history.
            const int delta = int(std::lround(std::clamp(d->filteredErrorFrames, -48.0, 48.0)));
            if (!d->check(swr_set_compensation(d->context, delta, Rate))) return {};
            d->framesSinceCorrection = 0;
        }
    }
    d->lastStartUs = timestampUs;
    d->lastEndUs = timestampUs + qint64(frames) * 1000000 / Rate;
    // Sanitize before the persistent filter: one NaN or near-FLT_MAX value
    // would contaminate later blocks. Keep ample headroom for the capture limiter,
    // while preventing arithmetic overflow on malformed native samples.
    auto* samples = reinterpret_cast<float*>(pcm.data());
    for (int sample = 0; sample < frames * Channels; ++sample)
        samples[sample] = std::isfinite(samples[sample]) ? std::clamp(samples[sample], -16.0f, 16.0f) : 0;
    const int capacity = swr_get_out_samples(d->context, frames);
    if (!d->check(capacity)) return {};
    // With input and output fixed at 48 kHz, any larger delay signals a broken
    // resampler state; do not allocate or retain an unbounded capture backlog.
    if (capacity > MaximumFrames * 2) {
        d->error = QStringLiteral("Audio capture resampler exceeded its bounded buffer");
        d->clear(); return {};
    }
    QByteArray output(capacity * frameBytes, Qt::Uninitialized);
    auto* destination = reinterpret_cast<uint8_t*>(output.data());
    const auto* source = reinterpret_cast<const uint8_t*>(pcm.constData());
    const int produced = swr_convert(d->context, &destination, capacity, &source, frames);
    if (!d->check(produced) || !produced) return {};
    output.resize(produced * frameBytes);
    // First output corresponds to the first input sample. Its availability is
    // delayed by the filter, but its media timestamp must not acquire that delay.
    const qint64 firstFrame = d->anchorFrame + d->outputFrames;
    const qint64 outputTimestampUs = (firstFrame / 48) * 1000 + (firstFrame % 48) * 1000 / 48;
    Result result{std::move(output), outputTimestampUs, d->transition};
    d->outputFrames += produced;
    d->transition = false;
    return result;
}
