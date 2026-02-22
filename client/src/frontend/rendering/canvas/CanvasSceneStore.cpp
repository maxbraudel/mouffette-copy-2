#include "CanvasSceneStore.h"

CanvasSceneStore::CanvasSceneStore(QObject* parent)
    : QObject(parent)
{
}

void CanvasSceneStore::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    emit screensChanged();
}

QList<ScreenInfo> CanvasSceneStore::screens() const {
    return m_screens;
}

void CanvasSceneStore::setSceneScreenRects(const QHash<int, QRectF>& rects) {
    m_sceneScreenRects = rects;
}

QHash<int, QRectF> CanvasSceneStore::sceneScreenRects() const {
    return m_sceneScreenRects;
}

void CanvasSceneStore::setRemoteCursor(bool visible, qreal x, qreal y) {
    if (m_remoteCursorVisible != visible || m_remoteCursorPos != QPointF(x, y)) {
        m_remoteCursorVisible = visible;
        m_remoteCursorPos = QPointF(x, y);
        emit remoteCursorChanged();
    }
}

bool CanvasSceneStore::remoteCursorVisible() const {
    return m_remoteCursorVisible;
}

QPointF CanvasSceneStore::remoteCursorPos() const {
    return m_remoteCursorPos;
}

void CanvasSceneStore::setSceneUnitScale(qreal scale) {
    if (!qFuzzyCompare(m_sceneUnitScale, scale)) {
        m_sceneUnitScale = scale;
        emit sceneUnitScaleChanged();
    }
}

qreal CanvasSceneStore::sceneUnitScale() const {
    return m_sceneUnitScale;
}
