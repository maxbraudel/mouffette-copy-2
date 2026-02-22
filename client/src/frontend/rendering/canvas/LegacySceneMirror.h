#ifndef LEGACYSCENEMIRROR_H
#define LEGACYSCENEMIRROR_H

#include <QObject>
#include <QGraphicsView>
#include <QString>
#include <QList>
#include <QStringList>
#include "backend/domain/models/ClientInfo.h"

class ScreenCanvas;
class QGraphicsScene;
class QPointF;
class QWidget;
class WebSocketClient;
class UploadManager;
class FileManager;
class QPushButton;
class ResizableMediaBase;
class ClientInfo;
struct ScreenInfo;

class LegacySceneMirror : public QObject {
    Q_OBJECT

public:
    explicit LegacySceneMirror(ScreenCanvas* mediaCanvas, QObject* parent = nullptr);
    ~LegacySceneMirror() override = default;

    ScreenCanvas* mediaCanvas() const;

    void setVisible(bool visible);
    void setOverlayViewport(QWidget* viewport);

    void setActiveIdeaId(const QString& canvasSessionId);
    void setWebSocketClient(WebSocketClient* client);
    void setUploadManager(UploadManager* manager);
    void setFileManager(FileManager* manager);
    void setRemoteSceneTarget(const QString& id, const QString& machineName);
    void updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients);

    void setScreens(const QList<ScreenInfo>& screens);
    void hideContentPreservingState();
    void showContentAfterReconnect();
    void setOverlayActionsEnabled(bool enabled);
    void handleRemoteConnectionLost();
    void setViewportUpdateMode(QGraphicsView::ViewportUpdateMode mode);

    bool isTextToolActive() const;
    void createTextAt(const QPointF& scenePos, qreal currentZoomScale);
    void requestLocalFileDropAt(const QStringList& localPaths, const QPointF& scenePos);

    QPushButton* getUploadButton() const;
    bool isRemoteSceneLaunched() const;
    QString overlayDisabledButtonStyle() const;

    QGraphicsScene* scene() const;
    void refreshInfoOverlay();
    QList<ResizableMediaBase*> enumerateMediaItems() const;

signals:
    void mediaItemAdded(ResizableMediaBase* mediaItem);
    void mediaItemRemoved(ResizableMediaBase* mediaItem);
    void remoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName);
    void textToolActiveChanged(bool active);

private:
    ScreenCanvas* m_mediaCanvas = nullptr;
};

#endif // LEGACYSCENEMIRROR_H
