#pragma once

#include <QAbstractVideoBuffer>

struct AVFrame;

// Optional zero-copy input shared by the native capture adapter and encoder.
// Returned frames own their native surface and must be freed with av_frame_free.
// The ordinary map() path remains available for software encoder fallback.
class ScreenCaptureVideoBuffer : public QAbstractVideoBuffer {
public:
    virtual AVFrame* nativeEncoderFrame() const { return nullptr; }
};

// Qt exposes construction of custom video buffers publicly, but retrieving
// that buffer uses a deprecated accessor. Isolate this one adapter alongside
// the existing scene-graph buffer adapter; this project pins Qt 6.11.2.
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
inline AVFrame* screenCaptureNativeFrame(const QVideoFrame& frame) {
    const auto* native = dynamic_cast<ScreenCaptureVideoBuffer*>(frame.videoBuffer());
    return native ? native->nativeEncoderFrame() : nullptr;
}
QT_WARNING_POP
