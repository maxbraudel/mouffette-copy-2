#ifndef CANVASSESSIONVIEWMODEL_H
#define CANVASSESSIONVIEWMODEL_H

#include <QObject>
#include <QPointer>
#include <functional>

class ICanvasHost;
class MediaListModel;
class MediaSettingsViewModel;
class UploadManager;

class CanvasSessionViewModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString workspaceId READ workspaceId CONSTANT)
    Q_PROPERTY(QString sessionId READ sessionId CONSTANT)
    Q_PROPERTY(QObject* mediaModel READ mediaModel NOTIFY mediaModelChanged)
    Q_PROPERTY(int mediaCount READ mediaCount NOTIFY mediaCountChanged)
    Q_PROPERTY(QObject* mediaSettings READ mediaSettings CONSTANT)
    Q_PROPERTY(QObject* canvasController READ canvasController NOTIFY mediaModelChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY actionStateChanged)
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
    Q_PROPERTY(SceneActionState remoteSceneActionState READ remoteSceneActionState NOTIFY actionStateChanged)
    Q_PROPERTY(bool remoteSceneActionEnabled READ remoteSceneActionEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(QString remoteSceneUnavailableReason READ remoteSceneUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString testSceneActionText READ testSceneActionText NOTIFY actionStateChanged)
    Q_PROPERTY(SceneActionState testSceneActionState READ testSceneActionState NOTIFY actionStateChanged)
    Q_PROPERTY(bool testSceneActionEnabled READ testSceneActionEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(QString testSceneUnavailableReason READ testSceneUnavailableReason NOTIFY actionStateChanged)
    Q_PROPERTY(QString uploadActionText READ uploadActionText NOTIFY actionStateChanged)
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

    explicit CanvasSessionViewModel(QString sessionId,
                                    ICanvasHost* canvas,
                                    std::function<void()> uploadAction,
                                    UploadManager* uploadManager,
                                    std::function<bool()> remoteFilesPresent,
                                    std::function<bool()> hasUnuploadedFiles,
                                    std::function<bool()> hasProject = {},
                                    QObject* parent = nullptr);

    QString workspaceId() const { return m_sessionId; }
    QString sessionId() const { return workspaceId(); }
    QObject* mediaModel() const;
    int mediaCount() const;
    QObject* mediaSettings() const;
    QObject* canvasController() const;
    bool loading() const { return m_loading; }
    bool hasProject() const;
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
    SceneActionState remoteSceneActionState() const;
    bool remoteSceneActionEnabled() const;
    QString remoteSceneUnavailableReason() const;
    QString testSceneActionText() const;
    SceneActionState testSceneActionState() const;
    bool testSceneActionEnabled() const;
    QString testSceneUnavailableReason() const;
    QString uploadActionText() const;
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
    void loadingChanged();
    void actionStateChanged();
    void activeToolChanged();
    void settingsVisibleChanged();

private:
    MediaListModel* typedMediaModel() const;
    bool remoteCommandsEnabled() const;

    QString m_sessionId;
    QPointer<ICanvasHost> m_canvas;
    std::function<void()> m_uploadAction;
    QPointer<UploadManager> m_uploadManager;
    std::function<bool()> m_remoteFilesPresent;
    std::function<bool()> m_hasUnuploadedFiles;
    std::function<bool()> m_hasProject;
    bool m_loading = true;
    bool m_settingsVisible = false;
    MediaSettingsViewModel* m_mediaSettings = nullptr;
    int m_uploadPercent = 0;
    int m_uploadFilesCompleted = 0;
    int m_uploadFilesTotal = 0;
};

// Production UI uses workspace terminology. The base class keeps the former
// name as a source-compatible facade for extensions while they migrate.
class ClientWorkspaceViewModel final : public CanvasSessionViewModel
{
public:
    using CanvasSessionViewModel::CanvasSessionViewModel;
};

#endif // CANVASSESSIONVIEWMODEL_H
