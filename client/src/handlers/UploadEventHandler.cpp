#include "UploadEventHandler.h"
#include "MainWindow.h"
#include "UploadManager.h"
#include "FileManager.h"
#include "FileWatcher.h"
#include "ScreenCanvas.h"
#include "WebSocketClient.h"
#include "MediaItems.h"
#include "ToastNotificationSystem.h"
#include "SessionManager.h"
#include <QGraphicsItem>
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

    ScreenCanvas* canvas = session->canvas;
    auto& upload = session->upload;

    if (uploadManager->isUploading()) {
        if (m_mainWindow->getActiveUploadSessionIdentity() == session->persistentClientId) {
            uploadManager->requestCancel();
            TOAST_WARNING("Upload cancelled");
        } else {
            TOAST_WARNING("Another client upload is currently in progress. Please wait for it to finish.");
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
    qDebug() << "  - lastRemovalClientId:" << uploadManager->lastRemovalClientId();
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

    FileManager* fileManager = m_mainWindow->getFileManager();
    if (canvas->scene()) {
        const QList<QGraphicsItem*> allItems = canvas->scene()->items();
        for (QGraphicsItem* it : allItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(it);
            if (!media) continue;

            const QString path = media->sourcePath();
            if (path.isEmpty()) continue;

            QFileInfo fi(path);
            if (!fi.exists() || !fi.isFile()) {
                mediaItemsToRemove.append(media);
                continue;
            }

            const QString fileId = media->fileId();
            if (fileId.isEmpty()) {
                qWarning() << "MainWindow: Media item has no fileId, skipping:" << media->mediaId();
                continue;
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

    m_mainWindow->reconcileRemoteFilesForSession(*session, currentFileIds);

    if (files.isEmpty()) {
        if (hasRemoteFiles) {
            bool promotedAny = false;
            if (canvas->scene()) {
                const QList<QGraphicsItem*> allItems = canvas->scene()->items();
                for (QGraphicsItem* it : allItems) {
                    if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                        const QString fileId = media->fileId();
                        if (fileId.isEmpty()) continue;
                        if (!fileManager->isFileUploadedToClient(fileId, targetClientId)) {
                            fileManager->markFileUploadedToClient(fileId, targetClientId);
                            media->setUploadUploaded();
                            promotedAny = true;
                        }
                    }
                }
            }
            if (promotedAny) {
                emit uploadManager->uiStateChanged();
                TOAST_SUCCESS("All media already synchronized with remote client");
                return;
            }

            if (sessionHasRemote) {
                qDebug() << "Requesting remote removal for" << targetClientId << "(session" << session->persistentClientId << ")";
                uploadManager->setActiveSessionIdentity(session->persistentClientId);
                uploadManager->requestRemoval(targetClientId);
                TOAST_INFO(QString("Requesting remote removal from %1…").arg(clientLabel));
                return;
            }

            if (managerHasActive) {
                uploadManager->requestUnload();
            } else {
                TOAST_INFO("Remote media already cleared");
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

    uploadManager->clearLastRemovalClientId();

    if (canvas->scene()) {
        const QList<QGraphicsItem*> allItems = canvas->scene()->items();
        for (QGraphicsItem* it : allItems) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(it)) {
                // Check if this media's file is being uploaded
                const QString fileId = fileManager->getFileIdForMedia(media->mediaId());
                if (!fileId.isEmpty() && fileIdsBeingUploaded.contains(fileId)) {
                    media->setUploadUploading(0);
                }
            }
        }
    }

    if (!m_mainWindow->areUploadSignalsConnected()) {
        m_mainWindow->connectUploadSignals();
    }

    TOAST_INFO(QString("Starting upload of %1 file(s) to %2...").arg(files.size()).arg(clientLabel));

    upload.remoteFilesPresent = false;
    m_mainWindow->setActiveUploadSessionIdentity(session->persistentClientId);
    uploadManager->setActiveSessionIdentity(session->persistentClientId);

    uploadManager->toggleUpload(files);

    if (uploadManager->isUploading()) {
        upload.activeUploadId = uploadManager->currentUploadId();
        if (!upload.activeUploadId.isEmpty()) {
            m_mainWindow->setUploadSessionByUploadId(upload.activeUploadId, session->persistentClientId);
        }
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
    if (!session || !session->canvas || !session->canvas->scene()) return;

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

