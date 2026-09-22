#pragma once

#include "backend/screensharing/ScreenCaptureError.h"
#include "backend/screensharing/ScreenStreamCodec.h"
#include <QVideoFrame>
#include <functional>
#include <memory>

class QScreen;

// Windows Graphics Capture fallback for drivers/session types that reject
// DXGI desktop duplication. Native objects stay on one MTA worker; callbacks
// feed the same bounded encoder mailbox as the primary backend.
class WindowsScreenCapture final {
public:
    using FrameCallback = std::function<void(const QVideoFrame&)>;
    using ErrorCallback = std::function<void(ScreenCaptureError, const QString&)>;
    WindowsScreenCapture();
    ~WindowsScreenCapture();
    bool start(QScreen* screen, FrameCallback frame, ErrorCallback error);
    void stop();
    bool isActive() const;
    void setProfile(const ScreenStreamProfile& profile);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
