#ifndef ICANVASHOST_H
#define ICANVASHOST_H

#include <QObject>
#include <QGraphicsView>
#include <QSizePolicy>
#include <QString>
#include "backend/domain/models/ClientInfo.h"
#include "shared/rendering/IMediaSceneAdapter.h"
#include "shared/rendering/IOverlayProjection.h"

class QWidget;
class WebSocketClient;
class UploadManager;
class FileManager;
class ResizableMediaBase;

class ICanvasHost : public QObject, public IMediaSceneAdapter, public IOverlayProjection {
    Q_OBJECT

public:
    explicit ICanvasHost(QObject* parent = nullptr) : QObject(parent) {}
    ~ICanvasHost() override = default;

    virtual QWidget* asWidget() const = 0;
    virtual QWidget* viewportWidget() const = 0;

    virtual void setActiveIdeaId(const QString& canvasSessionId) = 0;
    virtual void setWebSocketClient(WebSocketClient* client) = 0;
    virtual void setUploadManager(UploadManager* manager) = 0;
    virtual void setFileManager(FileManager* manager) = 0;
    virtual void setRemoteSceneTarget(const QString& id, const QString& machineName) = 0;
    virtual void updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients) = 0;

    virtual void setScreens(const QList<ScreenInfo>& screens) = 0;
    virtual bool hasActiveScreens() const = 0;
    virtual void requestDeferredInitialRecenter(int marginPx = 53) = 0;
    virtual void recenterWithMargin(int marginPx = 33) = 0;
    virtual void hideContentPreservingState() = 0;
    virtual void showContentAfterReconnect() = 0;
    virtual void resetTransform() = 0;
    virtual void updateRemoteCursor(int globalX, int globalY) = 0;
    virtual void hideRemoteCursor() = 0;

    virtual void setOverlayActionsEnabled(bool enabled) = 0;
    virtual void handleRemoteConnectionLost() = 0;

    virtual void setSizePolicy(QSizePolicy::Policy horizontal, QSizePolicy::Policy vertical) = 0;
    virtual void setViewportUpdateMode(QGraphicsView::ViewportUpdateMode mode) = 0;
    virtual void setFocusPolicy(Qt::FocusPolicy policy) = 0;
    virtual void setFocus(Qt::FocusReason reason) = 0;
    virtual void installEventFilter(QObject* filterObj) = 0;

signals:
    void mediaItemAdded(ResizableMediaBase* mediaItem);
    void mediaItemRemoved(ResizableMediaBase* mediaItem);
    void remoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName);
};

#endif // ICANVASHOST_H
