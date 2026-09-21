#include "backend/audiosharing/AudioStreamCodec.h"
#include <opus.h>

struct AudioStreamEncoder::Private { OpusEncoder* encoder = nullptr; ~Private() { if (encoder) opus_encoder_destroy(encoder); } };
AudioStreamEncoder::AudioStreamEncoder() : d(std::make_unique<Private>()) {}
AudioStreamEncoder::~AudioStreamEncoder() = default;
bool AudioStreamEncoder::initialize(int bitrateBps, QString& error) {
    error.clear();
    if (!d->encoder) {
        int result = 0; d->encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &result);
        if (!d->encoder) { error = QString::fromUtf8(opus_strerror(result)); return false; }
        opus_encoder_ctl(d->encoder, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(d->encoder, OPUS_SET_VBR(1));
        opus_encoder_ctl(d->encoder, OPUS_SET_VBR_CONSTRAINT(1));
        opus_encoder_ctl(d->encoder, OPUS_SET_DTX(1));
    }
    setBitrate(bitrateBps); return true;
}
void AudioStreamEncoder::setBitrate(int bitrateBps) { if (d->encoder) opus_encoder_ctl(d->encoder, OPUS_SET_BITRATE(bitrateBps <= 32000 ? 32000 : 96000)); }
QByteArray AudioStreamEncoder::encode(const float* samples, QString& error) {
    error.clear();
    if (!d->encoder || !samples) { error = QStringLiteral("Audio encoder is not initialized"); return {}; }
    QByteArray packet(1275, Qt::Uninitialized);
    const int size = opus_encode_float(d->encoder, samples, 960, reinterpret_cast<unsigned char*>(packet.data()), packet.size());
    if (size < 0) { error = QString::fromUtf8(opus_strerror(size)); return {}; }
    packet.resize(size); return packet;
}
void AudioStreamEncoder::reset() { if (d->encoder) opus_encoder_ctl(d->encoder, OPUS_RESET_STATE); }
struct AudioStreamDecoder::Private { OpusDecoder* decoder = nullptr; ~Private() { if (decoder) opus_decoder_destroy(decoder); } };
AudioStreamDecoder::AudioStreamDecoder() : d(std::make_unique<Private>()) {}
AudioStreamDecoder::~AudioStreamDecoder() = default;
QByteArray AudioStreamDecoder::decode(const QByteArray& packet, QString& error) {
    error.clear();
    if (packet.isEmpty() || packet.size() > 1275 || opus_packet_get_nb_samples(
        reinterpret_cast<const unsigned char*>(packet.constData()), packet.size(), 48000) != 960) {
        error = QStringLiteral("Invalid 20 ms Opus packet"); return {};
    }
    if (!d->decoder) {
        int result = 0; d->decoder = opus_decoder_create(48000, 2, &result);
        if (!d->decoder) { error = QString::fromUtf8(opus_strerror(result)); return {}; }
    }
    QByteArray samples(960 * 2 * sizeof(float), Qt::Uninitialized);
    const int frames = opus_decode_float(d->decoder, reinterpret_cast<const unsigned char*>(packet.constData()),
        packet.size(), reinterpret_cast<float*>(samples.data()), 960, 0);
    if (frames != 960) { error = frames < 0 ? QString::fromUtf8(opus_strerror(frames)) : QStringLiteral("Unexpected audio frame duration"); return {}; }
    return samples;
}
void AudioStreamDecoder::reset() { if (d->decoder) opus_decoder_ctl(d->decoder, OPUS_RESET_STATE); }
