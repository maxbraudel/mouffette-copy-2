#include "UploadSignalConnector.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/media/CanvasMedia.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QHash>
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <algorithm>

namespace {
QList<CanvasMedia*> currentMediaForSource(ApplicationRuntime::ClientWorkspace* session,
                                         const QString& fileId)
{
    QList<CanvasMedia*> matches;
    if (!session || !session->canvas) return matches;
    for (auto* media : session->canvas->enumerateMediaItems())
        if (media && media->fileId() == fileId) matches.append(media);
    return matches;
}

QString uploadFailureDetail(const QString& reason)
{
    const QString detail = reason.trimmed();
    if (detail.isEmpty()) return QStringLiteral("The remote computer rejected the upload. Please try again.");
    static const QRegularExpression internalCode(QStringLiteral("^[a-zA-Z][a-zA-Z0-9]*_[a-zA-Z0-9_]+$"));
    if (!internalCode.match(detail).hasMatch()) return detail;
    if (detail == QLatin1String("remote_session_unavailable")
        || detail == QLatin1String("unknown_remote_session")
        || detail == QLatin1String("lease_expired")
        || detail == QLatin1String("session_terminal"))
        return QStringLiteral("The remote session is no longer available. Reconnect to the remote computer and try again.");
    if (detail == QLatin1String("remote_session_reconnecting")
        || detail == QLatin1String("session_requires_resume")
        || detail == QLatin1String("remote_session_not_active"))
        return QStringLiteral("The remote connection is recovering. Please wait before retrying the upload.");
    if (detail == QLatin1String("session_cleanup_pending")
        || detail == QLatin1String("cleanup_not_committed"))
        return QStringLiteral("The remote computer is still cleaning up the previous session. Please wait before retrying.");
    if (detail == QLatin1String("target_offline") || detail == QLatin1String("target_unavailable"))
        return QStringLiteral("The remote computer is offline. Check its connection and try again.");
    return QStringLiteral("The upload could not be completed. Please try again once the remote connection is ready.");
}

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
        if (ApplicationRuntime::ClientWorkspace* session = mainWindow->workspaceForActiveUpload()) {
            if (!session->canvas) return;
            session->upload.perFileProgress[fileId] = 0;
            const QList<CanvasMedia*> items = currentMediaForSource(session, fileId);
            for (CanvasMedia* item : items) {
                if (item && item->uploadState() != CanvasMedia::UploadState::Uploaded) {
                    item->setUploadUploading(0);
                }
            }
        }
    });
    
    // Signal: File upload progress - update upload progress on items
    connect(uploadManager, &UploadManager::fileUploadProgress, mainWindow, [mainWindow](const QString& fileId, int percent) {
        if (ApplicationRuntime::ClientWorkspace* session = mainWindow->workspaceForActiveUpload()) {
            if (!session->canvas) return;
            if (percent >= 100) {
                session->upload.perFileProgress[fileId] = 100;
                const QList<CanvasMedia*> items = currentMediaForSource(session, fileId);
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
            const QList<CanvasMedia*> items = currentMediaForSource(session, fileId);
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
        if (ApplicationRuntime::ClientWorkspace* session = mainWindow->workspaceForUploadId(uploadId)) {
            const QString label = session->lastClientInfo.getDisplayText().isEmpty()
                ? session->targetEndpointId
                : session->lastClientInfo.getDisplayText();
            publishTerminalUploadNotification(
                uploadId, NotificationSeverity::Success,
                QStringLiteral("Upload completed successfully to %1").arg(label));
            session->upload.remoteFilesPresent = true;
            session->knownRemoteFileIds.unite(session->upload.fileIds);
            mainWindow->clearUploadTracking(*session);
        } else {
            publishTerminalUploadNotification(
                uploadId, NotificationSeverity::Success,
                QStringLiteral("Upload completed successfully"));
        }
    });

    connect(uploadManager, &UploadManager::uploadCancelled, mainWindow,
            [mainWindow](const QString& uploadId) {
        if (ApplicationRuntime::ClientWorkspace* session = mainWindow->workspaceForUploadId(uploadId)) {
            for (const QString& fileId : session->upload.fileIds) {
                for (CanvasMedia* item : currentMediaForSource(session, fileId)) {
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
        ApplicationRuntime::ClientWorkspace* session = mainWindow->workspaceForUploadId(uploadId);
        if (session) {
            for (const QString& fileId : session->upload.fileIds) {
                for (CanvasMedia* item : currentMediaForSource(session, fileId)) {
                    if (item) item->setUploadNotUploaded();
                }
            }
            session->upload.remoteFilesPresent = !session->knownRemoteFileIds.isEmpty();
            mainWindow->clearUploadTracking(*session);
        }

        const QString detail = uploadFailureDetail(reason);
        publishTerminalUploadNotification(
            uploadId, NotificationSeverity::Error,
            QStringLiteral("Upload failed: %1").arg(detail),
            AppConfig::instance().toastErrorDurationMs());
    });

    // Signal: Upload completed file IDs - mark files as uploaded
    connect(uploadManager, &UploadManager::uploadCompletedFileIds, mainWindow, [mainWindow](const QStringList& fileIds) {
        if (ApplicationRuntime::ClientWorkspace* session = mainWindow->workspaceForActiveUpload()) {
            if (!session->canvas) return;
            for (const QString& fileId : fileIds) {
                if (session->upload.serverCompletedFileIds.contains(fileId)) continue;
                const QList<CanvasMedia*> items = currentMediaForSource(session, fileId);
                for (CanvasMedia* item : items) {
                    if (item) item->setUploadUploaded();
                }
                session->upload.serverCompletedFileIds.insert(fileId);
            }
        }
    });
    
    uploadSignalsConnected = true;
}
