#pragma once

#include "backend/domain/models/ClientInfo.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

class CanvasDocument;
class CanvasMedia;
class FileManager;
class UploadManager;
class WebSocketClient;

// Non-visual session contract. The Qt Quick item is attached separately by the
// presentation view model; business services only see this document host.
class ICanvasHost : public QObject
{
    Q_OBJECT

public:
    enum class Tool { Selection, Text };
    Q_ENUM(Tool)

    explicit ICanvasHost(QObject* parent = nullptr) : QObject(parent) {}
    ~ICanvasHost() override = default;

    virtual CanvasDocument* document() const = 0;
    virtual QList<CanvasMedia*> enumerateMediaItems() const = 0;
    virtual void deleteMediaItemCanonical(CanvasMedia* mediaItem) = 0;

    virtual void setActiveProjectId(const QString& projectId) = 0;
    virtual void setWebSocketClient(WebSocketClient* client) = 0;
    virtual void setUploadManager(UploadManager* manager) = 0;
    virtual void setFileManager(FileManager* manager) = 0;
    virtual void setRemoteSceneTarget(const QString& id,
                                      const QString& machineName) = 0;
    virtual void updateRemoteSceneTargetFromClientList(
        const QList<ClientInfo>& clients) = 0;
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
    virtual bool overlayActionsEnabled() const = 0;
    virtual void setProjectEditingEnabled(bool enabled) = 0;
    virtual bool projectEditingEnabled() const = 0;
    virtual void handleRemoteConnectionLost() = 0;
    virtual void stopScenesForSourceInvalidation() = 0;

    virtual Tool currentTool() const = 0;
    virtual void setCurrentTool(Tool tool) = 0;
    virtual bool remoteSceneLaunching() const = 0;
    virtual bool remoteSceneStopping() const = 0;
    virtual bool remoteSceneLaunched() const = 0;
    virtual bool testSceneLaunched() const = 0;
    virtual bool remoteSceneActionEnabled() const = 0;
    virtual bool testSceneActionEnabled() const = 0;
    virtual void triggerRemoteSceneAction() = 0;
    virtual void triggerTestSceneAction() = 0;

    virtual QJsonObject serializeProjectState() const = 0;
    virtual bool restoreProjectState(
        const QJsonObject& state,
        const QHash<QString, QString>& sourcePathByMediaId,
        QStringList* skippedMediaIds = nullptr) = 0;

signals:
    void mediaItemAdded(CanvasMedia* mediaItem);
    void mediaItemRemoved(CanvasMedia* mediaItem);
    void mediaItemChanged(CanvasMedia* mediaItem);
    void actionStateChanged();
    void toolChanged();
    void remoteSceneLaunchStateChanged(bool active,
                                       const QString& targetClientId,
                                       const QString& targetMachineName);
    void localScenePresentationRequested(quint64 generation);
};
