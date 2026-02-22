#include "frontend/rendering/canvas/LegacySceneMirror.h"

#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "backend/domain/media/MediaItems.h"
#include "backend/domain/models/ClientInfo.h"

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QPushButton>
#include <QTransform>
#include <cmath>

LegacySceneMirror::LegacySceneMirror(ScreenCanvas* mediaCanvas, QObject* parent)
    : QObject(parent)
    , m_mediaCanvas(mediaCanvas)
{
    Q_ASSERT(m_mediaCanvas);

    connect(m_mediaCanvas, &ScreenCanvas::mediaItemAdded, this, &LegacySceneMirror::mediaItemAdded);
    connect(m_mediaCanvas, &ScreenCanvas::mediaItemRemoved, this, &LegacySceneMirror::mediaItemRemoved);
    connect(m_mediaCanvas, &ScreenCanvas::remoteSceneLaunchStateChanged,
            this, &LegacySceneMirror::remoteSceneLaunchStateChanged);
    connect(m_mediaCanvas, &ScreenCanvas::textToolActiveChanged,
            this, &LegacySceneMirror::textToolActiveChanged);
}

ScreenCanvas* LegacySceneMirror::mediaCanvas() const {
    return m_mediaCanvas;
}

void LegacySceneMirror::setVisible(bool visible) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setVisible(visible);
    }
}

void LegacySceneMirror::setOverlayViewport(QWidget* viewport) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setOverlayViewport(viewport);
    }
}

void LegacySceneMirror::setActiveIdeaId(const QString& canvasSessionId) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setActiveIdeaId(canvasSessionId);
    }
}

void LegacySceneMirror::setWebSocketClient(WebSocketClient* client) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setWebSocketClient(client);
    }
}

void LegacySceneMirror::setUploadManager(UploadManager* manager) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setUploadManager(manager);
    }
}

void LegacySceneMirror::setFileManager(FileManager* manager) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setFileManager(manager);
    }
}

void LegacySceneMirror::setRemoteSceneTarget(const QString& id, const QString& machineName) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setRemoteSceneTarget(id, machineName);
    }
}

void LegacySceneMirror::updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients) {
    if (m_mediaCanvas) {
        m_mediaCanvas->updateRemoteSceneTargetFromClientList(clients);
    }
}

void LegacySceneMirror::setScreens(const QList<ScreenInfo>& screens) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setScreens(screens);
    }
}

void LegacySceneMirror::hideContentPreservingState() {
    if (m_mediaCanvas) {
        m_mediaCanvas->hideContentPreservingState();
    }
}

void LegacySceneMirror::showContentAfterReconnect() {
    if (m_mediaCanvas) {
        m_mediaCanvas->showContentAfterReconnect();
    }
}

void LegacySceneMirror::setOverlayActionsEnabled(bool enabled) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setOverlayActionsEnabled(enabled);
    }
}

void LegacySceneMirror::handleRemoteConnectionLost() {
    if (m_mediaCanvas) {
        m_mediaCanvas->handleRemoteConnectionLost();
    }
}

void LegacySceneMirror::setViewportUpdateMode(QGraphicsView::ViewportUpdateMode mode) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setViewportUpdateMode(mode);
    }
}

bool LegacySceneMirror::isTextToolActive() const {
    return m_mediaCanvas ? m_mediaCanvas->isTextToolActive() : false;
}

void LegacySceneMirror::createTextAt(const QPointF& scenePos, qreal currentZoomScale) {
    if (!m_mediaCanvas) {
        return;
    }

    if (currentZoomScale > 1e-6 && std::abs(currentZoomScale - 1.0) > 1e-4) {
        m_mediaCanvas->setTransform(QTransform::fromScale(currentZoomScale, currentZoomScale));
    }
    m_mediaCanvas->requestTextMediaCreateAt(scenePos, false);
    m_mediaCanvas->resetTransform();
}

void LegacySceneMirror::requestLocalFileDropAt(const QStringList& localPaths, const QPointF& scenePos) {
    if (m_mediaCanvas) {
        m_mediaCanvas->requestLocalFileDropAt(localPaths, scenePos);
    }
}

QPushButton* LegacySceneMirror::getUploadButton() const {
    return m_mediaCanvas ? m_mediaCanvas->getUploadButton() : nullptr;
}

bool LegacySceneMirror::isRemoteSceneLaunched() const {
    return m_mediaCanvas ? m_mediaCanvas->isRemoteSceneLaunched() : false;
}

QString LegacySceneMirror::overlayDisabledButtonStyle() const {
    return ScreenCanvas::overlayDisabledButtonStyle();
}

QGraphicsScene* LegacySceneMirror::scene() const {
    return m_mediaCanvas ? m_mediaCanvas->scene() : nullptr;
}

void LegacySceneMirror::refreshInfoOverlay() {
    if (m_mediaCanvas) {
        m_mediaCanvas->refreshInfoOverlay();
    }
}

QList<ResizableMediaBase*> LegacySceneMirror::enumerateMediaItems() const {
    QList<ResizableMediaBase*> mediaItems;
    QGraphicsScene* currentScene = scene();
    if (!currentScene) {
        return mediaItems;
    }

    const QList<QGraphicsItem*> allItems = currentScene->items();
    for (QGraphicsItem* item : allItems) {
        if (auto* media = dynamic_cast<ResizableMediaBase*>(item)) {
            mediaItems.append(media);
        }
    }

    return mediaItems;
}
