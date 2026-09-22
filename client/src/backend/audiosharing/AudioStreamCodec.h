#pragma once
#include <QByteArray>
#include <QString>
#include <memory>

class AudioStreamEncoder {
public:
    AudioStreamEncoder();
    ~AudioStreamEncoder();
    bool initialize(int bitrateBps, QString& error);
    void setBitrate(int bitrateBps);
    QByteArray encode(const float* stereo960, QString& error);
    int lookaheadSamples() const;
    qint64 lookaheadUs() const { return qint64(lookaheadSamples()) * 1000000 / 48000; }
    void reset();
private:
    struct Private;
    std::unique_ptr<Private> d;
};
class AudioStreamDecoder {
public:
    AudioStreamDecoder();
    ~AudioStreamDecoder();
    QByteArray decode(const QByteArray& opus, QString& error, bool fec = false);
    // Advance exactly one missing 20 ms frame in the stateful Opus decoder.
    QByteArray conceal(QString& error);
    void reset();
private:
    struct Private;
    std::unique_ptr<Private> d;
};
