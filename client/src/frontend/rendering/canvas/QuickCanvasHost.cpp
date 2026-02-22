#include "frontend/rendering/canvas/QuickCanvasHost.h"

#include "frontend/rendering/canvas/LegacySceneMirror.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"

QuickCanvasHost::QuickCanvasHost(QuickCanvasController* controller, LegacySceneMirror* legacyMirror, QObject* parent)
    : ICanvasHost(parent)
    , m_controller(controller)
    , m_legacyMirror(legacyMirror)
{
    Q_ASSERT(m_controller);
    Q_ASSERT(m_legacyMirror);

    connect(m_legacyMirror, &LegacySceneMirror::mediaItemAdded, this, &QuickCanvasHost::mediaItemAdded);
    connect(m_legacyMirror, &LegacySceneMirror::mediaItemRemoved, this, &QuickCanvasHost::mediaItemRemoved);
    connect(m_legacyMirror, &LegacySceneMirror::remoteSceneLaunchStateChanged,
            this, &QuickCanvasHost::remoteSceneLaunchStateChanged);
    connect(m_legacyMirror, &LegacySceneMirror::textToolActiveChanged,
            m_controller, &QuickCanvasController::setTextToolActive);
    connect(m_controller, &QuickCanvasController::textMediaCreateRequested, this,
            [this](const QPointF& scenePos) {
                if (!m_legacyMirror) {
                    return;
                }
                if (m_controller) {
                    m_controller->ensureInitialFit();
                }
                const qreal zoom = m_controller ? m_controller->currentViewScale() : 1.0;
                m_legacyMirror->createTextAt(scenePos, zoom);
            });
    connect(m_controller, &QuickCanvasController::localFilesDropRequested, this,
            [this](const QStringList& localPaths, const QPointF& scenePos) {
                if (m_legacyMirror) {
                    m_legacyMirror->requestLocalFileDropAt(localPaths, scenePos);
                }
            });

    m_controller->setTextToolActive(m_legacyMirror->isTextToolActive());
}

QuickCanvasHost::~QuickCanvasHost() {
    if (m_controller) {
        delete m_controller;
        m_controller = nullptr;
    }
}

QuickCanvasHost* QuickCanvasHost::create(QWidget* parentWidget, LegacySceneMirror* legacyMirror, QString* errorMessage) {
    if (!legacyMirror) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("missing explicit LegacySceneMirror bridge");
        }
        return nullptr;
    }

    QuickCanvasController* controller = new QuickCanvasController();
    if (!controller->initialize(parentWidget, errorMessage)) {
        delete controller;
        return nullptr;
    }

    if (legacyMirror->parent() != controller->widget()) {
        legacyMirror->setParent(controller->widget());
    }
    legacyMirror->setVisible(false);
    legacyMirror->setOverlayViewport(controller->widget());
    controller->setMediaScene(legacyMirror->scene());

    return new QuickCanvasHost(controller, legacyMirror, controller->widget());
}

QWidget* QuickCanvasHost::asWidget() const {
    return m_controller ? m_controller->widget() : nullptr;
}

QWidget* QuickCanvasHost::viewportWidget() const {
    return asWidget();
}

void QuickCanvasHost::setActiveIdeaId(const QString& canvasSessionId) {
    if (m_legacyMirror) {
        m_legacyMirror->setActiveIdeaId(canvasSessionId);
    }
}

void QuickCanvasHost::setWebSocketClient(WebSocketClient* client) {
    if (m_legacyMirror) {
        m_legacyMirror->setWebSocketClient(client);
    }
}

void QuickCanvasHost::setUploadManager(UploadManager* manager) {
    if (m_legacyMirror) {
        m_legacyMirror->setUploadManager(manager);
    }
}

void QuickCanvasHost::setFileManager(FileManager* manager) {
    if (m_legacyMirror) {
        m_legacyMirror->setFileManager(manager);
    }
}

void QuickCanvasHost::setRemoteSceneTarget(const QString& id, const QString& machineName) {
    if (m_legacyMirror) {
        m_legacyMirror->setRemoteSceneTarget(id, machineName);
    }
}

void QuickCanvasHost::updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients) {
    if (m_legacyMirror) {
        m_legacyMirror->updateRemoteSceneTargetFromClientList(clients);
    }
}

void QuickCanvasHost::setScreens(const QList<ScreenInfo>& screens) {
    m_hasActiveScreens = !screens.isEmpty();
    if (m_legacyMirror) {
        m_legacyMirror->setScreens(screens);
    }
    if (m_controller) {
        m_controller->setScreenCount(screens.size());
        m_controller->setScreens(screens);
    }
}

bool QuickCanvasHost::hasActiveScreens() const {
    return m_hasActiveScreens;
}

void QuickCanvasHost::requestDeferredInitialRecenter(int marginPx) {
    Q_UNUSED(marginPx);
    if (m_controller) {
        m_controller->recenterView();
    }
}

void QuickCanvasHost::recenterWithMargin(int marginPx) {
    Q_UNUSED(marginPx);
    if (m_controller) {
        m_controller->recenterView();
    }
}

void QuickCanvasHost::hideContentPreservingState() {
    if (m_legacyMirror) {
        m_legacyMirror->hideContentPreservingState();
    }
}

void QuickCanvasHost::showContentAfterReconnect() {
    if (m_legacyMirror) {
        m_legacyMirror->showContentAfterReconnect();
    }
}

void QuickCanvasHost::resetTransform() {
    if (m_controller) {
        m_controller->resetView();
    }
}

void QuickCanvasHost::updateRemoteCursor(int globalX, int globalY) {
    if (m_controller) {
        m_controller->updateRemoteCursor(globalX, globalY);
    }
}

void QuickCanvasHost::hideRemoteCursor() {
    if (m_controller) {
        m_controller->hideRemoteCursor();
    }
}

QPushButton* QuickCanvasHost::getUploadButton() const {
    return m_legacyMirror ? m_legacyMirror->getUploadButton() : nullptr;
}

bool QuickCanvasHost::isRemoteSceneLaunched() const {
    return m_legacyMirror ? m_legacyMirror->isRemoteSceneLaunched() : false;
}

QString QuickCanvasHost::overlayDisabledButtonStyle() const {
    return m_legacyMirror ? m_legacyMirror->overlayDisabledButtonStyle() : QString();
}

void QuickCanvasHost::setOverlayActionsEnabled(bool enabled) {
    if (m_legacyMirror) {
        m_legacyMirror->setOverlayActionsEnabled(enabled);
    }
    if (m_controller) {
        m_controller->setShellActive(enabled);
    }
}

void QuickCanvasHost::handleRemoteConnectionLost() {
    if (m_legacyMirror) {
        m_legacyMirror->handleRemoteConnectionLost();
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
    if (m_legacyMirror) {
        m_legacyMirror->setViewportUpdateMode(mode);
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
    return m_legacyMirror ? m_legacyMirror->scene() : nullptr;
}

QList<ResizableMediaBase*> QuickCanvasHost::enumerateMediaItems() const {
    return m_legacyMirror ? m_legacyMirror->enumerateMediaItems() : QList<ResizableMediaBase*>();
}

void QuickCanvasHost::refreshInfoOverlay() {
    if (m_legacyMirror) {
        m_legacyMirror->refreshInfoOverlay();
    }
}
