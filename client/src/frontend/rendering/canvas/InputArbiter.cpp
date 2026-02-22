#include "frontend/rendering/canvas/InputArbiter.h"

bool InputArbiter::isIdle() const {
    return m_mode == Mode::Idle;
}

InputArbiter::Mode InputArbiter::mode() const {
    return m_mode;
}

bool InputArbiter::beginPan() {
    return begin(Mode::Pan, QString(), QString());
}

bool InputArbiter::beginMove(const QString& mediaId) {
    if (mediaId.isEmpty()) {
        return false;
    }
    return begin(Mode::Move, mediaId, QString());
}

bool InputArbiter::beginResize(const QString& mediaId, const QString& handleId) {
    if (mediaId.isEmpty() || handleId.isEmpty()) {
        return false;
    }
    return begin(Mode::Resize, mediaId, handleId);
}

bool InputArbiter::beginTextCreate() {
    return begin(Mode::TextCreate, QString(), QString());
}

bool InputArbiter::endPan() {
    if (m_mode != Mode::Pan) {
        return false;
    }
    reset();
    return true;
}

bool InputArbiter::endMove(const QString& mediaId) {
    if (m_mode != Mode::Move || mediaId.isEmpty() || m_mediaId != mediaId) {
        return false;
    }
    reset();
    return true;
}

bool InputArbiter::endResize(const QString& mediaId, const QString& handleId) {
    if (m_mode != Mode::Resize || mediaId.isEmpty() || m_mediaId != mediaId) {
        return false;
    }
    if (!handleId.isEmpty() && m_handleId != handleId) {
        return false;
    }
    reset();
    return true;
}

bool InputArbiter::endTextCreate() {
    if (m_mode != Mode::TextCreate) {
        return false;
    }
    reset();
    return true;
}

bool InputArbiter::isSameResizeSession(const QString& mediaId, const QString& handleId) const {
    return m_mode == Mode::Resize && m_mediaId == mediaId && m_handleId == handleId;
}

QString InputArbiter::activeMediaId() const {
    return m_mediaId;
}

QString InputArbiter::activeHandleId() const {
    return m_handleId;
}

void InputArbiter::reset() {
    m_mode = Mode::Idle;
    m_mediaId.clear();
    m_handleId.clear();
}

bool InputArbiter::begin(Mode requestedMode, const QString& mediaId, const QString& handleId) {
    if (m_mode == Mode::Idle) {
        m_mode = requestedMode;
        m_mediaId = mediaId;
        m_handleId = handleId;
        return true;
    }

    if (requestedMode == Mode::Resize
        && m_mode == Mode::Resize
        && m_mediaId == mediaId
        && m_handleId == handleId) {
        return true;
    }

    if (requestedMode == Mode::Move
        && m_mode == Mode::Move
        && m_mediaId == mediaId) {
        return true;
    }

    return false;
}
