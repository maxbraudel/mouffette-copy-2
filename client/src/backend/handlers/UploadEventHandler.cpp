#include "UploadEventHandler.h"
#include "MainWindow.h"
#include "backend/network/UploadManager.h"
#include "backend/files/FileManager.h"
#include "backend/files/FileWatcher.h"
#include "shared/rendering/ICanvasHost.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/media/MediaItems.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include "backend/domain/session/SessionManager.h"
#include <QFileInfo>
#include <QTimer>
#include <QDebug>

UploadEventHandler::UploadEventHandler(MainWindow* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void UploadEventHandler::onUploadButtonClicked()
{
    UploadManager* uploadManager = m_mainWindow->getUploadManager();
    if (!uploadManager) return;

    MainWindow::CanvasSession* session = m_mainWindow->findCanvasSession(m_mainWindow->getActiveSessionIdentity());
    if (!session || !session->canvas) return;

    ICanvasHost* canvas = session->canvas;
    auto& upload = session->upload;

    if (uploadManager->isBusy()) {
        if (uploadManager->isUploading()
            && m_mainWindow->getActiveUploadSessionIdentity() == session->persistentClientId
            && uploadManager->canRequestCancel()) {
            uploadManager->requestCancel();
        } else if (m_mainWindow->getActiveUploadSessionIdentity() != session->persistentClientId) {
            TOAST_WARNING("Another client upload is currently in progress. Please wait for it to finish.");
        } else {
            qInfo() << "UploadManager: Duplicate upload click ignored in state"
                    << static_cast<int>(uploadManager->outgoingState());
        }
        return;
    }

    const QString targetClientId = session->serverAssignedId;
    if (targetClientId.isEmpty()) {
        TOAST_ERROR("No remote client selected for upload");
        return;
    }
    uploadManager->setTargetClientId(targetClientId);
    uploadManager->setActiveIdeaId(session->canvasSessionId);

    const QString clientLabel = session->lastClientInfo.getDisplayText().isEmpty()
        ? targetClientId
        : session->lastClientInfo.getDisplayText();

    const bool managerHasActive = uploadManager->hasActiveUpload() &&
        uploadManager->activeUploadTargetClientId() == targetClientId;
    const bool sessionHasRemote = upload.remoteFilesPresent;
    const bool hasRemoteFiles = sessionHasRemote || managerHasActive;
    
    qDebug() << "=== Upload Button Clicked Debug ===";
    qDebug() << "Target:" << targetClientId;
    qDebug() << "upload.remoteFilesPresent:" << upload.remoteFilesPresent;
    qDebug() << "sessionHasRemote:" << sessionHasRemote;
    qDebug() << "managerHasActive:" << managerHasActive;
    qDebug() << "  - hasActiveUpload:" << uploadManager->hasActiveUpload();
    qDebug() << "  - activeTargetClientId:" << uploadManager->activeUploadTargetClientId();
    qDebug() << "hasRemoteFiles:" << hasRemoteFiles;

    upload.itemsByFileId.clear();
    upload.currentUploadFileOrder.clear();
    upload.serverCompletedFileIds.clear();
    upload.perFileProgress.clear();
    upload.receivingFilesToastShown = false;

    QVector<UploadFileInfo> files;
    QList<ResizableMediaBase*> mediaItemsToRemove;
    QSet<QString> processedFileIds;
    QSet<QString> currentFileIds;
    QStringList rejectedMedia;

    FileManager* fileManager = m_mainWindow->getFileManager();
    const QList<ResizableMediaBase*> mediaItems = canvas->enumerateMediaItems();
    for (ResizableMediaBase* media : mediaItems) {
        if (!media) continue;

        if (media->isTextMedia()) continue;

        const QString path = media->sourcePath();
        if (path.isEmpty()) continue;

        QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile()) {
            mediaItemsToRemove.append(media);
            continue;
        }

        const MediaFilePolicy::ValidationResult validation =
            MediaFilePolicy::validateLocalFile(fi.absoluteFilePath());
        const MediaFilePolicy::Kind mediaKind = validation.kind;
        const bool accepted = media->isVideoMedia()
            ? mediaKind == MediaFilePolicy::Kind::Mp4Video
            : mediaKind == MediaFilePolicy::Kind::Image;
        if (!accepted) {
            rejectedMedia.append(QStringLiteral("%1 — %2")
                                     .arg(fi.fileName(),
                                          MediaFilePolicy::validationErrorDescription(
                                              validation)));
            continue;
        }

        // Refresh the identity at the upload boundary. A video can be
        // re-encoded/replaced at the same path after it was placed on canvas;
        // reusing the old path-derived ID would incorrectly skip the upload
        // and leave stale bytes on the remote client.
        const QString fileId = fileManager->getOrCreateFileId(fi.absoluteFilePath());
        if (fileId.isEmpty()) {
            qWarning() << "MainWindow: Media item has no fileId, skipping:" << media->mediaId();
            continue;
        }
        if (media->fileId() != fileId) {
            media->setFileId(fileId);
            fileManager->associateMediaWithFile(media->mediaId(), fileId);
        }

        currentFileIds.insert(fileId);
        fileManager->associateFileWithIdea(fileId, session->canvasSessionId);

        const bool alreadyOnTarget = fileManager->isFileUploadedToClient(fileId, targetClientId);
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
            upload.itemsByFileId[fileId].append(media);
        }
    }

    FileWatcher* fileWatcher = m_mainWindow->getFileWatcher();
    for (ResizableMediaBase* mediaItem : mediaItemsToRemove) {
        if (fileWatcher) {
            fileWatcher->unwatchMediaItem(mediaItem);
        }
        QTimer::singleShot(0, [itemPtr = mediaItem]() {
            itemPtr->prepareForDeletion();
            if (itemPtr->scene()) itemPtr->scene()->removeItem(itemPtr);
            delete itemPtr;
        });
    }

    if (!mediaItemsToRemove.isEmpty()) {
        canvas->refreshInfoOverlay();
        TOAST_WARNING(QString("%1 media item(s) removed - source files not found").arg(mediaItemsToRemove.size()));
    }

    if (!rejectedMedia.isEmpty()) {
        TOAST_ERROR(QStringLiteral("Upload blocked: %1")
                        .arg(rejectedMedia.join(QStringLiteral("; "))), 5000);
        return;
    }

    m_mainWindow->reconcileRemoteFilesForSession(*session, currentFileIds);

    if (files.isEmpty()) {
        if (hasRemoteFiles) {
            // This is the Unload half of the button state machine. Protocol v3
            // no longer has the legacy unscoped remove-all command, so remove
            // the session's exact validated assets through their authenticated
            // inventory tuples and wait for each target acknowledgement.
            uploadManager->setActiveSessionIdentity(session->persistentClientId);
            QSet<QString> knownRemoteFileIds = session->knownRemoteFileIds;
            knownRemoteFileIds.unite(currentFileIds);
            if (uploadManager->requestUnload(targetClientId,
                                             knownRemoteFileIds)) {
                TOAST_INFO(QString("Removing remote media from %1…")
                               .arg(clientLabel));
            } else if (!uploadManager->isRemoving()) {
                TOAST_ERROR("Remote media could not be unloaded safely", 5000);
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
    m_mainWindow->setActiveUploadSessionIdentity(session->persistentClientId);
    uploadManager->setActiveSessionIdentity(session->persistentClientId);

    const bool accepted = uploadManager->toggleUpload(files);

    if (accepted && uploadManager->isUploading()) {
        upload.activeUploadId = uploadManager->currentUploadId();
        if (!upload.activeUploadId.isEmpty()) {
            m_mainWindow->setUploadSessionByUploadId(upload.activeUploadId, session->persistentClientId);
        }

        for (ResizableMediaBase* media : canvas->enumerateMediaItems()) {
            if (!media) continue;
            const QString fileId = fileManager->getFileIdForMedia(media->mediaId());
            if (!fileId.isEmpty() && fileIdsBeingUploaded.contains(fileId)) {
                media->setUploadUploading(0);
            }
        }
        TOAST_INFO(QString("Starting upload of %1 file(s) to %2...")
                       .arg(files.size()).arg(clientLabel));
    } else if (m_mainWindow->getActiveUploadSessionIdentity() == session->persistentClientId) {
        m_mainWindow->setActiveUploadSessionIdentity(QString());
        uploadManager->setActiveSessionIdentity(QString());
    }
}

void UploadEventHandler::updateIndividualProgressFromServer(int globalPercent, int filesCompleted, int totalFiles) {
    Q_UNUSED(globalPercent);
    Q_UNUSED(totalFiles);
    if (totalFiles == 0) return;

    auto* session = m_mainWindow->sessionForActiveUpload();
    if (!session || !session->canvas) return;

    const int desired = qMax(0, filesCompleted);
    if (desired <= 0) return;

    int have = session->upload.serverCompletedFileIds.size();
    if (have >= desired) return;

    for (const QString& fileId : session->upload.currentUploadFileOrder) {
        if (session->upload.serverCompletedFileIds.contains(fileId)) continue;
        const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
        for (ResizableMediaBase* item : items) {
            if (item) item->setUploadUploaded();
        }
        session->upload.serverCompletedFileIds.insert(fileId);
        have++;
        if (have >= desired) break;
    }
}
