#pragma once

#include <QObject>
#include <QImage>
#include <QVideoFrame>

// Shared immutable CPU frame projection; independent from Qt Quick.
class RemoteVideoFrameSource final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged)

public:
    explicit RemoteVideoFrameSource(QObject* parent = nullptr);

    bool hasFrame() const { return !m_frame.isNull() || m_videoFrame.isValid(); }
    const QImage& frame() const { return m_frame; }

    void setFrame(const QImage& frame);
    void setVideoFrame(const QVideoFrame& frame);
    const QVideoFrame& videoFrame() const { return m_videoFrame; }
    Q_INVOKABLE void clear();

signals:
    void frameChanged();
    void hasFrameChanged();

private:
    QImage m_frame;
    QVideoFrame m_videoFrame;
};
