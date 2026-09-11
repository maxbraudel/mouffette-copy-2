#include "frontend/rendering/remote/RemoteVideoFrameItem.h"

#include <QPainter>

RemoteVideoFrameSource::RemoteVideoFrameSource(QObject* parent)
    : QObject(parent) {
}

void RemoteVideoFrameSource::setFrame(const QImage& frame) {
    if (frame.isNull()) {
        clear();
        return;
    }
    m_frame = frame;
    emit frameChanged();
}

void RemoteVideoFrameSource::clear() {
    if (m_frame.isNull()) {
        return;
    }
    m_frame = QImage();
    emit frameChanged();
}

RemoteVideoFrameItem::RemoteVideoFrameItem(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(false);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
}

void RemoteVideoFrameItem::setFrameSource(QObject* source) {
    auto* typedSource = qobject_cast<RemoteVideoFrameSource*>(source);
    if (typedSource == m_source) {
        return;
    }

    QObject::disconnect(m_frameConnection);
    m_source = typedSource;
    if (m_source) {
        m_frameConnection = connect(m_source, &RemoteVideoFrameSource::frameChanged,
                                    this, [this]() {
            update();
            emit hasFrameChanged();
        });
    }
    update();
    emit frameSourceChanged();
    emit hasFrameChanged();
}

bool RemoteVideoFrameItem::hasFrame() const {
    return m_source && m_source->hasFrame();
}

void RemoteVideoFrameItem::paint(QPainter* painter) {
    if (!painter || !m_source || !m_source->hasFrame()) {
        return;
    }
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(boundingRect(), m_source->frame());
}
