#ifndef CANVASSESSIONVIEWMODEL_H
#define CANVASSESSIONVIEWMODEL_H

#include <QObject>
#include <QPointer>
#include <functional>

class ICanvasHost;
class MediaListModel;
class MediaSettingsViewModel;
class UploadManager;

class CanvasSessionViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString sessionId READ sessionId CONSTANT)
    Q_PROPERTY(QObject* mediaModel READ mediaModel NOTIFY mediaModelChanged)
    Q_PROPERTY(int mediaCount READ mediaCount NOTIFY mediaCountChanged)
    Q_PROPERTY(QObject* mediaSettings READ mediaSettings CONSTANT)
    Q_PROPERTY(QObject* canvasController READ canvasController NOTIFY mediaModelChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool actionsEnabled READ actionsEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(QString activeTool READ activeTool NOTIFY activeToolChanged)
    Q_PROPERTY(bool settingsVisible READ settingsVisible WRITE setSettingsVisible NOTIFY settingsVisibleChanged)
    Q_PROPERTY(QString remoteSceneActionText READ remoteSceneActionText NOTIFY actionStateChanged)
    Q_PROPERTY(SceneActionState remoteSceneActionState READ remoteSceneActionState NOTIFY actionStateChanged)
    Q_PROPERTY(bool remoteSceneActionEnabled READ remoteSceneActionEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(QString testSceneActionText READ testSceneActionText NOTIFY actionStateChanged)
    Q_PROPERTY(SceneActionState testSceneActionState READ testSceneActionState NOTIFY actionStateChanged)
    Q_PROPERTY(bool testSceneActionEnabled READ testSceneActionEnabled NOTIFY actionStateChanged)
    Q_PROPERTY(QString uploadActionText READ uploadActionText NOTIFY actionStateChanged)
    Q_PROPERTY(UploadState uploadState READ uploadState NOTIFY actionStateChanged)
    Q_PROPERTY(bool uploadActionEnabled READ uploadActionEnabled NOTIFY actionStateChanged)
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
                                    QObject* parent = nullptr);

    QString sessionId() const { return m_sessionId; }
    QObject* mediaModel() const;
    int mediaCount() const;
    QObject* mediaSettings() const;
    QObject* canvasController() const;
    bool loading() const { return m_loading; }
    bool actionsEnabled() const;
    QString activeTool() const;
    bool settingsVisible() const { return m_settingsVisible; }
    void setSettingsVisible(bool visible);
    QString remoteSceneActionText() const;
    SceneActionState remoteSceneActionState() const;
    bool remoteSceneActionEnabled() const;
    QString testSceneActionText() const;
    SceneActionState testSceneActionState() const;
    bool testSceneActionEnabled() const;
    QString uploadActionText() const;
    UploadState uploadState() const;
    bool uploadActionEnabled() const;
    int uploadActionTone() const;

    void setCanvas(ICanvasHost* canvas);
    void setLoading(bool loading);

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

    QString m_sessionId;
    QPointer<ICanvasHost> m_canvas;
    std::function<void()> m_uploadAction;
    QPointer<UploadManager> m_uploadManager;
    std::function<bool()> m_remoteFilesPresent;
    std::function<bool()> m_hasUnuploadedFiles;
    bool m_loading = true;
    bool m_settingsVisible = false;
    MediaSettingsViewModel* m_mediaSettings = nullptr;
    int m_uploadPercent = 0;
    int m_uploadFilesCompleted = 0;
    int m_uploadFilesTotal = 0;
};

#endif // CANVASSESSIONVIEWMODEL_H
