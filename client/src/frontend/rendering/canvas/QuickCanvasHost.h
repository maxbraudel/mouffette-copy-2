#ifndef QUICKCANVASHOST_H
#define QUICKCANVASHOST_H

#include "shared/rendering/ICanvasHost.h"

class QuickCanvasController;

class QuickCanvasHost : public ICanvasHost {
    Q_OBJECT

public:
    explicit QuickCanvasHost(QuickCanvasController* controller, QObject* parent = nullptr);
    ~QuickCanvasHost() override;

    static QuickCanvasHost* create(QWidget* parentWidget, QString* errorMessage = nullptr);

    QWidget* asWidget() const override;
    QWidget* viewportWidget() const override;

    void setActiveIdeaId(const QString& canvasSessionId) override;
    void setWebSocketClient(WebSocketClient* client) override;
    void setUploadManager(UploadManager* manager) override;
    void setFileManager(FileManager* manager) override;
    void setRemoteSceneTarget(const QString& id, const QString& machineName) override;
    void updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients) override;

    void setScreens(const QList<ScreenInfo>& screens) override;
    bool hasActiveScreens() const override;
    void requestDeferredInitialRecenter(int marginPx) override;
    void recenterWithMargin(int marginPx) override;
    void hideContentPreservingState() override;
    void showContentAfterReconnect() override;
    void resetTransform() override;
    void updateRemoteCursor(int globalX, int globalY) override;
    void hideRemoteCursor() override;

    QPushButton* getUploadButton() const override;
    bool isRemoteSceneLaunched() const override;
    QString overlayDisabledButtonStyle() const override;

    void setOverlayActionsEnabled(bool enabled) override;
    void handleRemoteConnectionLost() override;

    void setSizePolicy(QSizePolicy::Policy horizontal, QSizePolicy::Policy vertical) override;
    void setViewportUpdateMode(QGraphicsView::ViewportUpdateMode mode) override;
    void setFocusPolicy(Qt::FocusPolicy policy) override;
    void setFocus(Qt::FocusReason reason) override;
    void installEventFilter(QObject* filterObj) override;

    QGraphicsScene* scene() const override;
    void refreshInfoOverlay() override;

private:
    QuickCanvasController* m_controller;
    QGraphicsScene* m_placeholderScene;
    bool m_hasActiveScreens = false;
};

#endif // QUICKCANVASHOST_H
