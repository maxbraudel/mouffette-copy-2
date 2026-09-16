#include "frontend/rendering/remote/RemoteVideoFrameItem.h"

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>
#include <memory>

namespace {
// The scene graph owns all graphics resources, including the content key.
// A new node after window/context recreation must upload the frame again.
class FrameNode final : public QSGNode {
public:
    explicit FrameNode(QSGImageNode* imageNode) : image(imageNode) {
        image->setFiltering(QSGTexture::Linear);
        appendChildNode(image);
    }

    ~FrameNode() override {
        // Destroy the material before its texture, on the render thread.
        delete image;
    }

    QSGImageNode* image;
    std::unique_ptr<QSGTexture> texture;
    qint64 frameKey = 0;
};
}

RemoteVideoFrameItem::RemoteVideoFrameItem(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

void RemoteVideoFrameItem::setFrameSource(QObject* source) {
    auto* typedSource = qobject_cast<RemoteVideoFrameSource*>(source);
    if (typedSource == m_source) {
        return;
    }

    QObject::disconnect(m_frameConnection);
    QObject::disconnect(m_destroyedConnection);
    m_source = typedSource;
    if (m_source) {
        m_frameConnection = connect(m_source, &RemoteVideoFrameSource::frameChanged,
                                    this, &RemoteVideoFrameItem::refreshFrame);
        m_destroyedConnection = connect(m_source, &QObject::destroyed, this, [this]() {
            m_source = nullptr;
            refreshFrame();
            emit frameSourceChanged();
        });
    }
    refreshFrame();
    emit frameSourceChanged();
}

bool RemoteVideoFrameItem::hasFrame() const {
    return m_source && m_source->hasFrame();
}

void RemoteVideoFrameItem::refreshFrame() {
    update();
    const bool ready = hasFrame();
    if (m_hasFrame != ready) {
        m_hasFrame = ready;
        emit hasFrameChanged();
    }
}

void RemoteVideoFrameItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update();
}

QSGNode* RemoteVideoFrameItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<FrameNode*>(oldNode);
    if (!window() || !hasFrame() || width() <= 0 || height() <= 0) {
        delete node;
        return nullptr;
    }

    // The GUI thread is blocked during synchronization; the shared immutable
    // frame can be read here without copying or retaining it on the item.
    const QImage& frame = m_source->frame();
    if (!node || node->frameKey != frame.cacheKey()) {
        std::unique_ptr<QSGTexture> texture(window()->createTextureFromImage(frame));
        if (!texture) {
            delete node;
            return nullptr;
        }
        if (!node)
            node = new FrameNode(window()->createImageNode());
        node->image->setTexture(texture.get());
        // The default empty source rect means the entire texture, including
        // when Qt limits an oversized source to the GPU's maximum dimensions.
        node->texture = std::move(texture);
        node->frameKey = frame.cacheKey();
    }

    // Resize, Alt-resize and zoom change only this quad/the parent transform.
    // In particular, no canvas-sized QImage, paint pass or texture upload.
    node->image->setRect(boundingRect());
    return node;
}
