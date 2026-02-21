#include "frontend/rendering/canvas/QuickCanvasHost.h"

#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include <QTransform>

QuickCanvasHost::QuickCanvasHost(QuickCanvasController* controller, ScreenCanvas* mediaCanvas, QObject* parent)
    : ICanvasHost(parent)
    , m_controller(controller)
    , m_mediaCanvas(mediaCanvas)
{
    Q_ASSERT(m_controller);
    Q_ASSERT(m_mediaCanvas);

    connect(m_mediaCanvas, &ScreenCanvas::mediaItemAdded, this, &QuickCanvasHost::mediaItemAdded);
    connect(m_mediaCanvas, &ScreenCanvas::mediaItemRemoved, this, &QuickCanvasHost::mediaItemRemoved);
    connect(m_mediaCanvas, &ScreenCanvas::remoteSceneLaunchStateChanged,
            this, &QuickCanvasHost::remoteSceneLaunchStateChanged);
    connect(m_mediaCanvas, &ScreenCanvas::textToolActiveChanged,
            m_controller, &QuickCanvasController::setTextToolActive);
    connect(m_controller, &QuickCanvasController::textMediaCreateRequested,
            m_mediaCanvas, [this](const QPointF& scenePos) {
                if (!m_mediaCanvas) return;
                if (m_controller) {
                    m_controller->ensureInitialFit();
                }
                // Temporarily apply the Quick Canvas view scale to the hidden ScreenCanvas so
                // createTextMediaAtPosition computes the correct adjustedScale (= 1/zoom), giving
                // a constant ~400 px creation size regardless of camera zoom — matching Widget Canvas.
                const qreal zoom = m_controller ? m_controller->currentViewScale() : 1.0;
                if (zoom > 1e-6 && std::abs(zoom - 1.0) > 1e-4) {
                    m_mediaCanvas->setTransform(QTransform::fromScale(zoom, zoom));
                }
                m_mediaCanvas->requestTextMediaCreateAt(scenePos, false);
                m_mediaCanvas->resetTransform();
            });
    connect(m_controller, &QuickCanvasController::localFilesDropRequested,
            m_mediaCanvas, [this](const QStringList& localPaths, const QPointF& scenePos) {
                if (m_mediaCanvas) {
                    m_mediaCanvas->requestLocalFileDropAt(localPaths, scenePos);
                }
            });

    m_controller->setTextToolActive(m_mediaCanvas->isTextToolActive());
}

QuickCanvasHost::~QuickCanvasHost() {
    if (m_controller) {
        delete m_controller;
        m_controller = nullptr;
    }
}

QuickCanvasHost* QuickCanvasHost::create(QWidget* parentWidget, QString* errorMessage) {
    QuickCanvasController* controller = new QuickCanvasController();
    if (!controller->initialize(parentWidget, errorMessage)) {
        delete controller;
        return nullptr;
    }

    ScreenCanvas* mediaCanvas = new ScreenCanvas(controller->widget());
    mediaCanvas->setVisible(false);
    mediaCanvas->setOverlayViewport(controller->widget());
    controller->setMediaScene(mediaCanvas->scene());

    return new QuickCanvasHost(controller, mediaCanvas, controller->widget());
}

QWidget* QuickCanvasHost::asWidget() const {
    return m_controller ? m_controller->widget() : nullptr;
}

QWidget* QuickCanvasHost::viewportWidget() const {
    return asWidget();
}

void QuickCanvasHost::setActiveIdeaId(const QString& canvasSessionId) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setActiveIdeaId(canvasSessionId);
    }
}

void QuickCanvasHost::setWebSocketClient(WebSocketClient* client) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setWebSocketClient(client);
    }
}

void QuickCanvasHost::setUploadManager(UploadManager* manager) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setUploadManager(manager);
    }
}

void QuickCanvasHost::setFileManager(FileManager* manager) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setFileManager(manager);
    }
}

void QuickCanvasHost::setRemoteSceneTarget(const QString& id, const QString& machineName) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setRemoteSceneTarget(id, machineName);
    }
}

void QuickCanvasHost::updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients) {
    if (m_mediaCanvas) {
        m_mediaCanvas->updateRemoteSceneTargetFromClientList(clients);
    }
}

