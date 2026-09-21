#pragma once

#include "backend/screensharing/ScreenStreamCodec.h"
#include "backend/screensharing/ScreenCaptureError.h"
#include <QObject>
#include <memory>

class QScreen;

// GUI-thread facade. ScreenCaptureKit on macOS, Qt's DXGI capture on Windows;
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
    void setProfile(const ScreenStreamProfile& profile);
    void setBackpressured(bool backpressured);
    void requestKeyFrame();

signals:
    void packetReady(const ScreenStreamPacket& packet);
    void errorOccurred(ScreenCaptureError code, const QString& message);
    void backendChanged(const QString& backend);
    // Worker conversion/encoder-call time; delivered with the existing packet
    // callback, excluding capture, network and asynchronous GPU completion.
    void encodingMeasured(int elapsedMs);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
