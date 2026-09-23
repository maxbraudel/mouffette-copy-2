#pragma once

#include "backend/screensharing/ScreenCaptureError.h"
#include "backend/screensharing/ScreenStreamCodec.h"
#include <QVideoFrame>
#include <QString>
#include <functional>
#include <memory>
#include <CoreVideo/CVPixelBuffer.h>

class QScreen;

// Main-thread lifecycle; native callbacks deliver directly to the encoder's
// bounded mailbox. No screen pixels cross the Qt GUI event queue.
// Display filters exclude this entire process, including its scene surfaces.
// Audio uses an independent capture with the same process exclusion policy.
class MacScreenCapture final {
public:
    using FrameCallback = std::function<void(const QVideoFrame&)>;
    using ErrorCallback = std::function<void(ScreenCaptureError, const QString&)>;
    MacScreenCapture();
    ~MacScreenCapture();
    bool start(QScreen* screen, FrameCallback frame, ErrorCallback error);
    void stop();
    bool isActive() const;
    void setProfile(const ScreenStreamProfile& profile);
    // Retains a native NV12 surface without copying. Shared by capture and
    // synthetic codec tests, which never request access to the desktop.
    static QVideoFrame frameFromPixelBuffer(CVPixelBufferRef buffer);
    // Native policy is kept pure so permission/status handling can be checked
    // without granting desktop access or constructing a running SCStream.
    enum class SampleAction { Deliver, Ignore, Suspend, Stop };
    static SampleAction sampleAction(int nativeStatus);
    static ScreenCaptureError nativeErrorCode(const QString& domain, qint64 code);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
