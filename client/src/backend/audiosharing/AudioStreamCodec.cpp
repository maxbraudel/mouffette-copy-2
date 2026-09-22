#include "backend/audiosharing/AudioStreamCodec.h"
#include <opus.h>
#include <array>
#include <algorithm>
#include <cmath>

struct AudioStreamEncoder::Private {
    OpusEncoder* encoder = nullptr;
    int lookahead = 0;
    ~Private() { if (encoder) opus_encoder_destroy(encoder); }
};
AudioStreamEncoder::AudioStreamEncoder() : d(std::make_unique<Private>()) {}
AudioStreamEncoder::~AudioStreamEncoder() = default;
bool AudioStreamEncoder::initialize(int bitrateBps, QString& error) {
    error.clear();
    if (!d->encoder) {
        int result = 0;
        d->encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &result);
        if (!d->encoder) { error = QString::fromUtf8(opus_strerror(result)); return false; }
        // Keep music and quiet transients continuous. DTX is intended for voice
        // inactivity; dropping system-audio detail is not an acceptable tradeoff.
        const std::array<int, 7> settings{
            opus_encoder_ctl(d->encoder, OPUS_SET_COMPLEXITY(7)),
            opus_encoder_ctl(d->encoder, OPUS_SET_VBR(1)),
            opus_encoder_ctl(d->encoder, OPUS_SET_VBR_CONSTRAINT(1)),
            opus_encoder_ctl(d->encoder, OPUS_SET_DTX(0)),
            opus_encoder_ctl(d->encoder, OPUS_SET_INBAND_FEC(1)),
            opus_encoder_ctl(d->encoder, OPUS_SET_PACKET_LOSS_PERC(5)),
            opus_encoder_ctl(d->encoder, OPUS_GET_LOOKAHEAD(&d->lookahead))
        };
        for (const int setting : settings) {
            if (setting == OPUS_OK) continue;
            error = QString::fromUtf8(opus_strerror(setting));
            opus_encoder_destroy(d->encoder); d->encoder = nullptr; d->lookahead = 0;
            return false;
        }
    }
    setBitrate(bitrateBps);
    return true;
}
void AudioStreamEncoder::setBitrate(int bitrateBps) {
    if (d->encoder) opus_encoder_ctl(d->encoder, OPUS_SET_BITRATE(bitrateBps <= 32000 ? 32000 : 96000));
}
int AudioStreamEncoder::lookaheadSamples() const { return d->lookahead; }
QByteArray AudioStreamEncoder::encode(const float* samples, QString& error) {
    error.clear();
    if (!d->encoder || !samples) { error = QStringLiteral("Audio encoder is not initialized"); return {}; }
    // One NaN/Inf from a faulty native source can poison the codec's persistent
    // prediction state. Sanitize before it enters Opus, including finite values
    // outside the documented normalized float range.
    std::array<float, 1920> clean;
    for (size_t index = 0; index < clean.size(); ++index)
        clean[index] = std::isfinite(samples[index]) ? std::clamp(samples[index], -1.0f, 1.0f) : 0.0f;
    QByteArray packet(1275, Qt::Uninitialized);
    const int size = opus_encode_float(d->encoder, clean.data(), 960,
        reinterpret_cast<unsigned char*>(packet.data()), int(packet.size()));
    if (size < 0) { error = QString::fromUtf8(opus_strerror(size)); return {}; }
    packet.resize(size);
    return packet;
}
void AudioStreamEncoder::reset() { if (d->encoder) opus_encoder_ctl(d->encoder, OPUS_RESET_STATE); }

struct AudioStreamDecoder::Private {
    OpusDecoder* decoder = nullptr;
    ~Private() { if (decoder) opus_decoder_destroy(decoder); }
    QByteArray decode(const QByteArray* packet, QString& error, bool fec) {
        if (!decoder) {
            int result = 0;
            decoder = opus_decoder_create(48000, 2, &result);
            if (!decoder) { error = QString::fromUtf8(opus_strerror(result)); return {}; }
        }
        QByteArray samples(960 * 2 * sizeof(float), Qt::Uninitialized);
        const auto* data = packet ? reinterpret_cast<const unsigned char*>(packet->constData()) : nullptr;
        const int frames = opus_decode_float(decoder, data, packet ? int(packet->size()) : 0,
            reinterpret_cast<float*>(samples.data()), 960, fec ? 1 : 0);
        if (frames != 960) {
            error = frames < 0 ? QString::fromUtf8(opus_strerror(frames)) : QStringLiteral("Unexpected audio frame duration");
            return {};
        }
        return samples;
    }
};
AudioStreamDecoder::AudioStreamDecoder() : d(std::make_unique<Private>()) {}
AudioStreamDecoder::~AudioStreamDecoder() = default;
QByteArray AudioStreamDecoder::decode(const QByteArray& packet, QString& error, bool fec) {
    error.clear();
    if (packet.isEmpty() || packet.size() > 1275 || opus_packet_get_nb_samples(
        reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), 48000) != 960) {
        error = QStringLiteral("Invalid 20 ms Opus packet");
        return {};
    }
    return d->decode(&packet, error, fec);
}
QByteArray AudioStreamDecoder::conceal(QString& error) {
    error.clear();
    return d->decode(nullptr, error, false);
}
void AudioStreamDecoder::reset() { if (d->decoder) opus_decoder_ctl(d->decoder, OPUS_RESET_STATE); }
