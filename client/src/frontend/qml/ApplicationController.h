#ifndef APPLICATIONCONTROLLER_H
#define APPLICATIONCONTROLLER_H

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <memory>

#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/RuntimeStorageBootstrap.h"

class ClientWorkspaceViewModel;
class ClientListModel;
class HistoryListModel;
class ApplicationRuntime;
class SceneActivityListModel;
class ToastListModel;

class ApplicationController final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QString bootstrapTitle READ bootstrapTitle NOTIFY bootstrapChanged)
    Q_PROPERTY(QString bootstrapDetail READ bootstrapDetail NOTIFY bootstrapChanged)
    Q_PROPERTY(QString bootstrapPrimaryText READ bootstrapPrimaryText NOTIFY bootstrapChanged)
    Q_PROPERTY(bool bootstrapDecisionRequired READ bootstrapDecisionRequired NOTIFY bootstrapChanged)

    Q_PROPERTY(ApplicationPage applicationPage READ applicationPage NOTIFY applicationPageChanged)
    Q_PROPERTY(QString pageTitle READ pageTitle NOTIFY applicationPageChanged)
    Q_PROPERTY(QObject* clientsModel READ clientsModel CONSTANT)
    Q_PROPERTY(QObject* sceneActivitiesModel READ sceneActivitiesModel CONSTANT)
    Q_PROPERTY(QObject* historyModel READ historyModel CONSTANT)
    Q_PROPERTY(QObject* toastModel READ toastModel CONSTANT)
    Q_PROPERTY(QObject* activeWorkspace READ activeWorkspace NOTIFY activeWorkspaceChanged)

    Q_PROPERTY(bool connectionEnabled READ connectionEnabled NOTIFY presentationChanged)
    Q_PROPERTY(QString localStatusText READ localStatusText NOTIFY presentationChanged)
    Q_PROPERTY(ConnectionState localConnectionState READ localConnectionState NOTIFY presentationChanged)
    Q_PROPERTY(QString remoteDisplayName READ remoteDisplayName NOTIFY presentationChanged)
    Q_PROPERTY(QString remoteStatusText READ remoteStatusText NOTIFY presentationChanged)
    Q_PROPERTY(ConnectionState remoteConnectionState READ remoteConnectionState NOTIFY presentationChanged)
    Q_PROPERTY(QString remoteVolumeText READ remoteVolumeText NOTIFY presentationChanged)
    Q_PROPERTY(bool remoteVolumeVisible READ remoteVolumeVisible NOTIFY presentationChanged)
    Q_PROPERTY(bool remoteBusy READ remoteBusy NOTIFY presentationChanged)
    Q_PROPERTY(bool canDeleteProject READ canDeleteProject NOTIFY presentationChanged)
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY presentationChanged)

    Q_PROPERTY(QString settingsServerUrl READ settingsServerUrl NOTIFY settingsChanged)
    Q_PROPERTY(bool settingsAutoUpload READ settingsAutoUpload NOTIFY settingsChanged)

    Q_PROPERTY(QString dialogTitle READ dialogTitle NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogMessage READ dialogMessage NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogAcceptText READ dialogAcceptText NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogRejectText READ dialogRejectText NOTIFY dialogChanged)
    Q_PROPERTY(bool dialogDestructive READ dialogDestructive NOTIFY dialogChanged)
    Q_PROPERTY(bool dialogShowReject READ dialogShowReject NOTIFY dialogChanged)

