#include "shared/rendering/MediaFrameSource.h"

RemoteVideoFrameSource::RemoteVideoFrameSource(QObject* parent)
    : QObject(parent) {
}

void RemoteVideoFrameSource::setFrame(const QImage& frame) {
    if (frame.isNull()) {
        clear();
        return;
    }
    // Residency updates can republish the same immutable image when another
    // owner joins its asset. Keep those updates from invalidating GPU textures.
    if (m_frame.cacheKey() == frame.cacheKey()) return;
    const bool wasEmpty = m_frame.isNull();
    m_frame = frame;
    emit frameChanged();
    if (wasEmpty) emit hasFrameChanged();
}

void RemoteVideoFrameSource::clear() {
    if (m_frame.isNull()) {
        return;
    }
    m_frame = QImage();
    emit frameChanged();
    emit hasFrameChanged();
}
