#include "UploadSignalConnector.h"
#include "MainWindow.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/media/MediaItems.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QHash>
#include <QString>
#include <QStringList>
#include <algorithm>

UploadSignalConnector::UploadSignalConnector(QObject* parent)
    : QObject(parent)
{
}

void UploadSignalConnector::connectAllSignals(
    MainWindow* mainWindow,
    UploadManager* uploadManager,
    WebSocketClient* webSocketClient,
    bool& uploadSignalsConnected)
{
    if (uploadSignalsConnected || !uploadManager || !mainWindow) return;
    
    // Signal: File upload started - mark items as uploading
    connect(uploadManager, &UploadManager::fileUploadStarted, mainWindow, [mainWindow](const QString& fileId) {
        if (MainWindow::CanvasSession* session = mainWindow->sessionForActiveUpload()) {
            if (!session->canvas) return;
            session->upload.perFileProgress[fileId] = 0;
            const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
            for (ResizableMediaBase* item : items) {
                if (item && item->uploadState() != ResizableMediaBase::UploadState::Uploaded) {
                    item->setUploadUploading(0);
                }
            }
        }
    });
    
    // Signal: File upload progress - update upload progress on items
    connect(uploadManager, &UploadManager::fileUploadProgress, mainWindow, [mainWindow](const QString& fileId, int percent) {
        if (MainWindow::CanvasSession* session = mainWindow->sessionForActiveUpload()) {
            if (!session->canvas) return;
            if (percent >= 100) {
                session->upload.perFileProgress[fileId] = 100;
                const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
                for (ResizableMediaBase* item : items) {
                    if (item) item->setUploadUploaded();
                }
                session->upload.serverCompletedFileIds.insert(fileId);
                return;
            }
            int clamped = std::clamp(percent, 0, 99);
            int previous = session->upload.perFileProgress.value(fileId, -1);
            if (previous >= 100 || clamped <= previous) return;
            session->upload.perFileProgress[fileId] = clamped;
            const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
            for (ResizableMediaBase* item : items) {
                if (item && item->uploadState() != ResizableMediaBase::UploadState::Uploaded) {
                    item->setUploadUploading(clamped);
                }
            }
        }
    });
    
    // Signal: Per-file progress from server - update items based on server feedback
    connect(webSocketClient, &WebSocketClient::uploadPerFileProgressReceived, mainWindow,
            [mainWindow](const QString& uploadId, const QHash<QString, int>& filePercents) {
        MainWindow::CanvasSession* session = mainWindow->sessionForUploadId(uploadId);
        if (!session || !session->canvas) return;

        if (!session->upload.receivingFilesToastShown && !filePercents.isEmpty()) {
            const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                ? session->serverAssignedId
                : session->lastClientInfo.getDisplayText();
            TOAST_INFO(QString("Remote client %1 is receiving files...").arg(label));
            session->upload.receivingFilesToastShown = true;
        }

        for (auto it = filePercents.constBegin(); it != filePercents.constEnd(); ++it) {
            const QString& fid = it.key();
            const int p = std::clamp(it.value(), 0, 100);
            int previous = session->upload.perFileProgress.value(fid, -1);
            if (p <= previous && p < 100) continue;
            session->upload.perFileProgress[fid] = std::max(previous, p);
            const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fid);
            for (ResizableMediaBase* item : items) {
                if (!item) continue;
                if (p >= 100) item->setUploadUploaded();
                else item->setUploadUploading(p);
            }
            if (p >= 100) session->upload.serverCompletedFileIds.insert(fid);
        }
    });
    
    // Signal: Upload finished - show success toast and update session state
    connect(uploadManager, &UploadManager::uploadFinished, mainWindow, [mainWindow]() {
        if (MainWindow::CanvasSession* session = mainWindow->sessionForActiveUpload()) {
            const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                ? session->serverAssignedId
                : session->lastClientInfo.getDisplayText();
            TOAST_SUCCESS(QString("Upload completed successfully to %1").arg(label));
            session->upload.remoteFilesPresent = true;
            session->knownRemoteFileIds.unite(session->expectedIdeaFileIds);
            mainWindow->clearUploadTracking(*session);
        } else {
            TOAST_SUCCESS("Upload completed successfully");
        }
    });
    
    // Signal: Upload completed file IDs - mark files as uploaded
    connect(uploadManager, &UploadManager::uploadCompletedFileIds, mainWindow, [mainWindow](const QStringList& fileIds) {
        if (MainWindow::CanvasSession* session = mainWindow->sessionForActiveUpload()) {
            if (!session->canvas) return;
            for (const QString& fileId : fileIds) {
                if (session->upload.serverCompletedFileIds.contains(fileId)) continue;
                const QList<ResizableMediaBase*> items = session->upload.itemsByFileId.value(fileId);
                for (ResizableMediaBase* item : items) {
                    if (item) item->setUploadUploaded();
                }
                session->upload.serverCompletedFileIds.insert(fileId);
            }
        }
    });
    
    // Signal: All files removed - clear upload state and reset items
    connect(uploadManager, &UploadManager::allFilesRemoved, mainWindow, [mainWindow, uploadManager]() {
        MainWindow::CanvasSession* session = mainWindow->sessionForActiveUpload();
        if (!session) {
            const QString activeIdentity = uploadManager->activeSessionIdentity();
            if (!activeIdentity.isEmpty()) {
                session = mainWindow->findCanvasSession(activeIdentity);
            }
        }
        if (!session) {
            const QString lastClientId = uploadManager->lastRemovalClientId();
            if (!lastClientId.isEmpty()) {
                session = mainWindow->findCanvasSessionByServerClientId(lastClientId);
            }
        }

        if (session) {
            const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                ? session->serverAssignedId
                : session->lastClientInfo.getDisplayText();
            TOAST_INFO(QString("All files removed from %1").arg(label));

            session->upload.remoteFilesPresent = false;
            if (session->canvas) {
                for (ResizableMediaBase* media : session->canvas->enumerateMediaItems()) {
                    if (media) {
                        media->setUploadNotUploaded();
                    }
                }
            }
            mainWindow->clearUploadTracking(*session);
            uploadManager->clearLastRemovalClientId();
        } else {
            TOAST_INFO("All files removed from remote client");
            uploadManager->clearLastRemovalClientId();
        }
    });
    
    uploadSignalsConnected = true;
}