void QuickCanvasHost::setScreens(const QList<ScreenInfo>& screens) {
    m_hasActiveScreens = !screens.isEmpty();
    if (m_mediaCanvas) {
        m_mediaCanvas->setScreens(screens);
    }
    if (m_controller) {
        m_controller->setScreenCount(screens.size());
        m_controller->setScreens(screens);
    }
}

bool QuickCanvasHost::hasActiveScreens() const {
    if (m_mediaCanvas) {
        return m_mediaCanvas->hasActiveScreens();
    }
    return m_hasActiveScreens;
}

void QuickCanvasHost::requestDeferredInitialRecenter(int marginPx) {
    if (m_mediaCanvas) {
        m_mediaCanvas->requestDeferredInitialRecenter(marginPx);
    }
    if (m_controller) {
        m_controller->recenterView();
    }
}

void QuickCanvasHost::recenterWithMargin(int marginPx) {
    if (m_mediaCanvas) {
        m_mediaCanvas->recenterWithMargin(marginPx);
    }
    if (m_controller) {
        m_controller->recenterView();
    }
}

void QuickCanvasHost::hideContentPreservingState() {
    if (m_mediaCanvas) {
        m_mediaCanvas->hideContentPreservingState();
    }
}

void QuickCanvasHost::showContentAfterReconnect() {
    if (m_mediaCanvas) {
        m_mediaCanvas->showContentAfterReconnect();
    }
}

void QuickCanvasHost::resetTransform() {
    if (m_mediaCanvas) {
        m_mediaCanvas->resetTransform();
    }
    if (m_controller) {
        m_controller->resetView();
    }
}

void QuickCanvasHost::updateRemoteCursor(int globalX, int globalY) {
    if (m_mediaCanvas) {
        m_mediaCanvas->updateRemoteCursor(globalX, globalY);
    }
    if (m_controller) {
        m_controller->updateRemoteCursor(globalX, globalY);
    }
}

void QuickCanvasHost::hideRemoteCursor() {
    if (m_mediaCanvas) {
        m_mediaCanvas->hideRemoteCursor();
    }
    if (m_controller) {
        m_controller->hideRemoteCursor();
    }
}

QPushButton* QuickCanvasHost::getUploadButton() const {
    return m_mediaCanvas ? m_mediaCanvas->getUploadButton() : nullptr;
}

bool QuickCanvasHost::isRemoteSceneLaunched() const {
    return m_mediaCanvas ? m_mediaCanvas->isRemoteSceneLaunched() : false;
}

QString QuickCanvasHost::overlayDisabledButtonStyle() const {
    return ScreenCanvas::overlayDisabledButtonStyle();
}

void QuickCanvasHost::setOverlayActionsEnabled(bool enabled) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setOverlayActionsEnabled(enabled);
    }
    if (m_controller) {
        m_controller->setShellActive(enabled);
    }
}

void QuickCanvasHost::handleRemoteConnectionLost() {
    if (m_mediaCanvas) {
        m_mediaCanvas->handleRemoteConnectionLost();
    }
    if (m_controller) {
        m_controller->setShellActive(false);
    }
}

void QuickCanvasHost::setSizePolicy(QSizePolicy::Policy horizontal, QSizePolicy::Policy vertical) {
    if (QWidget* shell = asWidget()) {
        shell->setSizePolicy(horizontal, vertical);
    }
}

void QuickCanvasHost::setViewportUpdateMode(QGraphicsView::ViewportUpdateMode mode) {
    if (m_mediaCanvas) {
        m_mediaCanvas->setViewportUpdateMode(mode);
    }
}

void QuickCanvasHost::setFocusPolicy(Qt::FocusPolicy policy) {
    if (QWidget* shell = asWidget()) {
        shell->setFocusPolicy(policy);
    }
}

void QuickCanvasHost::setFocus(Qt::FocusReason reason) {
    if (QWidget* shell = asWidget()) {
        shell->setFocus(reason);
    }
}

void QuickCanvasHost::installEventFilter(QObject* filterObj) {
    if (QWidget* shell = asWidget()) {
        shell->installEventFilter(filterObj);
    }
}

QGraphicsScene* QuickCanvasHost::scene() const {
    return m_mediaCanvas ? m_mediaCanvas->scene() : nullptr;
}

void QuickCanvasHost::refreshInfoOverlay() {
    if (m_mediaCanvas) {
        m_mediaCanvas->refreshInfoOverlay();
    }
}
