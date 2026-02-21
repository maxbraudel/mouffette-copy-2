#include "frontend/rendering/canvas/QuickCanvasHost.h"

#include "frontend/rendering/canvas/QuickCanvasController.h"

#include <QGraphicsScene>

QuickCanvasHost::QuickCanvasHost(QuickCanvasController* controller, QObject* parent)
    : ICanvasHost(parent)
    , m_controller(controller)
    , m_placeholderScene(new QGraphicsScene(this))
{
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
    return new QuickCanvasHost(controller, controller->widget());
}

QWidget* QuickCanvasHost::asWidget() const {
    return m_controller ? m_controller->widget() : nullptr;
}

QWidget* QuickCanvasHost::viewportWidget() const {
    return asWidget();
}

void QuickCanvasHost::setActiveIdeaId(const QString&) {
}

void QuickCanvasHost::setWebSocketClient(WebSocketClient*) {
}

void QuickCanvasHost::setUploadManager(UploadManager*) {
}

void QuickCanvasHost::setFileManager(FileManager*) {
}

void QuickCanvasHost::setRemoteSceneTarget(const QString&, const QString&) {
}

void QuickCanvasHost::updateRemoteSceneTargetFromClientList(const QList<ClientInfo>&) {
}

void QuickCanvasHost::setScreens(const QList<ScreenInfo>& screens) {
    m_hasActiveScreens = !screens.isEmpty();
    if (m_controller) {
        m_controller->setScreenCount(screens.size());
        m_controller->setScreens(screens);
    }
}

bool QuickCanvasHost::hasActiveScreens() const {
    return m_hasActiveScreens;
}

void QuickCanvasHost::requestDeferredInitialRecenter(int) {
    if (m_controller) {
        m_controller->recenterView();
    }
}

void QuickCanvasHost::recenterWithMargin(int) {
    if (m_controller) {
        m_controller->recenterView();
    }
}

void QuickCanvasHost::hideContentPreservingState() {
    if (QWidget* shell = asWidget()) {
        shell->setVisible(false);
    }
}

void QuickCanvasHost::showContentAfterReconnect() {
    if (QWidget* shell = asWidget()) {
        shell->setVisible(true);
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
    return nullptr;
}

bool QuickCanvasHost::isRemoteSceneLaunched() const {
    return false;
}

QString QuickCanvasHost::overlayDisabledButtonStyle() const {
    return QStringLiteral(
        "QPushButton {"
        "background-color: rgba(0,0,0,0.35);"
        "color: rgba(255,255,255,0.6);"
        "border: 1px solid rgba(255,255,255,0.2);"
        "border-radius: 10px;"
        "padding: 8px 14px;"
        "}");
}

void QuickCanvasHost::setOverlayActionsEnabled(bool enabled) {
    if (m_controller) {
        m_controller->setShellActive(enabled);
    }
}

void QuickCanvasHost::handleRemoteConnectionLost() {
    if (m_controller) {
        m_controller->setShellActive(false);
    }
}

void QuickCanvasHost::setSizePolicy(QSizePolicy::Policy horizontal, QSizePolicy::Policy vertical) {
    if (QWidget* shell = asWidget()) {
        shell->setSizePolicy(horizontal, vertical);
    }
}

void QuickCanvasHost::setViewportUpdateMode(QGraphicsView::ViewportUpdateMode) {
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
    return m_placeholderScene;
}

void QuickCanvasHost::refreshInfoOverlay() {
}
