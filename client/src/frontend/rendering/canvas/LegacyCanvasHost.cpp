#include "frontend/rendering/canvas/LegacyCanvasHost.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/UploadManager.h"
#include "backend/files/FileManager.h"

LegacyCanvasHost::LegacyCanvasHost(ScreenCanvas* canvas, QObject* parent)
    : ICanvasHost(parent)
    , m_canvas(canvas)
{
    Q_ASSERT(m_canvas);

    connect(m_canvas, &ScreenCanvas::mediaItemAdded, this, &LegacyCanvasHost::mediaItemAdded);
    connect(m_canvas, &ScreenCanvas::mediaItemRemoved, this, &LegacyCanvasHost::mediaItemRemoved);
    connect(m_canvas, &ScreenCanvas::remoteSceneLaunchStateChanged,
            this, &LegacyCanvasHost::remoteSceneLaunchStateChanged);
}

LegacyCanvasHost* LegacyCanvasHost::create(QWidget* parentWidget) {
    ScreenCanvas* canvas = new ScreenCanvas(parentWidget);
    return new LegacyCanvasHost(canvas, canvas);
}

ScreenCanvas* LegacyCanvasHost::legacyCanvas() const {
    return m_canvas;
}

QWidget* LegacyCanvasHost::asWidget() const {
    return m_canvas;
}

QWidget* LegacyCanvasHost::viewportWidget() const {
    return m_canvas ? m_canvas->viewport() : nullptr;
}

void LegacyCanvasHost::setActiveIdeaId(const QString& canvasSessionId) {
    m_canvas->setActiveIdeaId(canvasSessionId);
}

void LegacyCanvasHost::setWebSocketClient(WebSocketClient* client) {
    m_canvas->setWebSocketClient(client);
}

void LegacyCanvasHost::setUploadManager(UploadManager* manager) {
    m_canvas->setUploadManager(manager);
}

void LegacyCanvasHost::setFileManager(FileManager* manager) {
    m_canvas->setFileManager(manager);
}

void LegacyCanvasHost::setRemoteSceneTarget(const QString& id, const QString& machineName) {
    m_canvas->setRemoteSceneTarget(id, machineName);
}

void LegacyCanvasHost::updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients) {
    m_canvas->updateRemoteSceneTargetFromClientList(clients);
}

void LegacyCanvasHost::setScreens(const QList<ScreenInfo>& screens) {
    m_canvas->setScreens(screens);
}

bool LegacyCanvasHost::hasActiveScreens() const {
    return m_canvas->hasActiveScreens();
}

void LegacyCanvasHost::requestDeferredInitialRecenter(int marginPx) {
    m_canvas->requestDeferredInitialRecenter(marginPx);
}

void LegacyCanvasHost::recenterWithMargin(int marginPx) {
    m_canvas->recenterWithMargin(marginPx);
}

void LegacyCanvasHost::hideContentPreservingState() {
    m_canvas->hideContentPreservingState();
}

void LegacyCanvasHost::showContentAfterReconnect() {
    m_canvas->showContentAfterReconnect();
}

void LegacyCanvasHost::resetTransform() {
    m_canvas->resetTransform();
}

void LegacyCanvasHost::updateRemoteCursor(int globalX, int globalY) {
    m_canvas->updateRemoteCursor(globalX, globalY);
}

void LegacyCanvasHost::hideRemoteCursor() {
    m_canvas->hideRemoteCursor();
}

QPushButton* LegacyCanvasHost::getUploadButton() const {
    return m_canvas->getUploadButton();
}

bool LegacyCanvasHost::isRemoteSceneLaunched() const {
    return m_canvas->isRemoteSceneLaunched();
}

QString LegacyCanvasHost::overlayDisabledButtonStyle() const {
    return ScreenCanvas::overlayDisabledButtonStyle();
}

void LegacyCanvasHost::setOverlayActionsEnabled(bool enabled) {
    m_canvas->setOverlayActionsEnabled(enabled);
}

void LegacyCanvasHost::handleRemoteConnectionLost() {
    m_canvas->handleRemoteConnectionLost();
}

void LegacyCanvasHost::setSizePolicy(QSizePolicy::Policy horizontal, QSizePolicy::Policy vertical) {
    m_canvas->setSizePolicy(horizontal, vertical);
}

void LegacyCanvasHost::setViewportUpdateMode(QGraphicsView::ViewportUpdateMode mode) {
    m_canvas->setViewportUpdateMode(mode);
}

void LegacyCanvasHost::setFocusPolicy(Qt::FocusPolicy policy) {
    m_canvas->setFocusPolicy(policy);
}

void LegacyCanvasHost::setFocus(Qt::FocusReason reason) {
    m_canvas->setFocus(reason);
}

void LegacyCanvasHost::installEventFilter(QObject* filterObj) {
    m_canvas->installEventFilter(filterObj);
}

QGraphicsScene* LegacyCanvasHost::scene() const {
    return m_canvas->scene();
}

void LegacyCanvasHost::refreshInfoOverlay() {
    m_canvas->refreshInfoOverlay();
}
