#include "frontend/rendering/canvas/PointerSession.h"

bool PointerSession::draggingMedia() const {
    return m_draggingMedia;
}

void PointerSession::setDraggingMedia(bool dragging) {
    m_draggingMedia = dragging;
}

bool PointerSession::resizeActive() const {
    return m_resizeActive;
}

QString PointerSession::resizeMediaId() const {
    return m_resizeMediaId;
}

QString PointerSession::resizeHandleId() const {
    return m_resizeHandleId;
}

void PointerSession::beginResize(const QString& mediaId, const QString& handleId) {
    m_resizeActive = true;
    m_resizeMediaId = mediaId;
    m_resizeHandleId = handleId;
}

void PointerSession::clearResize() {
    m_resizeActive = false;
    m_resizeMediaId.clear();
    m_resizeHandleId.clear();
}
