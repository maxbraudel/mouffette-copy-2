#pragma once

#include <QVideoSink>

#include "shared/rendering/MediaFrameSource.h"
#include <QPointer>
#include <QQuickItem>

// Scene-graph projection of a resident image or shared remote video frame.
// Texture storage follows the source pixels, never the canvas geometry or DPR.
// Keep the existing QML name because both local images and remote video use it.
class RemoteVideoFrameItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject* frameSource READ frameSource WRITE setFrameSource NOTIFY frameSourceChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged)
    Q_PROPERTY(QVideoSink* videoSink READ videoSink CONSTANT)

public:
    explicit RemoteVideoFrameItem(QQuickItem* parent = nullptr);

    QObject* frameSource() const { return m_source.data(); }
    void setFrameSource(QObject* source);
    bool hasFrame() const;
    QVideoSink* videoSink() { return &m_sink; }

signals:
    void frameSourceChanged();
    void hasFrameChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void refreshFrame();

    QVideoSink m_sink;
    QPointer<RemoteVideoFrameSource> m_source;
    QMetaObject::Connection m_frameConnection;
    QMetaObject::Connection m_destroyedConnection;
    bool m_hasFrame = false;
};
