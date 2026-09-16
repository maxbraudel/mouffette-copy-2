#include "shared/rendering/MediaFrameSource.h"

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

