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
    const bool wasEmpty = !hasFrame();
    m_videoFrame = {};
    m_frame = frame;
    emit frameChanged();
    if (wasEmpty) emit hasFrameChanged();
}

void RemoteVideoFrameSource::clear() {
    if (!hasFrame()) return;
    m_frame = QImage();
    m_videoFrame = {};
    emit frameChanged();
    emit hasFrameChanged();
}

void RemoteVideoFrameSource::setVideoFrame(const QVideoFrame& frame) {
    if (!frame.isValid()) { clear(); return; }
    const bool empty = !hasFrame();
    m_frame = {}; m_videoFrame = frame;
    emit frameChanged();
    if (empty) emit hasFrameChanged();
}
