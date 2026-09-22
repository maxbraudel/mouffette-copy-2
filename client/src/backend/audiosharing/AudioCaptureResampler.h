#pragma once
#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>

// One instance per independent capture clock. Native timestamps establish the
// epoch and slowly correct clock drift; they never insert holes into otherwise
// continuous PCM. Processing belongs to the application thread, not an audio
// device callback. Both input and output are interleaved 48 kHz stereo float.
class AudioCaptureResampler {
public:
    static constexpr int Rate = 48000, Channels = 2, MaximumFrames = 4800;
    struct Result { QByteArray pcm; qint64 timestampUs; bool discontinuity; };
    AudioCaptureResampler();
    ~AudioCaptureResampler();
    AudioCaptureResampler(const AudioCaptureResampler&) = delete;
    AudioCaptureResampler& operator=(const AudioCaptureResampler&) = delete;

    std::optional<Result> append(QByteArray pcm, qint64 timestampUs, bool discontinuity = false);
    void reset();
    QString errorString() const;
private:
    struct Private;
    std::unique_ptr<Private> d;
};
