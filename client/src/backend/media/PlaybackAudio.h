#pragma once
#include "backend/media/ResidentMediaAsset.h"
#include <QObject>
#include <memory>
class QAudioOutput;
// A control cursor and 200 ms of PCM; the device and callback are shared.
class PlaybackAudio final : public QObject {
    Q_OBJECT
public:
    explicit PlaybackAudio(QObject* parent = nullptr);
    ~PlaybackAudio() override;
    void setAsset(std::shared_ptr<const ResidentMediaAsset> asset);
    void setOutput(QAudioOutput* output);
    void prepare(qint64 positionUs);
    bool preparedAt(qint64 positionUs) const;
    void play(qint64 positionUs);
    void pause();
    bool presentedSincePlay() const; // Callback consumption, not physical DAC latency.
    static quint64 bufferBytes();
    static quint64 decodeRequestCount(); // Submitted PCM jobs, including muted preparation.
    static int deviceCount();
signals:
    void prepared();
    void failed(const QString& error);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
    void refill();
    void rebuildOutput();
};
