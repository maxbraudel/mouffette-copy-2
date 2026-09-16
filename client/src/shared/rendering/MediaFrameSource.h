#pragma once

#include <QObject>
#include <QImage>

// Shared immutable CPU frame projection; independent from Qt Quick.
class RemoteVideoFrameSource final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged)

public:
    explicit RemoteVideoFrameSource(QObject* parent = nullptr);

    bool hasFrame() const { return !m_frame.isNull(); }
    const QImage& frame() const { return m_frame; }

    void setFrame(const QImage& frame);
    Q_INVOKABLE void clear();

signals:
    void frameChanged();
    void hasFrameChanged();

private:
    QImage m_frame;
};
