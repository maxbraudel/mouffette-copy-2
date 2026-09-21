#include "backend/handlers/UploadEventHandler.h"
#include "ClientWorkspaceController.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/domain/models/ClientInfo.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/UploadManager.h"
#include "backend/files/FileManager.h"
#include "backend/files/FileWatcher.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QTimer>
#include <QDebug>

ClientWorkspaceController::ClientWorkspaceController(ApplicationRuntime* runtime,
                                                     QObject* parent)
    : QObject(parent)
    , m_runtime(runtime)
{
}

// ============================================================================
// Workspace Lookup Methods
// ============================================================================

ClientWorkspaceController::ClientWorkspace* ClientWorkspaceController::findWorkspace(
    const QString& targetEndpointId) {
    return m_runtime->getWorkspaceManager()->findWorkspace(targetEndpointId);
}

const ClientWorkspaceController::ClientWorkspace*
ClientWorkspaceController::findWorkspace(const QString& targetEndpointId) const {
    return m_runtime->getWorkspaceManager()->findWorkspace(targetEndpointId);
}

// ============================================================================
// Workspace Lifecycle
// ============================================================================

ClientWorkspaceController::ClientWorkspace* ClientWorkspaceController::ensureWorkspace(
    const ClientInfo& client) {
    const QString targetEndpointId = client.endpointId();
    if (targetEndpointId.isEmpty()) {
        qCritical() << "ClientWorkspaceController: client has no endpoint ID";
        return nullptr;
    }
    ClientWorkspace* workspace = m_runtime->getWorkspaceManager()
        ->getOrCreateWorkspace(targetEndpointId, client);
    if (!workspace) return nullptr;
    
    // Initialize canvas if needed (UI-specific responsibility)
    if (!workspace->canvas) {
        if (m_prewarmedQuickCanvasHost) {
            workspace->canvas = m_prewarmedQuickCanvasHost;
            m_prewarmedQuickCanvasHost = nullptr;
        } else {
            QString quickError;
            workspace->canvas = QuickCanvasHost::create(&quickError);
            if (workspace->canvas) {
                m_lastQuickInitError.clear();
            } else {
                m_lastQuickInitError = quickError.isEmpty()
                    ? QStringLiteral("Qt Quick canvas failed to initialize") : quickError;
                qCritical() << "ClientWorkspaceController: mandatory Qt Quick canvas unavailable."
                            << "error=" << m_lastQuickInitError;
                TOAST_ERROR(QStringLiteral("Canvas initialization failed: %1")
                                .arg(m_lastQuickInitError),
                            5000);
            }
        }

        if (!workspace->canvas) {
            return workspace;
        }

        workspace->canvas->setActiveProjectId(workspace->projectId);
        workspace->connectionsInitialized = false;
        configureWorkspace(workspace);
    }

    workspace->canvas->setRemoteSceneTarget(
        workspace->targetEndpointId, workspace->lastClientInfo.getInstanceDisplayName());
    workspace->canvas->updateRemoteSceneTargetFromClientList({workspace->lastClientInfo});
    if (workspace->lastClientInfo.isOnline()) {
        workspace->remoteContentClearedOnDisconnect = false;
    }
    return workspace;
}

void ClientWorkspaceController::prewarmQuickCanvasHost() {
    if (!m_runtime || m_prewarmedQuickCanvasHost) {
        return;
    }

    QString quickError;
    QuickCanvasHost* prewarmedHost = QuickCanvasHost::create(&quickError);
    if (!prewarmedHost) {
        m_lastQuickInitError = quickError;
        if (!quickError.isEmpty()) {
            qWarning() << "ClientWorkspaceController: Quick prewarm failed:" << quickError;
        } else {
            qWarning() << "ClientWorkspaceController: Quick prewarm failed with unknown error";
        }
        return;
    }

    m_lastQuickInitError.clear();
    m_prewarmedQuickCanvasHost = prewarmedHost;
    prewarmedHost->setOverlayActionsEnabled(false);
}

