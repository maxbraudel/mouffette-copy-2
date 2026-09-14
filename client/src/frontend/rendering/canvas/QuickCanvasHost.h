#pragma once

#include "shared/rendering/ICanvasHost.h"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

class CanvasDocument;
class CanvasMedia;
class QuickCanvasController;

class QuickCanvasHost final : public ICanvasHost
{
    Q_OBJECT

public:
    explicit QuickCanvasHost(CanvasDocument* document,
                             QuickCanvasController* controller,
                             QObject* parent = nullptr);
    ~QuickCanvasHost() override;

    static QuickCanvasHost* create(QString* errorMessage = nullptr);
    QuickCanvasController* controller() const { return m_controller; }
    CanvasDocument* document() const override { return m_document; }
    QList<CanvasMedia*> enumerateMediaItems() const override;
    void deleteMediaItemCanonical(CanvasMedia* mediaItem) override;

    void setActiveIdeaId(const QString& canvasSessionId) override;
    void setWebSocketClient(WebSocketClient* client) override;
    void setUploadManager(UploadManager* manager) override;
    void setFileManager(FileManager* manager) override;
    void setRemoteSceneTarget(const QString& id,
                              const QString& machineName) override;
    void updateRemoteSceneTargetFromClientList(
        const QList<ClientInfo>& clients) override;
    void setScreens(const QList<ScreenInfo>& screens) override;
    bool hasActiveScreens() const override;
    void requestDeferredInitialRecenter(int marginPx) override;
    void recenterWithMargin(int marginPx) override;
    void hideContentPreservingState() override;
    void showContentAfterReconnect() override;
    void resetTransform() override;
    void updateRemoteCursor(int globalX, int globalY) override;
    void hideRemoteCursor() override;
    void setOverlayActionsEnabled(bool enabled) override;
    bool overlayActionsEnabled() const override { return m_actionsEnabled; }
    void setProjectEditingEnabled(bool enabled) override;
    bool projectEditingEnabled() const override { return m_projectEditingEnabled; }
    void handleRemoteConnectionLost() override;
    void stopScenesForSourceInvalidation() override;

    Tool currentTool() const override { return m_tool; }
    void setCurrentTool(Tool tool) override;
    bool remoteSceneLaunching() const override { return m_sceneLaunching; }
    bool remoteSceneStopping() const override { return m_sceneStopping; }
    bool remoteSceneLaunched() const override { return m_sceneLaunched; }
    bool testSceneLaunched() const override { return m_testSceneLaunched; }
    bool remoteSceneActionEnabled() const override;
    bool testSceneActionEnabled() const override;
    void triggerRemoteSceneAction() override;
    void triggerTestSceneAction() override;

    QJsonObject serializeProjectState() const override;
    bool restoreProjectState(
        const QJsonObject& state,
        const QHash<QString, QString>& sourcePathByMediaId,
        QStringList* skippedMediaIds = nullptr) override;

private:
    struct DraftMediaState {
        QPointer<CanvasMedia> media;
        bool visible = true;
        bool muted = false;
        bool playing = false;
        qint64 positionMs = 0;
    };

    QJsonArray buildSceneManifest(const QJsonObject& scene,
                                  QString* errorMessage) const;
    QJsonArray localPreparationChecklist(const QJsonObject& scene,
                                         bool* ready,
                                         QString* errorMessage) const;
    void beginScenePresentation(bool remote);
    void stopScenePresentation();
    void startPresentationBarrier();
    void cancelPresentationBarrier();
    void failScene(const QString& message, bool notifyServer);
    bool matchesScene(const QJsonObject& envelope) const;
    void publishActionState();
    void connectWebSocketSignals();
    void sendVideoSnapshot();

    CanvasDocument* m_document = nullptr;
    QuickCanvasController* m_controller = nullptr;
    QPointer<WebSocketClient> m_webSocket;
    QPointer<UploadManager> m_uploadManager;
    FileManager* m_fileManager = nullptr;
    QString m_targetClientId;
    QString m_targetMachineName;
    Tool m_tool = Tool::Selection;
    bool m_actionsEnabled = false;
    bool m_projectEditingEnabled = false;
    bool m_contentAvailable = true;
    bool m_sceneLaunching = false;
    bool m_sceneStopping = false;
    bool m_sceneLaunched = false;
    bool m_testSceneLaunched = false;
    bool m_sceneArmed = false;
    bool m_firstFrameReported = false;
    quint64 m_sceneRevision = 0;
    QString m_sceneRunId;
    QString m_sceneDigest;
    QList<DraftMediaState> m_draftState;
    QObject* m_sceneContext = nullptr;
    QTimer m_sceneTimeout;
    QTimer m_videoSnapshotTimer;
    QMetaObject::Connection m_frameConnection;
    int m_framesRemaining = 0;
};
