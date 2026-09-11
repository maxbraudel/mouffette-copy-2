#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>

class QPainter;

class RemoteVideoFrameSource final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)

public:
    explicit RemoteVideoFrameSource(QObject* parent = nullptr);

    bool hasFrame() const { return !m_frame.isNull(); }
    const QImage& frame() const { return m_frame; }

    void setFrame(const QImage& frame);
    void clear();

signals:
    void frameChanged();

private:
    QImage m_frame;
};

// Qt Quick surface for a frame source shared by every screen span of one
// remote video. Conversion from QVideoFrame happens once in the controller;
// this item only paints the shared QImage into the scene graph surface.
class RemoteVideoFrameItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QObject* frameSource READ frameSource WRITE setFrameSource NOTIFY frameSourceChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged)

public:
    explicit RemoteVideoFrameItem(QQuickItem* parent = nullptr);

    QObject* frameSource() const { return m_source.data(); }
    void setFrameSource(QObject* source);
    bool hasFrame() const;

    void paint(QPainter* painter) override;

signals:
    void frameSourceChanged();
    void hasFrameChanged();

private:
    QPointer<RemoteVideoFrameSource> m_source;
    QMetaObject::Connection m_frameConnection;
};