void ClientWorkspaceController::configureWorkspace(ClientWorkspace* workspace) {
    if (!workspace || !workspace->canvas) return;

    workspace->canvas->setActiveProjectId(workspace->projectId);
    workspace->canvas->setWebSocketClient(m_runtime->getWebSocketClient());
    workspace->canvas->setUploadManager(m_runtime->getUploadManager());
    workspace->canvas->setFileManager(m_runtime->getFileManager());
    // Connect to ApplicationRuntime signal via direct call (onRemoteSceneLaunchStateChanged is private)
    connect(workspace->canvas, &ICanvasHost::remoteSceneLaunchStateChanged, m_runtime,
            &ApplicationRuntime::onRemoteSceneLaunchStateChanged,
            Qt::UniqueConnection);

    if (!workspace->connectionsInitialized) {
        auto* projectAutosaveTimer = new QTimer(workspace->canvas);
        projectAutosaveTimer->setObjectName(
            QStringLiteral("projectAutosave_%1").arg(workspace->targetEndpointId));
        projectAutosaveTimer->setSingleShot(true);
        projectAutosaveTimer->setInterval(
            AppConfig::instance().projectAutosaveDelayMs());
        connect(projectAutosaveTimer, &QTimer::timeout, m_runtime,
                [this, targetEndpointId=workspace->targetEndpointId]() {
            if (m_runtime) {
                m_runtime->persistProjectCanvas(targetEndpointId);
            }
        });
        auto* sourceReconcileTimer = new QTimer(workspace->canvas);
        sourceReconcileTimer->setSingleShot(true);
        sourceReconcileTimer->setInterval(0);
        connect(sourceReconcileTimer, &QTimer::timeout, m_runtime,
                [this, targetEndpointId=workspace->targetEndpointId] {
            auto* current = findWorkspace(targetEndpointId);
            if (!current || !current->canvas) return;
            QSet<QString> sources;
            for (const auto* media : current->canvas->enumerateMediaItems()) {
                if (!media || media->isText()) continue;
                // Keep existing remote sources while an import is acquiring its identity.
                if (media->fileId().isEmpty()) return;
                sources.insert(media->fileId());
            }
            m_runtime->reconcileRemoteFilesForWorkspace(*current, sources);
        });
        if (workspace->canvas->document()) {
            connect(workspace->canvas->document(), &CanvasDocument::documentChanged,
                    sourceReconcileTimer, qOverload<>(&QTimer::start));
            connect(workspace->canvas->document(), &CanvasDocument::editsLockedChanged,
                    m_runtime, [this, targetEndpointId=workspace->targetEndpointId] {
                m_runtime->reconcileProjectMediaResidency(targetEndpointId);
            });
            connect(workspace->canvas->document(), &CanvasDocument::documentChanged,
                    projectAutosaveTimer, [projectAutosaveTimer] {
                        projectAutosaveTimer->start();
                    });
            connect(workspace->canvas->document(), &CanvasDocument::cameraChanged,
                    projectAutosaveTimer, [projectAutosaveTimer] {
                        projectAutosaveTimer->start();
                    });
        }
        auto* autoUploadTimer = new QTimer(workspace->canvas);
        workspace->upload.automaticUploadTimer = autoUploadTimer;
        autoUploadTimer->setSingleShot(true);
        autoUploadTimer->setInterval(350);
        connect(autoUploadTimer, &QTimer::timeout, m_runtime,
                [this, autoUploadTimer, targetEndpointId=workspace->targetEndpointId]() {
            if (!m_runtime->getAutoUploadImportedMedia() || !m_runtime->getUploadManager()
                || !m_runtime->m_uploadEventHandler) return;
            auto* current = findWorkspace(targetEndpointId);
            if (!current || !current->canvas || !current->canvas->overlayActionsEnabled()) return;
            auto* manager = m_runtime->getUploadManager();
            const QString selected = manager->targetClientId();
            manager->setTargetClientId(targetEndpointId);
            const bool busy = manager->isBusy();
            manager->setTargetClientId(selected);
            if (busy) { autoUploadTimer->start(); return; }
            m_runtime->m_uploadEventHandler->uploadWorkspace(targetEndpointId, true);
        });
        connect(workspace->canvas, &ICanvasHost::mediaItemAdded, m_runtime,
                [this, autoUploadTimer, targetEndpointId=workspace->targetEndpointId](CanvasMedia* mediaItem) {
                    if (m_runtime->getFileWatcher() && mediaItem && !mediaItem->sourcePath().isEmpty()) {
                        m_runtime->getFileWatcher()->watchMediaItem(mediaItem);
                        qDebug() << "ClientWorkspaceController: source watch added for mediaId"
                                 << mediaItem->mediaId();
                    }
                    ClientWorkspace* changedWorkspace = m_runtime->getWorkspaceManager()
                        ->findWorkspace(targetEndpointId);
                    if (changedWorkspace) {
                        changedWorkspace->lastClientInfo.setFromMemory(true);
                    }
                    // Update upload button state immediately when media is added
                    if (m_runtime->getUploadManager()) {
                        emit m_runtime->getUploadManager()->uiStateChanged();
                    }
                    if (mediaItem && !mediaItem->isText()) {
                        const quint64 cancellationGeneration = changedWorkspace
                            ? changedWorkspace->upload.automaticUploadCancellationGeneration : 0;
                        connect(mediaItem, &CanvasMedia::identityReady, autoUploadTimer,
                                [this, autoUploadTimer, targetEndpointId,
                                 cancellationGeneration](const QString&) {
                            if (m_runtime->getUploadManager())
                                emit m_runtime->getUploadManager()->uiStateChanged();
                            const auto* current = findWorkspace(targetEndpointId);
                            if (current && current->upload.automaticUploadCancellationGeneration
                                    == cancellationGeneration
                                && m_runtime->getAutoUploadImportedMedia()) autoUploadTimer->start();
                        });
                        if (!mediaItem->fileId().isEmpty()
                            && m_runtime->getAutoUploadImportedMedia()) autoUploadTimer->start();
                    }
                });
        
        connect(workspace->canvas, &ICanvasHost::mediaItemRemoved, m_runtime,
                [this, targetEndpointId=workspace->targetEndpointId](CanvasMedia* mediaItem) {
                    if (m_runtime->getFileWatcher() && mediaItem) {
                        m_runtime->getFileWatcher()->unwatchMediaItem(mediaItem);
                    }
                    // Update upload button state immediately when media is removed
                    if (m_runtime->getUploadManager()) {
                        emit m_runtime->getUploadManager()->uiStateChanged();
                    }
                    if (m_runtime) {
                        QTimer::singleShot(0, m_runtime,
                            [this, targetEndpointId]() {
                                if (m_runtime) {
                                    m_runtime->persistProjectCanvas(targetEndpointId);
                                }
                            });
                    }
                });
    }

    workspace->connectionsInitialized = true;
}

