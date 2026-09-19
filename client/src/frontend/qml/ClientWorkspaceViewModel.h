#ifndef CLIENTWORKSPACEVIEWMODEL_H
#define CLIENTWORKSPACEVIEWMODEL_H

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>

class ICanvasHost;
class MediaListModel;
class MediaSettingsViewModel;
class TimelineController;
class UploadManager;

class ClientWorkspaceViewModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString workspaceId READ workspaceId CONSTANT)
    Q_PROPERTY(QObject* mediaModel READ mediaModel NOTIFY mediaModelChanged)
    Q_PROPERTY(int mediaCount READ mediaCount NOTIFY mediaCountChanged)
    Q_PROPERTY(bool hasCanvasMedia READ hasCanvasMedia NOTIFY hasCanvasMediaChanged)
    Q_PROPERTY(QObject* mediaSettings READ mediaSettings CONSTANT)
    Q_PROPERTY(QObject* timeline READ timeline CONSTANT)
    Q_PROPERTY(QObject* canvasController READ canvasController NOTIFY mediaModelChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool hasScreens READ hasScreens NOTIFY actionStateChanged)
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY actionStateChanged)
    Q_PROPERTY(bool actionPending READ actionPending NOTIFY actionStateChanged)
    Q_PROPERTY(bool mediaEditingEnabled READ mediaEditingEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(bool canvasNavigation READ canvasNavigation NOTIFY actionStateChanged)
    Q_PROPERTY(bool mediaEditing READ mediaEditingEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(bool textCreation READ textCreation NOTIFY actionStateChanged)
    Q_PROPERTY(bool fileDrop READ fileDrop NOTIFY actionStateChanged)
    Q_PROPERTY(bool localTest READ localTest NOTIFY actionStateChanged)
    Q_PROPERTY(bool mediaSync READ mediaSync NOTIFY actionStateChanged)
    Q_PROPERTY(bool remoteScene READ remoteScene NOTIFY actionStateChanged)
    Q_PROPERTY(QString canvasNavigationUnavailableReason READ canvasNavigationUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString mediaEditingUnavailableReason READ mediaEditingUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString textCreationUnavailableReason READ textCreationUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString fileDropUnavailableReason READ fileDropUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString localTestUnavailableReason READ localTestUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString mediaSyncUnavailableReason READ mediaSyncUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString activeTool READ activeTool NOTIFY activeToolChanged)
    Q_PROPERTY(bool settingsVisible READ settingsVisible WRITE setSettingsVisible NOTIFY settingsVisibleChanged)
    Q_PROPERTY(QString remoteSceneActionText READ remoteSceneActionText NOTIFY actionStateChanged)
    Q_PROPERTY(QUrl remoteSceneActionIcon READ remoteSceneActionIcon NOTIFY actionStateChanged)
    Q_PROPERTY(SceneActionState remoteSceneActionState READ remoteSceneActionState NOTIFY actionStateChanged)
    Q_PROPERTY(bool remoteSceneActionEnabled READ remoteSceneActionEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(int remoteSceneActionTone READ remoteSceneActionTone NOTIFY actionStateChanged)
    Q_PROPERTY(QString remoteSceneUnavailableReason READ remoteSceneUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString testSceneActionText READ testSceneActionText NOTIFY actionStateChanged)
    Q_PROPERTY(SceneActionState testSceneActionState READ testSceneActionState NOTIFY actionStateChanged)
    Q_PROPERTY(bool testSceneActionEnabled READ testSceneActionEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(int testSceneActionTone READ testSceneActionTone NOTIFY actionStateChanged)
    Q_PROPERTY(QString testSceneUnavailableReason READ testSceneUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString uploadActionText READ uploadActionText NOTIFY actionStateChanged)
    Q_PROPERTY(QUrl uploadActionIcon READ uploadActionIcon NOTIFY actionStateChanged)
    Q_PROPERTY(UploadState uploadState READ uploadState NOTIFY actionStateChanged)
    Q_PROPERTY(bool uploadActionEnabled READ uploadActionEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(QString uploadUnavailableReason READ uploadUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(int uploadActionTone READ uploadActionTone NOTIFY actionStateChanged)

public:
    enum class SceneActionState { Unavailable, Ready, Starting, Active, Stopping };
    Q_ENUM(SceneActionState)
    enum class UploadState {
        Unavailable,
        Ready,
        Preparing,
        Uploading,
        Finalizing,
        Cancelling,
        Uploaded,
        Removing
    };
    Q_ENUM(UploadState)

    explicit ClientWorkspaceViewModel(QString workspaceEndpointId,
                                    ICanvasHost* canvas,
                                    std::function<void()> uploadAction,
                                    UploadManager* uploadManager,
                                    std::function<bool()> remoteFilesPresent,
                                    std::function<bool()> hasUnuploadedFiles,
                                    std::function<bool()> hasProject = {},
                                    QObject* parent = nullptr);

    QString workspaceId() const { return m_workspaceEndpointId; }
    QObject* mediaModel() const;
    int mediaCount() const;
    bool hasCanvasMedia() const { return m_hasCanvasMedia; }
    QObject* mediaSettings() const;
    QObject* timeline() const;
    QObject* canvasController() const;
    bool loading() const { return m_loading; }
    bool hasScreens() const;
    bool hasProject() const;
    bool actionPending() const { return m_actionPending; }
    bool mediaEditingEnabled() const;
    bool canvasNavigation() const;
    bool textCreation() const { return mediaEditingEnabled(); }
    bool fileDrop() const { return mediaEditingEnabled(); }
    bool localTest() const { return testSceneActionEnabled(); }
    bool mediaSync() const;
    bool remoteScene() const { return remoteSceneActionEnabled(); }
    QString canvasNavigationUnavailableReason() const;
    QString mediaEditingUnavailableReason() const;
    QString textCreationUnavailableReason() const {
        return mediaEditingUnavailableReason();
    }
    QString fileDropUnavailableReason() const {
        return mediaEditingUnavailableReason();
    }
    QString localTestUnavailableReason() const {
        return testSceneUnavailableReason();
    }
    QString mediaSyncUnavailableReason() const;
    QString activeTool() const;
    bool settingsVisible() const { return m_settingsVisible; }
    void setSettingsVisible(bool visible);
    QString remoteSceneActionText() const;
    QUrl remoteSceneActionIcon() const;
    SceneActionState remoteSceneActionState() const;
    bool remoteSceneActionEnabled() const;
    int remoteSceneActionTone() const;
    QString remoteSceneUnavailableReason() const;
    QString testSceneActionText() const;
    SceneActionState testSceneActionState() const;
    bool testSceneActionEnabled() const;
    int testSceneActionTone() const;
    QString testSceneUnavailableReason() const;
    QString uploadActionText() const;
    QUrl uploadActionIcon() const;
    UploadState uploadState() const;
    bool uploadActionEnabled() const;
    QString uploadUnavailableReason() const;
    int uploadActionTone() const;

    void setCanvas(ICanvasHost* canvas);
    void setLoading(bool loading);
    void refreshCapabilities();

    Q_INVOKABLE void selectMedia(const QString& mediaId, bool additive);
    Q_INVOKABLE void setActiveTool(const QString& tool);
    Q_INVOKABLE void toggleRemoteScene();
    Q_INVOKABLE void toggleTestScene();
    Q_INVOKABLE void triggerUploadAction();
    Q_INVOKABLE bool beginFileDrag(const QVariantList& urls, qreal x, qreal y);
    Q_INVOKABLE bool updateFileDrag(qreal x, qreal y);
    Q_INVOKABLE bool commitFileDrop(qreal x, qreal y);
    Q_INVOKABLE void cancelFileDrag();

signals:
    void mediaModelChanged();
    void mediaCountChanged();
    void hasCanvasMediaChanged();
    void loadingChanged();
    void actionStateChanged();
    void activeToolChanged();
    void settingsVisibleChanged();

private:
    MediaListModel* typedMediaModel() const;
    void refreshSources();
    void scheduleSourceRefresh();
    bool remoteCommandsEnabled() const;
    void dispatchAction(std::function<void()> action);
    bool uploadBelongsToSession() const;
    // Shared order with OverlayActionButton.Tone, covered by rendered-state tests.
    enum ActionTone { NormalTone, UploadingTone, UploadedTone, RemoteTone, TestTone };

    QString m_workspaceEndpointId;
    QPointer<ICanvasHost> m_canvas;
    std::function<void()> m_uploadAction;
    QPointer<UploadManager> m_uploadManager;
    std::function<bool()> m_remoteFilesPresent;
    std::function<bool()> m_hasUnuploadedFiles;
    std::function<bool()> m_hasProject;
    bool m_loading = true;
    bool m_hasCanvasMedia = false;
    bool m_actionPending = false;
    bool m_settingsVisible = false;
    MediaSettingsViewModel* m_mediaSettings = nullptr;
    TimelineController* m_timeline = nullptr;
    MediaListModel* m_overlayMediaModel = nullptr;
    bool m_sourceRefreshQueued = false;
    int m_uploadPercent = 0;
    int m_uploadFilesCompleted = 0;
    int m_uploadFilesTotal = 0;
};

#endif // CLIENTWORKSPACEVIEWMODEL_H
