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
    void requestKeyFrame();

signals:
    void packetReady(const ScreenStreamPacket& packet);
    void errorOccurred(ScreenCaptureError code, const QString& message);
    void backendChanged(const QString& backend);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
