#pragma once

#include "shared/rendering/ICanvasHost.h"

#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <functional>

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

    void setActiveProjectId(const QString& projectId) override;
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
    void updateRemoteCursor(int screenId, const QPointF& screenPosition) override;
    void hideRemoteCursor() override;
    void setOverlayActionsEnabled(bool enabled) override;
    bool overlayActionsEnabled() const override { return m_actionsEnabled; }
    void setProjectEditingEnabled(bool enabled) override;
    bool projectEditingEnabled() const override { return m_projectEditingEnabled; }
    void handleRemoteConnectionLost() override;
    void stopScenesForSourceInvalidation() override;

    Tool currentTool() const override;
    void setCurrentTool(Tool tool) override;
    bool remoteSceneLaunching() const override { return m_sceneLaunching; }
    bool remoteSceneStopping() const override { return m_sceneStopping; }
    bool remoteSceneLaunched() const override { return m_sceneLaunched; }
    bool testSceneLaunched() const override { return m_testSceneLaunched; }
    bool remoteSceneActionEnabled() const override;
    bool testSceneActionEnabled() const override;
    QString mediaReadinessReason(bool remote) const;
    bool remoteMediaCached(const QString& mediaId) const;
    void triggerRemoteSceneAction() override;
    void triggerTestSceneAction() override;
    void timelinePlay();
    void timelinePause();
    void timelineSeek(qreal positionMs);
    qreal timelinePositionMs() const;
    bool timelinePlaying() const { return m_timelinePlaying; }

signals:
    void timelineTransportChanged();

public:

    QJsonObject serializeProjectState() const override;
    bool restoreProjectState(
        const QJsonObject& state,
        const QHash<QString, QString>& sourcePathByMediaId,
        QStringList* skippedMediaIds = nullptr) override;

private:
    QJsonArray buildSceneManifest(const QJsonObject& scene,
                                  QString* errorMessage) const;
    QJsonArray localPreparationChecklist(const QJsonObject& scene,
                                         bool* ready,
                                         QString* errorMessage) const;
    void reportLocalScenePrepared();
    void prepareSceneVideos(std::function<void()> ready);
    void tryArmRemoteScene();
    void beginScenePresentation(bool remote);
    void stopScenePresentation();
    void startPresentationBarrier();
    void cancelPresentationBarrier();
    void failScene(const QString& message, bool notifyServer);
    bool matchesScene(const QJsonObject& envelope) const;
    void publishActionState();
    void connectWebSocketSignals();
    void sendVideoSnapshot();
    void advanceTimeline();
    void applyTimeline(qreal positionMs, bool playing, bool forceSeek = false);
    qreal timelineStopMs() const;
    qreal timelineNowMs() const;

    QStringList residencyOwners() const;
    QString m_residencyGroup;
    CanvasDocument* m_document = nullptr;
    QuickCanvasController* m_controller = nullptr;
    QPointer<WebSocketClient> m_webSocket;
    QPointer<UploadManager> m_uploadManager;
    FileManager* m_fileManager = nullptr;
    QString m_targetClientId;
    QString m_targetMachineName;
    bool m_actionsEnabled = false;
    bool m_projectEditingEnabled = false;
    bool m_contentAvailable = true;
    bool m_sceneLaunching = false;
    bool m_sceneStopping = false;
    bool m_sceneLaunched = false;
    QJsonObject m_runningSceneDefinition;
    bool m_testSceneLaunched = false;
    bool m_sceneAccepted = false;
    bool m_localPreparedReported = false;
    bool m_sceneAllPrepared = false;
    bool m_sceneArmed = false;
    bool m_sceneCommitScheduled = false;
    bool m_firstFrameReported = false;
    quint64 m_sceneRevision = 0;
    QString m_sceneRunId;
    QString m_sceneDigest;
    QJsonArray m_localPrepareChecklist;
    QObject* m_sceneContext = nullptr;
    QPointer<QObject> m_videoPreparation;
    bool m_localVideosPrepared = false;
    QTimer m_sceneTimeout;
    QTimer m_videoSnapshotTimer;
    QMetaObject::Connection m_frameConnection;
    int m_framesRemaining = 0;
    QTimer m_timelineTimer;
    QElapsedTimer m_timelineClock;
    qreal m_timelineAnchorPositionMs = 0;
    qint64 m_remoteStartServerMs = -1;
    bool m_timelinePlaying = false;
    bool m_timelineRemote = false;
    QHash<QString, QString> m_timelineClipIds;
    QHash<QString, bool> m_timelineVideoPlaying;
    QHash<QString, qint64> m_timelineSeekGuards;
};
