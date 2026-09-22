#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>

class SystemAudioCapture {
public:
    using Pcm = std::function<void(QByteArray, qint64)>;
    using State = std::function<void(bool, QString)>;
    virtual ~SystemAudioCapture() = default;
    virtual void start(Pcm pcm, State state) = 0;
    virtual void stop() = 0;
};
// Native capture returns unattenuated 48 kHz stereo float. The remote output
// device's volume/mute is not reapplied; the worker's own process is excluded.
std::unique_ptr<SystemAudioCapture> createSystemAudioCapture();
// Keep the auxiliary process invisible in the Dock and application switcher.
void initializeAudioWorkerPlatform();
