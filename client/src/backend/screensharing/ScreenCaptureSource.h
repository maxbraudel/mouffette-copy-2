#pragma once

#include "backend/screensharing/ScreenStreamCodec.h"
#include "backend/screensharing/ScreenCaptureError.h"
#include <QHash>
#include <QObject>
#include <memory>

class QScreen;

// GUI-thread facade. ScreenCaptureKit on macOS, DXGI with WGC fallback on Windows;
// conversion and H.264 encoding run on a worker with one pending captured frame.
class ScreenCaptureSource final : public QObject {
    Q_OBJECT
public:
    explicit ScreenCaptureSource(QObject* parent = nullptr);
    ~ScreenCaptureSource() override;
    bool start(const QString& hardwareIdentity);
    bool start(QScreen* screen);
    void stop();
    bool isActive() const;
    // Only main/low are accepted. Both use the same native capture and latest
    // raw surface; adding/removing low preserves main's encoder reference chain.
    void setProfiles(const QHash<QString, ScreenStreamProfile>& profiles);
    void setProfile(const ScreenStreamProfile& profile);
    void setBackpressured(bool backpressured);
    void requestKeyFrame(const QString& layer = {});

signals:
    void packetReady(const ScreenStreamPacket& packet);
    void errorOccurred(ScreenCaptureError code, const QString& message);
    void backendChanged(const QString& backend);
    // A failed encoder is paused until its profile is reapplied or removed.
    // Main also emits EncodingFailed for compatibility with the single layer API.
    void layerEncodingFailed(const QString& layer, const QString& message);
    // Aggregate conversion/encoder-call time for the due layers, delivered with
    // the existing packet callback. Excludes capture, network and asynchronous
    // GPU completion.
    void encodingMeasured(int elapsedMs);

private:
    friend class WindowsCaptureFailoverTest;
#ifdef Q_OS_WIN
    void startWindowsFallback(const QString& dxgiError);
#endif
    struct Private;
    std::unique_ptr<Private> d;
};