void ClientWorkspaceController::switchToWorkspace(const QString& targetEndpointId) {
    ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    if (!workspace || !workspace->canvas) return;

    m_runtime->setActiveWorkspaceEndpointId(targetEndpointId);
    m_runtime->setActiveCanvas(workspace->canvas);
    if (m_runtime->getNavigationManager()) {
        m_runtime->getNavigationManager()->setActiveCanvas(workspace->canvas);
    }

    workspace->canvas->setRemoteSceneTarget(
        workspace->targetEndpointId, workspace->lastClientInfo.getInstanceDisplayName());
    workspace->canvas->updateRemoteSceneTargetFromClientList({workspace->lastClientInfo});
    m_runtime->updateWorkspaceCapabilities(targetEndpointId);
    updateUploadButtonForWorkspace(workspace);
}

void ClientWorkspaceController::updateUploadButtonForWorkspace(
    ClientWorkspace* workspace) {
    if (!workspace) return;
    
    if (m_runtime->getUploadManager()) {
        emit m_runtime->getUploadManager()->uiStateChanged();
    }
}

void ClientWorkspaceController::clearUploadTracking(ClientWorkspace* workspace) {
    if (!workspace) return;
    
    workspace->upload.fileIds.clear();
    workspace->upload.currentUploadFileOrder.clear();
    workspace->upload.serverCompletedFileIds.clear();
    workspace->upload.perFileProgress.clear();
    workspace->upload.receivingFilesToastShown = false;
    if (!workspace->upload.activeUploadId.isEmpty()) {
        m_runtime->removeUploadWorkspaceByUploadId(workspace->upload.activeUploadId);
        workspace->upload.activeUploadId.clear();
    }
    if (m_runtime->activeUploadWorkspaceEndpointId() == workspace->targetEndpointId) {
        m_runtime->setActiveUploadWorkspaceEndpointId(QString());
    }
    if (m_runtime->getUploadManager()
        && m_runtime->getUploadManager()->activeWorkspaceEndpointId()
            == workspace->targetEndpointId) {
        m_runtime->getUploadManager()->setActiveWorkspaceEndpointId(QString());
    }
}

ClientWorkspaceController::ClientWorkspace*
ClientWorkspaceController::workspaceForActiveUpload() {
    if (!m_runtime->activeUploadWorkspaceEndpointId().isEmpty()) {
        if (ClientWorkspace* workspace = findWorkspace(
                m_runtime->activeUploadWorkspaceEndpointId())) return workspace;
    }
    if (m_runtime->getUploadManager()) {
        const QString clientId = m_runtime->getUploadManager()->activeUploadTargetClientId();
        if (!clientId.isEmpty()) return findWorkspace(clientId);
    }
    return nullptr;
}

ClientWorkspaceController::ClientWorkspace*
ClientWorkspaceController::workspaceForUploadId(const QString& uploadId) {
    if (uploadId.isEmpty()) return workspaceForActiveUpload();
    const QString identity = m_runtime->uploadWorkspaceByUploadId(uploadId);
    if (identity.isEmpty()) return nullptr;
    ClientWorkspace* workspace = findWorkspace(identity);
    // A delayed terminal signal from a closed session must not finish or
    // clear a replacement upload, including one targeting the same workspace.
    return workspace && workspace->upload.activeUploadId == uploadId
        ? workspace : nullptr;
}
