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
    void reset();
private:
    struct Private;
    std::unique_ptr<Private> d;
};
class AudioStreamDecoder {
public:
    AudioStreamDecoder();
    ~AudioStreamDecoder();
    QByteArray decode(const QByteArray& opus, QString& error);
    void reset();
private:
    struct Private;
    std::unique_ptr<Private> d;
};
