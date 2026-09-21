#include "backend/media/MediaResidencyManager.h"
#include <QScopeGuard>
#include "UploadEventHandler.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/network/UploadManager.h"
#include "backend/files/FileManager.h"
#include "backend/files/FileWatcher.h"
#include "shared/rendering/ICanvasHost.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include <QFileInfo>
#include <algorithm>
#include <QDebug>

UploadEventHandler::UploadEventHandler(ApplicationRuntime* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void UploadEventHandler::onUploadButtonClicked()
{
    uploadWorkspace(m_mainWindow->activeWorkspaceEndpointId());
}

void UploadEventHandler::uploadWorkspace(const QString& workspaceEndpointId, bool automatic)
{
    UploadManager* uploadManager = m_mainWindow->getUploadManager();
    if (!uploadManager) return;

    ApplicationRuntime::ClientWorkspace* session = m_mainWindow->findWorkspace(workspaceEndpointId);
    if (!session || !session->canvas) return;

    const QString previousTarget = uploadManager->targetClientId();
    uploadManager->setTargetClientId(workspaceEndpointId);
    const auto restoreTarget = qScopeGuard([uploadManager, previousTarget]() {
        uploadManager->setTargetClientId(previousTarget);
    });
    if (automatic && uploadManager->isBusy()) return;
    ICanvasHost* canvas = session->canvas;
    auto& upload = session->upload;

    if (uploadManager->isBusy()) {
        if (uploadManager->canRequestCancel()) {
            if (upload.automaticUploadTimer) upload.automaticUploadTimer->stop();
            ++upload.automaticUploadCancellationGeneration;
            uploadManager->requestCancel();
        } else if (m_mainWindow->activeUploadWorkspaceEndpointId() != session->targetEndpointId) {
            TOAST_WARNING("Another client upload is currently in progress. Please wait for it to finish.");
        } else {
            qInfo() << "UploadManager: Duplicate upload click ignored in state"
                    << static_cast<int>(uploadManager->outgoingState());
        }
        return;
    }

    // Identity is published after the background metadata/SHA-256 pass. Never
    // turn an incomplete import into an accidental Unload action.
    for (const CanvasMedia* media : session->canvas->enumerateMediaItems()) {
        if (!media || media->isText()) continue;
        if (media->fileId().isEmpty() || MediaResidencyManager::instance().sha256(media->residencyOwnerId()).isEmpty()) {
            if (!automatic) TOAST_INFO("Media identity is still being analyzed; try again shortly");
            return;
        }
    }
    const QString targetClientId = session->targetEndpointId;
    if (targetClientId.isEmpty()) {
        TOAST_ERROR("No remote client selected for upload");
        return;
    }
    uploadManager->setTargetClientId(targetClientId);

    const NotificationPeer targetPeer{targetClientId,
        session->lastClientInfo.getMachineName(),
        qMax(1, session->lastClientInfo.instanceOrdinal()), QStringLiteral("To")};
    const auto notifyTarget = [&targetPeer](const QString& message) {
        if (auto* system = ToastNotificationSystem::instance()) {
            NotificationRequest notification;
            notification.category = QStringLiteral("Upload");
            notification.message = message;
            notification.peers = {targetPeer};
            system->publishNotification(notification);
        }
    };

    const bool managerHasActive = uploadManager->hasActiveUpload() &&
        uploadManager->activeUploadTargetClientId() == targetClientId;
    const bool sessionHasRemote = std::any_of(session->knownRemoteFileIds.cbegin(), session->knownRemoteFileIds.cend(),
        [this, &targetClientId](const QString& fileId) {
            return m_mainWindow->getFileManager()->isFileUploadedToClient(fileId, targetClientId);
        });
    upload.remoteFilesPresent = sessionHasRemote || managerHasActive;
    const bool hasRemoteFiles = sessionHasRemote || managerHasActive;
    
    qDebug() << "=== Upload Button Clicked Debug ===";
    qDebug() << "Target:" << targetClientId;
    qDebug() << "upload.remoteFilesPresent:" << upload.remoteFilesPresent;
    qDebug() << "sessionHasRemote:" << sessionHasRemote;
    qDebug() << "managerHasActive:" << managerHasActive;
    qDebug() << "  - hasActiveUpload:" << uploadManager->hasActiveUpload();
    qDebug() << "  - activeTargetClientId:" << uploadManager->activeUploadTargetClientId();
    qDebug() << "hasRemoteFiles:" << hasRemoteFiles;

    upload.fileIds.clear();
    upload.currentUploadFileOrder.clear();
    upload.serverCompletedFileIds.clear();
    upload.perFileProgress.clear();
    upload.receivingFilesToastShown = false;

    QVector<UploadFileInfo> files;
    QList<CanvasMedia*> mediaItemsToRemove;
    QSet<QString> processedFileIds;
    QSet<QString> currentFileIds;
    QStringList rejectedMedia;

    FileManager* fileManager = m_mainWindow->getFileManager();
    const QList<CanvasMedia*> mediaItems = canvas->enumerateMediaItems();
    for (CanvasMedia* media : mediaItems) {
        if (!media) continue;

        if (media->isText()) continue;

        const QString path = media->sourcePath();
        if (path.isEmpty()) continue;

        QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile()) {
            mediaItemsToRemove.append(media);
            continue;
        }

        const QString fileId = media->fileId();
        if (fileId.isEmpty() || fileId != MediaResidencyManager::instance().sha256(media->residencyOwnerId())) {
            rejectedMedia.append(QStringLiteral("%1 — media identity is not ready").arg(fi.fileName()));
            continue;
        }
        fileManager->associateMediaWithFile(media->mediaId(), fileId);

        currentFileIds.insert(fileId);
        fileManager->associateFileWithProject(fileId, session->projectId);

        const bool alreadyOnTarget = fileManager->isFileUploadedToClient(fileId, targetClientId)
            && uploadManager->remoteMediaReady(targetClientId, fileId);
        if (!processedFileIds.contains(fileId) && !alreadyOnTarget) {
            UploadFileInfo info;
            info.fileId = fileId;
            info.mediaId = media->mediaId();
            info.path = fi.absoluteFilePath();
            info.name = fi.fileName();
            info.extension = fi.suffix();
            info.size = fi.size();
            files.push_back(info);
            processedFileIds.insert(fileId);
            upload.currentUploadFileOrder.append(fileId);
        }

        if (!alreadyOnTarget) {
            upload.fileIds.insert(fileId);
        }
    }

    FileWatcher* fileWatcher = m_mainWindow->getFileWatcher();
    for (CanvasMedia* mediaItem : mediaItemsToRemove) {
        if (fileWatcher) {
            fileWatcher->unwatchMediaItem(mediaItem);
        }
        // Use the canonical removal path so the overlay and selection state
        // update after the item has actually left the scene.
        canvas->deleteMediaItemCanonical(mediaItem);
    }

    if (!mediaItemsToRemove.isEmpty()) {
        TOAST_WARNING(QString("%1 media item(s) removed - source files not found").arg(mediaItemsToRemove.size()));
    }

    if (!rejectedMedia.isEmpty()) {
        TOAST_ERROR(QStringLiteral("Upload blocked: %1")
                        .arg(rejectedMedia.join(QStringLiteral("; "))),
                    AppConfig::instance().toastErrorDurationMs());
        return;
    }

    m_mainWindow->reconcileRemoteFilesForWorkspace(*session, currentFileIds);

    if (files.isEmpty()) {
        if (automatic) return;
        if (hasRemoteFiles) {
            // This is the Unload half of the button state machine. Protocol v5
            // has no unscoped remove-all command, so remove
            // the session's exact validated assets through their authenticated
            // inventory tuples and wait for each target acknowledgement.
            uploadManager->setActiveWorkspaceEndpointId(session->targetEndpointId);
            QSet<QString> knownRemoteFileIds = session->knownRemoteFileIds;
            knownRemoteFileIds.unite(currentFileIds);
            if (uploadManager->requestUnload(targetClientId,
                                             knownRemoteFileIds)) {
                notifyTarget(QStringLiteral("Removing remote media…"));
            } else if (!uploadManager->isRemoving()) {
                TOAST_ERROR("Remote media could not be unloaded safely",
                            AppConfig::instance().toastErrorDurationMs());
            }
        } else {
            TOAST_INFO("No new media to upload");
            qDebug() << "Upload skipped: aucun média local nouveau (popup supprimée).";
        }
        return;
    }

    // Track which fileIds are being uploaded to update UI state
    QSet<QString> fileIdsBeingUploaded;
    for (const auto& f : files) {
        fileIdsBeingUploaded.insert(f.fileId);
    }

    if (!m_mainWindow->areUploadSignalsConnected()) {
        m_mainWindow->connectUploadSignals();
    }

    upload.remoteFilesPresent = hasRemoteFiles;
    m_mainWindow->setActiveUploadWorkspaceEndpointId(session->targetEndpointId);
    uploadManager->setActiveWorkspaceEndpointId(session->targetEndpointId);

    const bool accepted = uploadManager->toggleUpload(files);

    if (accepted && uploadManager->isUploading()) {
        upload.activeUploadId = uploadManager->currentUploadId();
        if (!upload.activeUploadId.isEmpty()) {
            m_mainWindow->setUploadWorkspaceByUploadId(upload.activeUploadId, session->targetEndpointId);
        }

        for (CanvasMedia* media : canvas->enumerateMediaItems()) {
            if (!media) continue;
            const QString fileId = fileManager->getFileIdForMedia(media->mediaId());
            if (!fileId.isEmpty() && fileIdsBeingUploaded.contains(fileId)) {
                media->setUploadUploading(0);
            }
        }
        notifyTarget(QStringLiteral("Starting upload of %1 file(s)...").arg(files.size()));
    } else if (m_mainWindow->activeUploadWorkspaceEndpointId() == session->targetEndpointId) {
        m_mainWindow->setActiveUploadWorkspaceEndpointId(QString());
        uploadManager->setActiveWorkspaceEndpointId(QString());
    }
}

void UploadEventHandler::updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles) {
    Q_UNUSED(globalPercent);
    Q_UNUSED(totalFiles);
    if (totalFiles == 0) return;

    auto* session = m_mainWindow->workspaceForActiveUpload();
    if (!session || !session->canvas) return;

    const int desired = qMax(0, filesCompleted);
    if (desired <= 0) return;

    int have = session->upload.serverCompletedFileIds.size();
    if (have >= desired) return;

    for (const QString& fileId : session->upload.currentUploadFileOrder) {
        if (session->upload.serverCompletedFileIds.contains(fileId)) continue;
        for (CanvasMedia* item : session->canvas->enumerateMediaItems()) {
            if (item && item->fileId() == fileId) item->setUploadUploaded();
        }
        session->upload.serverCompletedFileIds.insert(fileId);
        have++;
        if (have >= desired) break;
    }
}
