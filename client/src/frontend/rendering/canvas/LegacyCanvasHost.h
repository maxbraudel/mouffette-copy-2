#ifndef LEGACYCANVASHOST_H
#define LEGACYCANVASHOST_H

#include "shared/rendering/ICanvasHost.h"

class ScreenCanvas;

class LegacyCanvasHost : public ICanvasHost {
    Q_OBJECT

public:
    explicit LegacyCanvasHost(ScreenCanvas* canvas, QObject* parent = nullptr);
    ~LegacyCanvasHost() override = default;

    static LegacyCanvasHost* create(QWidget* parentWidget);

    ScreenCanvas* legacyCanvas() const;

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
    ScreenCanvas* m_canvas;
};

#endif // LEGACYCANVASHOST_H
