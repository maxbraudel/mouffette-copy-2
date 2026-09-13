#include "UploadSignalConnector.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/media/CanvasMedia.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QHash>
#include <QString>
#include <QStringList>
#include <algorithm>

namespace {
void publishTerminalUploadNotification(const QString& uploadId,
                                       NotificationSeverity severity,
                                       const QString& message,
                                       int durationMs = -1)
{
    ToastNotificationSystem* system = ToastNotificationSystem::instance();
    if (!system) return;

    NotificationRequest notification;
    notification.severity = severity;
    notification.category = QStringLiteral("Upload");
    notification.message = message;
    notification.toastDurationMs = durationMs;
    notification.correlationId = NotificationCorrelation::upload(uploadId);
    notification.terminal = true;
    system->publishNotification(notification);
}
}

UploadSignalConnector::UploadSignalConnector(QObject* parent)
    : QObject(parent)
{
}

void UploadSignalConnector::connectAllSignals(
    ApplicationRuntime* mainWindow,
    UploadManager* uploadManager,
    WebSocketClient* webSocketClient,
    bool& uploadSignalsConnected)
{
    if (uploadSignalsConnected || !uploadManager || !mainWindow) return;
    Q_UNUSED(webSocketClient);
    
    // Signal: File upload started - mark items as uploading
    connect(uploadManager, &UploadManager::fileUploadStarted, mainWindow, [mainWindow](const QString& fileId) {
        if (ApplicationRuntime::CanvasSession* session = mainWindow->sessionForActiveUpload()) {
            if (!session->canvas) return;
            session->upload.perFileProgress[fileId] = 0;
            const QList<CanvasMedia*> items = session->upload.itemsByFileId.value(fileId);
            for (CanvasMedia* item : items) {
                if (item && item->uploadState() != CanvasMedia::UploadState::Uploaded) {
                    item->setUploadUploading(0);
                }
            }
        }
    });
    
    // Signal: File upload progress - update upload progress on items
    connect(uploadManager, &UploadManager::fileUploadProgress, mainWindow, [mainWindow](const QString& fileId, int percent) {
        if (ApplicationRuntime::CanvasSession* session = mainWindow->sessionForActiveUpload()) {
            if (!session->canvas) return;
            if (percent >= 100) {
                session->upload.perFileProgress[fileId] = 100;
                const QList<CanvasMedia*> items = session->upload.itemsByFileId.value(fileId);
                for (CanvasMedia* item : items) {
                    if (item) item->setUploadUploaded();
                }
                session->upload.serverCompletedFileIds.insert(fileId);
                return;
            }
            int clamped = std::clamp(percent, 0, 99);
            int previous = session->upload.perFileProgress.value(fileId, -1);
            if (previous >= 100 || clamped <= previous) return;
            session->upload.perFileProgress[fileId] = clamped;
            const QList<CanvasMedia*> items = session->upload.itemsByFileId.value(fileId);
            for (CanvasMedia* item : items) {
                if (item && item->uploadState() != CanvasMedia::UploadState::Uploaded) {
                    item->setUploadUploading(clamped);
                }
            }
        }
    });
    
    // Signal: Upload finished - show success toast and update session state
    connect(uploadManager, &UploadManager::uploadFinished, mainWindow,
            [mainWindow](const QString& uploadId) {
        if (ApplicationRuntime::CanvasSession* session = mainWindow->sessionForUploadId(uploadId)) {
            const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                ? session->serverAssignedId
                : session->lastClientInfo.getDisplayText();
            publishTerminalUploadNotification(
                uploadId, NotificationSeverity::Success,
                QStringLiteral("Upload completed successfully to %1").arg(label));
            session->upload.remoteFilesPresent = true;
            session->knownRemoteFileIds.unite(session->expectedIdeaFileIds);
            mainWindow->clearUploadTracking(*session);
        } else {
            publishTerminalUploadNotification(
                uploadId, NotificationSeverity::Success,
                QStringLiteral("Upload completed successfully"));
        }
    });

    connect(uploadManager, &UploadManager::uploadCancelled, mainWindow,
            [mainWindow](const QString& uploadId) {
        if (ApplicationRuntime::CanvasSession* session = mainWindow->sessionForUploadId(uploadId)) {
            for (auto it = session->upload.itemsByFileId.constBegin();
                 it != session->upload.itemsByFileId.constEnd(); ++it) {
                for (CanvasMedia* item : it.value()) {
                    if (item) item->setUploadNotUploaded();
                }
            }
            session->upload.remoteFilesPresent = !session->knownRemoteFileIds.isEmpty();
            mainWindow->clearUploadTracking(*session);
        }
        publishTerminalUploadNotification(
            uploadId, NotificationSeverity::Warning,
            QStringLiteral("Upload cancelled; incomplete remote data is being cleaned automatically"));
    });

    // Signal: the target rejected the transfer. Roll back only this batch; files
    // already known on the remote remain synchronized and keep their state.
    connect(uploadManager, &UploadManager::uploadRejected, mainWindow,
            [mainWindow](const QString& uploadId, const QString& reason) {
        ApplicationRuntime::CanvasSession* session = mainWindow->sessionForUploadId(uploadId);
        if (session) {
            for (auto it = session->upload.itemsByFileId.constBegin();
                 it != session->upload.itemsByFileId.constEnd(); ++it) {
                for (CanvasMedia* item : it.value()) {
                    if (item) item->setUploadNotUploaded();
                }
            }
            session->upload.remoteFilesPresent = !session->knownRemoteFileIds.isEmpty();
            mainWindow->clearUploadTracking(*session);
        }

        const QString detail = reason.trimmed().isEmpty()
            ? QStringLiteral("Remote client rejected the upload")
            : reason.trimmed();
        publishTerminalUploadNotification(
            uploadId, NotificationSeverity::Error,
            QStringLiteral("Upload failed: %1").arg(detail), 5000);
    });

    // Signal: Upload completed file IDs - mark files as uploaded
    connect(uploadManager, &UploadManager::uploadCompletedFileIds, mainWindow, [mainWindow](const QStringList& fileIds) {
        if (ApplicationRuntime::CanvasSession* session = mainWindow->sessionForActiveUpload()) {
            if (!session->canvas) return;
            for (const QString& fileId : fileIds) {
                if (session->upload.serverCompletedFileIds.contains(fileId)) continue;
                const QList<CanvasMedia*> items = session->upload.itemsByFileId.value(fileId);
                for (CanvasMedia* item : items) {
                    if (item) item->setUploadUploaded();
                }
                session->upload.serverCompletedFileIds.insert(fileId);
            }
        }
    });
    
    uploadSignalsConnected = true;
}