public:
    enum class ApplicationPage { Clients = 0, Canvas = 1, History = 2 };
    Q_ENUM(ApplicationPage)
    enum class ConnectionState { Connected = 0, Transitional = 1, Disconnected = 2 };
    Q_ENUM(ConnectionState)
    enum class DialogKind { None, DeleteProject, ClearHistory, IncomingSceneInfo };
    Q_ENUM(DialogKind)

    ApplicationController(RuntimeProfileContext runtimeProfile,
                          QStringList arguments,
                          QObject* parent = nullptr);
    ~ApplicationController() override;

    bool ready() const { return m_ready; }
    QString bootstrapTitle() const { return m_bootstrapTitle; }
    QString bootstrapDetail() const { return m_bootstrapDetail; }
    QString bootstrapPrimaryText() const { return m_bootstrapPrimaryText; }
    bool bootstrapDecisionRequired() const { return m_bootstrapDecisionRequired; }

    ApplicationPage applicationPage() const { return m_applicationPage; }
    QString pageTitle() const;
    QObject* clientsModel() const;
    QObject* sceneActivitiesModel() const;
    QObject* historyModel() const;
    QObject* toastModel() const;
    QObject* activeWorkspace() const;

    bool connectionEnabled() const;
    QString localStatusText() const;
    ConnectionState localConnectionState() const;
    QString remoteDisplayName() const;
    QString remoteStatusText() const;
    ConnectionState remoteConnectionState() const;
    QString remoteVolumeText() const;
    bool remoteVolumeVisible() const;
    bool remoteBusy() const;
    bool canDeleteProject() const;
    bool hasProject() const;

    QString settingsServerUrl() const;
    bool settingsAutoUpload() const;

    QString dialogTitle() const { return m_dialogTitle; }
    QString dialogMessage() const { return m_dialogMessage; }
    QString dialogAcceptText() const { return m_dialogAcceptText; }
    QString dialogRejectText() const { return m_dialogRejectText; }
    bool dialogDestructive() const { return m_dialogDestructive; }
    bool dialogShowReject() const { return m_dialogShowReject; }

    Q_INVOKABLE void start();
    Q_INVOKABLE void acceptBootstrapDecision();
    Q_INVOKABLE void quitBootstrap();
    Q_INVOKABLE void openClient(const QString& endpointId);
    Q_INVOKABLE void openOngoingScene(const QString& sceneRunId);
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void showHistory();
    Q_INVOKABLE void toggleConnection();
    Q_INVOKABLE void requestDeleteProject();
    Q_INVOKABLE void requestClearHistory();
    Q_INVOKABLE void acceptDialog();
    Q_INVOKABLE void rejectDialog();
    Q_INVOKABLE QString saveSettings(const QString& serverUrl, bool autoUpload);
    Q_INVOKABLE void hideWindow();
    Q_INVOKABLE void setWindowVisible(bool visible);
    Q_INVOKABLE void setPointerInside(bool inside);

public slots:
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void handleNativeSystemSuspendedChanged(bool suspended);
    void handleApplicationAboutToQuit();

signals:
    void readyChanged();
    void bootstrapChanged();
    void applicationPageChanged();
    void activeWorkspaceChanged();
    void presentationChanged();
    void settingsChanged();
    void dialogChanged();
    void dialogRequested();
    void raiseRequested();
    void hideRequested();

private:
    enum class BootstrapDecision { None, Retry };

    void runBootstrap();
    void finishBootstrap();
    void initializeBackend();
    void setBootstrapStage(RuntimeStorageBootstrap::Stage stage);
    void setApplicationPage(ApplicationPage page);
    void refreshPresentation();
    void refreshActiveWorkspace();
    void clearDialog();
    void showDialog(DialogKind kind, const QString& title,
                    const QString& message, const QString& acceptText,
                    const QString& rejectText, bool destructive,
                    bool showReject);
    static ConnectionState connectionStateFromStatus(const QString& status);

    RuntimeProfileContext m_runtimeProfile;
    QStringList m_arguments;
    RuntimeStorageBootstrap m_storageBootstrap;
    RuntimeStorageBootstrap::Result m_bootstrapResult;
    BootstrapDecision m_bootstrapDecision = BootstrapDecision::None;
    bool m_bootstrapStarted = false;
    bool m_multimediaBootstrapPending = false;
    bool m_ready = false;
    QString m_bootstrapTitle = QStringLiteral("Starting Mouffette");
    QString m_bootstrapDetail = QStringLiteral("Preparing local storage…");
    QString m_bootstrapPrimaryText = QStringLiteral("Continue");
    bool m_bootstrapDecisionRequired = false;

    ApplicationPage m_applicationPage = ApplicationPage::Clients;
    DialogKind m_dialogKind = DialogKind::None;
    QString m_dialogTitle;
    QString m_dialogMessage;
    QString m_dialogAcceptText;
    QString m_dialogRejectText;
    bool m_dialogDestructive = false;
    bool m_dialogShowReject = true;

    std::unique_ptr<ApplicationRuntime> m_runtime;
    ClientListModel* m_clientsModel = nullptr;
    SceneActivityListModel* m_sceneActivitiesModel = nullptr;
    HistoryListModel* m_historyModel = nullptr;
    ToastListModel* m_toastModel = nullptr;
    QHash<QString, ClientWorkspaceViewModel*> m_workspaces;
    QPointer<ClientWorkspaceViewModel> m_activeWorkspace;
};

#endif // APPLICATIONCONTROLLER_H
