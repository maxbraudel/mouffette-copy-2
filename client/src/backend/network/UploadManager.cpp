#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/files/FileManager.h"
#include "backend/domain/session/SessionManager.h"  // Phase 3: For DEFAULT_IDEA_ID constant
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
bool isVideoExtension(const QString& extension) {
    static const QSet<QString> kVideoExtensions = {
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("m4v"), QStringLiteral("mkv"),
        QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("wmv"), QStringLiteral("flv"),
        QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("3gp"), QStringLiteral("3g2"),
        QStringLiteral("ts"), QStringLiteral("m2ts"), QStringLiteral("mts")
    };
    return kVideoExtensions.contains(extension.trimmed().toLower());
}
}

// Removed dependency on ResizableMediaBase / scene scanning.

UploadManager::UploadManager(FileManager* fileManager, QObject* parent)
    : QObject(parent), m_fileManager(fileManager) {
    m_lastActionTime.start();
    
    // Setup debounce timer for action throttling
    m_actionDebounceTimer = new QTimer(this);
    m_actionDebounceTimer->setSingleShot(true);
    connect(m_actionDebounceTimer, &QTimer::timeout, this, [this]() {
        m_actionInProgress = false;
    });
}

void UploadManager::setWebSocketClient(WebSocketClient* client) { m_ws = client; }
void UploadManager::setTargetClientId(const QString& id) { m_targetClientId = id; }

void UploadManager::forceResetForClient(const QString& clientId) {
    if (!clientId.isEmpty()) {
        const bool matchesUploadTarget = (!m_uploadTargetClientId.isEmpty() && m_uploadTargetClientId == clientId);
        const bool matchesCurrentTarget = (!m_targetClientId.isEmpty() && m_targetClientId == clientId);
        if (!matchesUploadTarget && !matchesCurrentTarget && !m_uploadActive && !m_uploadInProgress && !m_finalizing) {
            return;
        }
    }

    resetToInitial();
    emit uiStateChanged();
}

void UploadManager::toggleUpload(const QVector<UploadFileInfo>& files) {
    if (!m_ws || !m_ws->isConnected() || m_targetClientId.isEmpty()) {
        qWarning() << "UploadManager: Not connected or no target set";
        return;
    }
    
    // Anti-spam protection: check if we can accept a new action
    if (!canAcceptNewAction()) {
        qInfo() << "UploadManager: Action ignored due to rate limiting";
        return;
    }
    
    if (m_cancelFinalizePending) {
        qInfo() << "UploadManager: Cancellation cleanup pending; toggle ignored";
        return;
    }
    
    // Block new actions while a critical operation is in progress
    if (m_actionInProgress) {
        qInfo() << "UploadManager: Action in progress, toggle ignored";
        return;
    }
    
    if (m_uploadActive) {
        // If active state but we are provided with additional files, start a new upload for them
        if (!files.isEmpty()) {
            startUpload(files);
            return;
        }
        // No new files: behave as unload toggle
        requestUnload();
        return;
    }
    if (m_uploadInProgress) { // cancel
        requestCancel();
        return;
    }
    if (files.isEmpty()) {
        qInfo() << "UploadManager: No files provided";
        return;
    }
    startUpload(files);
}

void UploadManager::requestRemoval(const QString& clientId) {
    if (!m_ws || !m_ws->isConnected() || clientId.isEmpty()) return;
    // Phase 3: canvasSessionId is MANDATORY - always set to DEFAULT_IDEA_ID at minimum
    if (m_activeIdeaId.isEmpty()) {
        qWarning() << "UploadManager: requestRemoval has empty canvasSessionId (should never happen), using DEFAULT_IDEA_ID";
        m_activeIdeaId = DEFAULT_IDEA_ID;
    }
    // Ensure subsequent all_files_removed callbacks attribute to the correct target
    m_uploadTargetClientId = clientId;
    m_lastRemovalClientId = clientId;
    m_ws->sendRemoveAllFiles(clientId, m_activeIdeaId);
}

void UploadManager::requestUnload() {
    const QString clientId = m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId;
    if (!m_uploadActive || clientId.isEmpty()) return;
    // Phase 3: canvasSessionId is MANDATORY - always set to DEFAULT_IDEA_ID at minimum
    if (m_activeIdeaId.isEmpty()) {
        qWarning() << "UploadManager: requestUnload has empty canvasSessionId (should never happen), using DEFAULT_IDEA_ID";
        m_activeIdeaId = DEFAULT_IDEA_ID;
    }
    
    // Mark action in progress to prevent spam
    scheduleActionDebounce();
    
    requestRemoval(clientId);
    // Don't reset state here - wait for onAllFilesRemovedRemote() callback
    emit uiStateChanged();
}

void UploadManager::requestCancel() {
    const QString clientId = m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId;
    if (!m_ws || !m_ws->isConnected() || clientId.isEmpty()) return;
    if (!m_uploadInProgress) return;
    if (m_cancelRequested) return;
    // Phase 3: canvasSessionId is MANDATORY - always set to DEFAULT_IDEA_ID at minimum
    if (m_activeIdeaId.isEmpty()) {
        qWarning() << "UploadManager: requestCancel has empty canvasSessionId (should never happen), using DEFAULT_IDEA_ID";
        m_activeIdeaId = DEFAULT_IDEA_ID;
    }
    
    // Mark action in progress to prevent spam
    scheduleActionDebounce();
    
    m_cancelRequested = true;
    m_cancelFinalizePending = true;
    if (!m_currentUploadId.isEmpty()) {
        m_ws->sendUploadAbort(clientId, m_currentUploadId, "User cancelled", m_activeIdeaId);
    }
    // Also request removal of all files to clean remote state
    requestRemoval(clientId);
    // We'll reset final state upon all_files_removed callback
    emit uiStateChanged();
    // Start fallback timer (3s) in case remote never responds
    if (!m_cancelFallbackTimer) {
        m_cancelFallbackTimer = new QTimer(this);
        m_cancelFallbackTimer->setSingleShot(true);
        connect(m_cancelFallbackTimer, &QTimer::timeout, this, [this]() {
            if (m_cancelFinalizePending) {
                finalizeLocalCancelState();
            }
        });
    }
    m_cancelFallbackTimer->start(3000);
}

void UploadManager::startUpload(const QVector<UploadFileInfo>& files) {
    // Prevent concurrent uploads
    if (m_uploadInProgress || m_finalizing) {
        qWarning() << "UploadManager: Upload already in progress, ignoring new start request";
        return;
    }
    // Phase 3: canvasSessionId is MANDATORY - always set to DEFAULT_IDEA_ID at minimum
    if (m_activeIdeaId.isEmpty()) {
        qWarning() << "UploadManager: startUpload has empty canvasSessionId (should never happen), using DEFAULT_IDEA_ID";
        m_activeIdeaId = DEFAULT_IDEA_ID;
    }
    
    // Mark action in progress to prevent spam
    scheduleActionDebounce();
    
    if (m_ws) {
        // Prepare the dedicated upload channel to avoid blocking control messages
        m_ws->beginUploadSession(true);
    }
    // Capture stable target id for the entire upload session
    m_uploadTargetClientId = m_targetClientId;
    m_currentUploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_uploadInProgress = true;
    m_cancelRequested = false;
    m_finalizing = false;
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = files.size();
    m_totalBytes = 0;
    m_sentBytes = 0;
    m_remoteProgressReceived = false;
    emit uiStateChanged();
    resetProgressTracking();

    // Build manifest with file deduplication info
    QJsonArray manifest;
    
    for (const auto& f : files) {
        QJsonObject obj;
        obj["fileId"] = f.fileId;
        obj["name"] = f.name;
        obj["extension"] = f.extension;
        obj["sizeBytes"] = static_cast<double>(f.size);
        
        // Include all mediaIds that use this fileId
        QList<QString> mediaIds = m_fileManager->getMediaIdsForFile(f.fileId);
        QJsonArray mediaIdArray;
        for (const QString& mediaId : mediaIds) {
            mediaIdArray.append(mediaId);
        }
        obj["mediaIds"] = mediaIdArray;
        
        
        
        manifest.append(obj);
        // accumulate for weighted progress
        if (f.size > 0) m_totalBytes += f.size;
    }
    m_ws->sendUploadStart(m_uploadTargetClientId, manifest, m_currentUploadId, m_activeIdeaId);

    // Stream sequentially (same logic as previously in MainWindow)
    m_outgoingFiles = files;
    const int chunkSize = 128 * 1024;
    for (const auto& f : files) {
        QFile file(f.path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        emit fileUploadStarted(f.fileId);
        qint64 sentForFile = 0;
        int chunkIndex = 0;
        while (!file.atEnd()) {
            if (m_cancelRequested) break;
            QByteArray chunk = file.read(chunkSize);
            m_ws->sendUploadChunk(m_uploadTargetClientId, m_currentUploadId, f.fileId, chunkIndex++, chunk.toBase64(), m_activeIdeaId);
            sentForFile += chunk.size();
            m_sentBytes += chunk.size();
            if (f.size > 0) {
                int p = static_cast<int>(std::round(sentForFile * 100.0 / static_cast<double>(f.size)));
                updatePerFileLocalProgress(f.fileId, p);
            }
            // Emit weighted global progress based on bytes, but do not exceed 99%
            if (m_totalBytes > 0) {
                int globalPercent = static_cast<int>(std::round(m_sentBytes * 100.0 / static_cast<double>(m_totalBytes)));
                globalPercent = std::clamp(globalPercent, 0, 99); // keep <100 until remote confirms
                int filesCompletedLocal = m_filesCompleted + (file.atEnd() ? 1 : 0);
                updateLocalProgress(globalPercent, filesCompletedLocal);
            }
            // Yield briefly
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        }
        file.close();
        if (!m_cancelRequested) {
            updatePerFileLocalProgress(f.fileId, 99);
            emit fileUploadFinished(f.fileId);
        }
        if (m_cancelRequested) break;
        // After a file is fully sent, update local filesCompleted
        if (!m_cancelRequested) {
            m_filesCompleted = std::min(m_filesCompleted + 1, m_totalFiles);
            if (m_totalBytes > 0) {
                int globalPercent = static_cast<int>(std::round(m_sentBytes * 100.0 / static_cast<double>(m_totalBytes)));
                globalPercent = std::clamp(globalPercent, 0, 99);
                updateLocalProgress(globalPercent, m_filesCompleted);
            } else {
                updateLocalProgress(m_lastLocalPercent, m_filesCompleted);
            }
        }
    }
    if (m_cancelRequested) {
        finalizeLocalCancelState();
        return;
    }
    m_ws->sendUploadComplete(m_uploadTargetClientId, m_currentUploadId, m_activeIdeaId);
    // We have sent all bytes; remain in uploading state until remote finishes
    // Enter finalizing only when we stop sending and await remote ack
    m_uploadInProgress = true;
    m_finalizing = false;
    
    // Clear action lock since upload streaming is complete
    m_actionInProgress = false;
    if (m_actionDebounceTimer) {
        m_actionDebounceTimer->stop();
    }
    
    emit uiStateChanged();
}

// collectSceneFiles removed; files now gathered by caller (MainWindow)

void UploadManager::resetToInitial() {
    m_uploadActive = false;
    m_uploadInProgress = false;
    m_cancelRequested = false;
    m_finalizing = false;
    m_cancelFinalizePending = false;
    m_actionInProgress = false;
    m_currentUploadId.clear();
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = 0;
    m_sentBytes = 0;
    m_totalBytes = 0;
    m_remoteProgressReceived = false;
    m_outgoingFiles.clear();
    resetProgressTracking();
    if (m_cancelFallbackTimer) m_cancelFallbackTimer->stop();
    if (m_actionDebounceTimer) m_actionDebounceTimer->stop();
    m_uploadTargetClientId.clear();
    m_activeSessionIdentity.clear();
    m_activeIdeaId.clear();
    if (m_ws) m_ws->endUploadSession();
}

void UploadManager::finalizeLocalCancelState() {
    if (!m_cancelFinalizePending) return;
    const QString targetId = !m_lastRemovalClientId.isEmpty()
                               ? m_lastRemovalClientId
                               : (!m_uploadTargetClientId.isEmpty() ? m_uploadTargetClientId : m_targetClientId);
    m_cancelFinalizePending = false;
    resetToInitial();
    m_lastRemovalClientId = targetId;
    if (!targetId.isEmpty()) {
        m_fileManager->unmarkAllForClient(targetId);
    }
    emit allFilesRemoved();
    emit uiStateChanged();
}

void UploadManager::resetProgressTracking() {
    m_lastLocalPercent = 0;
    m_lastLocalFilesCompleted = 0;
    m_lastRemotePercent = 0;
    m_lastRemoteFilesCompleted = 0;
    m_effectivePercent = -1;
    m_effectiveFilesCompleted = -1;
    m_localFilePercents.clear();
    m_remoteFilePercents.clear();
    m_effectiveFilePercents.clear();
}

void UploadManager::updateLocalProgress(int percent, int filesCompleted) {
    if (m_totalFiles <= 0) return;
    percent = std::clamp(percent, 0, 99);
    filesCompleted = std::clamp(filesCompleted, 0, m_totalFiles);
    if (percent > m_lastLocalPercent) {
        m_lastLocalPercent = percent;
    }
    if (filesCompleted > m_lastLocalFilesCompleted) {
        m_lastLocalFilesCompleted = filesCompleted;
    }
    emitEffectiveProgressIfChanged();
}

void UploadManager::updateRemoteProgress(int percent, int filesCompleted) {
    if (m_totalFiles <= 0) m_totalFiles = std::max(0, filesCompleted);
    percent = std::clamp(percent, 0, 100);
    filesCompleted = std::clamp(filesCompleted, 0, std::max(1, m_totalFiles));
    if (percent > m_lastRemotePercent) {
        m_lastRemotePercent = percent;
    }
    if (filesCompleted > m_lastRemoteFilesCompleted) {
        m_lastRemoteFilesCompleted = filesCompleted;
    }
    m_remoteProgressReceived = true;
    emitEffectiveProgressIfChanged();
}

void UploadManager::emitEffectiveProgressIfChanged() {
    if (m_totalFiles <= 0) return;
    const int effectivePercent = std::clamp(std::max(m_lastLocalPercent, m_lastRemotePercent), 0, m_remoteProgressReceived ? 100 : 99);
    const int effectiveFilesCompleted = std::clamp(m_remoteProgressReceived ? std::max(m_lastLocalFilesCompleted, m_lastRemoteFilesCompleted)
                                                                          : m_lastLocalFilesCompleted,
                                                  0,
                                                  m_totalFiles);
    if (effectivePercent == m_effectivePercent && effectiveFilesCompleted == m_effectiveFilesCompleted) {
        return;
    }
    m_effectivePercent = effectivePercent;
    m_effectiveFilesCompleted = effectiveFilesCompleted;
    emit uploadProgress(m_effectivePercent, m_effectiveFilesCompleted, m_totalFiles);
}

void UploadManager::updatePerFileLocalProgress(const QString& fileId, int percent) {
    if (fileId.isEmpty()) return;
    percent = std::clamp(percent, 0, 99);
    int& localEntry = m_localFilePercents[fileId];
    if (percent <= localEntry) return;
    localEntry = percent;
    emitEffectivePerFileProgress(fileId);
}

void UploadManager::updatePerFileRemoteProgress(const QString& fileId, int percent) {
    if (fileId.isEmpty()) return;
    percent = std::clamp(percent, 0, 100);
    int& remoteEntry = m_remoteFilePercents[fileId];
    if (percent <= remoteEntry) return;
    remoteEntry = percent;
    emitEffectivePerFileProgress(fileId);
}

void UploadManager::emitEffectivePerFileProgress(const QString& fileId) {
    const int local = m_localFilePercents.value(fileId, 0);
    const int remote = m_remoteFilePercents.value(fileId, 0);
    int effective = std::max(local, remote);
    if (remote >= 100) {
        effective = 100;
    } else {
        effective = std::clamp(effective, 0, 99);
    }
    int& cached = m_effectiveFilePercents[fileId];
    if (effective == cached) return;
    cached = effective;
    emit fileUploadProgress(fileId, effective);
}

void UploadManager::cleanupIncomingSession(bool deleteDiskContents,
                                           bool notifySender,
                                           const QString& senderOverride,
                                           const QString& cacheDirOverride,
                                           const QString& uploadIdOverride,
                                           const QString& ideaOverride) {
    auto dropChunkTracking = [this](const QString& uploadId) {
        if (uploadId.isEmpty()) return;
        const QString prefix = uploadId + ":";
        auto it = m_expectedChunkIndex.begin();
        while (it != m_expectedChunkIndex.end()) {
            if (it.key().startsWith(prefix)) {
                it = m_expectedChunkIndex.erase(it);
            } else {
                ++it;
            }
        }
        m_canceledIncoming.remove(uploadId);
    };

    QString senderId = senderOverride;
    QString cacheDirPath = cacheDirOverride;
    QString uploadId = uploadIdOverride;
    QString canvasSessionId = ideaOverride;
    QStringList fileIds;
    bool matchesActiveSession = false;

    if (!m_incoming.senderId.isEmpty()) {
        if (senderId.isEmpty() || senderId == m_incoming.senderId) {
            matchesActiveSession = true;
            senderId = m_incoming.senderId;
        }
    }

    if (matchesActiveSession) {
        if (uploadId.isEmpty()) uploadId = m_incoming.uploadId;
        if (cacheDirPath.isEmpty()) cacheDirPath = m_incoming.cacheDirPath;
        // Phase 3: canvasSessionId is MANDATORY - fallback to incoming canvasSessionId or DEFAULT_IDEA_ID
        if (canvasSessionId.isEmpty()) {
            canvasSessionId = m_incoming.canvasSessionId.isEmpty() ? DEFAULT_IDEA_ID : m_incoming.canvasSessionId;
        }

        for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
            if (it.value()) {
                it.value()->flush();
                it.value()->close();
                delete it.value();
            }
        }
        m_incoming.openFiles.clear();
        fileIds = m_incoming.expectedSizes.keys();

        dropChunkTracking(uploadId);

        m_incoming = IncomingUploadSession();
    } else {
        if (cacheDirPath.isEmpty() && !senderId.isEmpty()) {
            QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (base.isEmpty()) base = QDir::homePath() + "/.cache";
            cacheDirPath = base + "/Mouffette/Uploads/" + senderId;
        }
        if (uploadId.isEmpty()) uploadId = uploadIdOverride;
    }

    if (!uploadIdOverride.isEmpty() && uploadIdOverride != uploadId) {
        dropChunkTracking(uploadIdOverride);
    }

    // Phase 3: canvasSessionId is MANDATORY - check if it's a specific idea or default
    const bool ideaScoped = (canvasSessionId != DEFAULT_IDEA_ID);
    QSet<QString> removalIds;
    for (const QString& fid : fileIds) {
        if (!fid.isEmpty()) {
            removalIds.insert(fid);
        }
    }
    if (ideaScoped) {
        const QSet<QString> ideaFiles = m_fileManager->getFileIdsForIdea(canvasSessionId);
        removalIds.unite(ideaFiles);
    }

    if (!ideaScoped) {
        if (!cacheDirPath.isEmpty()) {
            m_fileManager->removeReceivedFileMappingsUnderPathPrefix(cacheDirPath + "/");
        } else if (!removalIds.isEmpty()) {
            for (const QString& fid : removalIds) {
                m_fileManager->removeReceivedFileMapping(fid);
            }
        }

        if (deleteDiskContents && !cacheDirPath.isEmpty()) {
            QDir dir(cacheDirPath);
            if (dir.exists()) {
                if (!dir.removeRecursively()) {
                    qWarning() << "UploadManager: Failed to remove cache directory during cleanup:" << cacheDirPath;
                } else {
                    qDebug() << "UploadManager: Removed cache directory during cleanup:" << cacheDirPath;
                }
            }
        }
    } else {
        for (const QString& fid : removalIds) {
            if (fid.isEmpty()) continue;

            m_fileManager->dissociateFileFromIdea(fid, canvasSessionId);
            const QSet<QString> remainingIdeas = m_fileManager->getIdeaIdsForFile(fid);
            if (!remainingIdeas.isEmpty()) {
                continue; // keep file for other ideas still referencing it
            }

            const QString path = m_fileManager->getFilePathForId(fid);
            if (deleteDiskContents && !path.isEmpty()) {
                QFileInfo info(path);
                if (info.exists()) {
                    QFile file(path);
                    if (!file.remove()) {
                        qWarning() << "UploadManager: Failed to remove cached file" << path << "for idea" << canvasSessionId;
                    } else {
                        qDebug() << "UploadManager: Removed cached file" << path << "for idea" << canvasSessionId;
                    }
                }
            }
            m_fileManager->removeReceivedFileMapping(fid);
        }

        if (deleteDiskContents && !cacheDirPath.isEmpty()) {
            QDir dir(cacheDirPath);
            if (dir.exists() && dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
                if (!dir.rmdir(cacheDirPath)) {
                    qWarning() << "UploadManager: Failed to remove empty cache directory" << cacheDirPath;
                }
            }
        }
    }

    if (notifySender && m_ws && !senderId.isEmpty()) {
        m_ws->notifyAllFilesRemovedToSender(senderId);
    }

    if (!matchesActiveSession && !uploadId.isEmpty()) {
        dropChunkTracking(uploadId);
    }
}

// Slots forwarded from WebSocketClient (sender side)
void UploadManager::onUploadProgress(const QString& uploadId, int percent, int filesCompleted, int totalFiles) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    // Always accept target-side progress; it's authoritative
    m_lastPercent = percent;
    m_filesCompleted = filesCompleted;
    if (totalFiles > 0) {
        m_totalFiles = totalFiles;
    }
    updateRemoteProgress(percent, filesCompleted);
}

void UploadManager::onUploadCompletedFileIds(const QString& uploadId, const QStringList& fileIds) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    if (fileIds.isEmpty()) return;
    emit uploadCompletedFileIds(fileIds);
    for (const QString& fid : fileIds) {
        updatePerFileRemoteProgress(fid, 100);
    }
}

void UploadManager::onUploadFinished(const QString& uploadId) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    updateRemoteProgress(100, m_totalFiles > 0 ? m_totalFiles : m_filesCompleted);
    // Switch to finalizing for a brief moment to align UI state, then finish
    m_uploadInProgress = false;
    m_finalizing = true;
    emit uiStateChanged();
    
    // Mark all uploaded files and media as available on the target client
    for (const auto& f : m_outgoingFiles) {
        m_fileManager->markFileUploadedToClient(f.fileId, m_uploadTargetClientId);
        // File-based tracking covers all media instances
        const QList<QString> mediaIds = m_fileManager->getMediaIdsForFile(f.fileId);
    }
    
    m_uploadActive = true; // switch to active state
    m_uploadInProgress = false;
    m_finalizing = false; // finalization complete
    m_actionInProgress = false; // Clear action lock
    emit uploadFinished();
    emit uiStateChanged();
    if (m_ws) m_ws->endUploadSession();
}

void UploadManager::onAllFilesRemovedRemote() {
    if (m_cancelFinalizePending) {
        finalizeLocalCancelState();
        return;
    }

    // Remote side confirmed unload; reset state
    // Clear all uploaded markers for this client so that all items are considered Not uploaded
    const QString removedClientId = !m_lastRemovalClientId.isEmpty()
                                        ? m_lastRemovalClientId
                                        : (!m_uploadTargetClientId.isEmpty() ? m_uploadTargetClientId : m_targetClientId);
    if (!removedClientId.isEmpty()) {
        m_fileManager->unmarkAllForClient(removedClientId);
    }

    // Now reset state
    resetToInitial();

    m_lastRemovalClientId = removedClientId;
    m_actionInProgress = false; // Clear action lock after removal confirmed

    emit allFilesRemoved();
    emit uiStateChanged();
}

void UploadManager::onConnectionLost() {
    // If we were uploading or finalizing, treat it as an aborted session.
    const bool hadOngoing = m_uploadInProgress || m_finalizing;

    if (hadOngoing) {
        // Cancel local flags immediately
        m_cancelRequested = true;
        m_uploadInProgress = false;
        m_finalizing = false;

        // Do not mark anything as uploaded; roll back any optimistic UI
        // Unmark any files that were part of the outgoing batch but not yet confirmed by onUploadFinished
        if (!m_uploadTargetClientId.isEmpty()) {
            for (const auto& f : m_outgoingFiles) {
                m_fileManager->unmarkFileUploadedToClient(f.fileId, m_uploadTargetClientId);
                // File-based unmark covers all media instances
            }
        }

        // Notify UI to recompute button state and progress text
        emit uiStateChanged();

        // Leave m_uploadActive = false so next click starts a fresh upload.
        m_uploadActive = false;
        m_currentUploadId.clear();
        m_lastPercent = 0;
        m_filesCompleted = 0;
        m_totalFiles = 0;
        m_sentBytes = 0;
        m_totalBytes = 0;
        m_remoteProgressReceived = false;
        m_outgoingFiles.clear();
    }

    if (m_ws) {
        m_ws->endUploadSession();
    }

    cleanupIncomingCacheForConnectionLoss();
}

void UploadManager::cleanupIncomingCacheForConnectionLoss() {
    // Reset any active incoming session and associated bookkeeping, then remove cached files on disk.
    if (!m_incoming.senderId.isEmpty()) {
        qDebug() << "UploadManager: Clearing incoming upload cache for sender" << m_incoming.senderId << "after connection loss";
    }
    m_incoming = IncomingUploadSession();
    m_expectedChunkIndex.clear();
    m_canceledIncoming.clear();

    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.cache";
    const QString uploadsRoot = base + "/Mouffette/Uploads";
    QDir uploadsDir(uploadsRoot);
    if (!uploadsDir.exists()) {
        return;
    }

    if (!uploadsDir.removeRecursively()) {
        qWarning() << "UploadManager: Failed to remove uploads cache folder after connection loss:" << uploadsRoot;
    } else {
        qDebug() << "UploadManager: Cleared uploads cache folder after connection loss:" << uploadsRoot;
    }

    m_fileManager->removeReceivedFileMappingsUnderPathPrefix(uploadsRoot + "/");
}

// Incoming side (target) - replicate subset of MainWindow logic for assembling files
void UploadManager::handleIncomingMessage(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    if (type == "upload_start") {
        m_incoming = IncomingUploadSession();
        // Reset per-session chunk ordering state
        m_expectedChunkIndex.clear();
        m_incoming.senderId = message.value("senderClientId").toString();
        m_incoming.uploadId = message.value("uploadId").toString();
        
        // CRITICAL FIX: Use the directional canvasSessionId sent by the sender
        // The sender already generated: "senderClient_TO_targetClient_canvas_uuid"
        // We must use the SAME ID to maintain session consistency
        m_incoming.canvasSessionId = message.value("canvasSessionId").toString();
        
        qDebug() << "UploadManager: Received directional canvasSessionId:" << m_incoming.canvasSessionId;
        
        m_canceledIncoming.remove(m_incoming.uploadId);
        QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (base.isEmpty()) base = QDir::homePath() + "/.cache";
        QDir dir(base); dir.mkpath("Mouffette/Uploads");
        // Use sender client ID as folder name instead of upload ID
        QString cacheDir = base + "/Mouffette/Uploads/" + m_incoming.senderId;
        QDir().mkpath(cacheDir);
        m_incoming.cacheDirPath = cacheDir;
        
        qDebug() << "UploadManager: Creating upload folder:" << cacheDir;
        qDebug() << "UploadManager: Sender ID:" << m_incoming.senderId;
        QJsonArray files = message.value("files").toArray();
        m_incoming.totalFiles = files.size();
        for (const QJsonValue& v : files) {
            QJsonObject f = v.toObject();
            QString fileId = f.value("fileId").toString();
            QString name = f.value("name").toString();
            QString extension = f.value("extension").toString();
            QJsonArray mediaIdsArray = f.value("mediaIds").toArray();
            qint64 size = static_cast<qint64>(f.value("sizeBytes").toDouble());
            m_incoming.totalSize += qMax<qint64>(0, size);
            if (!extension.isEmpty()) {
                m_incoming.fileIdToExtension.insert(fileId, extension.toLower());
            } else {
                m_incoming.fileIdToExtension.insert(fileId, QString());
            }
            
            // Store all mediaIds for this fileId
            QStringList mediaIdsList;
            for (const QJsonValue& mediaIdVal : mediaIdsArray) {
                QString mediaId = mediaIdVal.toString();
                mediaIdsList.append(mediaId);
                m_incoming.fileIdToMediaId.insert(fileId, mediaId);
            }
            
            // Use fileId as filename with original extension
            QString filename = fileId;
            if (!extension.isEmpty()) {
                filename += "." + extension;
            }
            QString fullPath = cacheDir + "/" + filename;
            qDebug() << "UploadManager: Creating file:" << fullPath;
            qDebug() << "UploadManager: File ID:" << fileId;
            QFile* qf = new QFile(fullPath);
            if (!qf->open(QIODevice::WriteOnly)) { delete qf; continue; }
            m_incoming.openFiles.insert(fileId, qf);
            m_incoming.expectedSizes.insert(fileId, qMax<qint64>(0, size));
            m_incoming.receivedByFile.insert(fileId, 0);
            // Register mapping so remote scene resolution can find this fileId immediately (even before complete)
            m_fileManager->registerReceivedFilePath(fileId, fullPath);
            // Phase 3: canvasSessionId is MANDATORY - associate with idea (even if DEFAULT_IDEA_ID)
            if (m_incoming.canvasSessionId != DEFAULT_IDEA_ID) {
                m_fileManager->associateFileWithIdea(fileId, m_incoming.canvasSessionId);
            }
            // Initialize expected chunk index for this file to 0
            m_expectedChunkIndex.insert(m_incoming.uploadId + ":" + fileId, 0);
        }
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            m_ws->notifyUploadProgressToSender(m_incoming.senderId, m_incoming.uploadId, 0, 0, m_incoming.totalFiles, QStringList());
        }
    } else if (type == "upload_chunk") {
        if (message.value("uploadId").toString() != m_incoming.uploadId) return;
        const QString canvasSessionId = message.value("canvasSessionId").toString();
        // Phase 3: canvasSessionId matching - compare against incoming canvasSessionId (both should be set)
        if (!canvasSessionId.isEmpty() && m_incoming.canvasSessionId != DEFAULT_IDEA_ID && canvasSessionId != m_incoming.canvasSessionId) {
            qWarning() << "UploadManager: Ignoring chunk for mismatched idea" << canvasSessionId << "expected" << m_incoming.canvasSessionId;
            return;
        }
        if (m_canceledIncoming.contains(m_incoming.uploadId)) return;
        QString fid = message.value("fileId").toString();
        QFile* qf = m_incoming.openFiles.value(fid, nullptr);
        if (!qf) return;
        
        // CRITICAL FIX: Handle chunk ordering properly per upload session
        int chunkIndex = message.value("chunkIndex").toInt();
        QByteArray data = QByteArray::fromBase64(message.value("data").toString().toUtf8());

        const QString key = m_incoming.uploadId + ":" + fid;
        if (!m_expectedChunkIndex.contains(key)) {
            m_expectedChunkIndex.insert(key, 0);
        }
        const int expected = m_expectedChunkIndex.value(key);
        if (chunkIndex != expected) {
            qWarning() << "UploadManager: Out-of-order chunk for" << fid
                       << "(upload" << m_incoming.uploadId << ") - expected" << expected
                       << "got" << chunkIndex << "- dropping to prevent corruption";
            return;
        }
        m_expectedChunkIndex[key] = expected + 1;

        // Ensure we are at end for append (defensive in case of any reuse)
        qf->seek(qf->size());
        qint64 written = qf->write(data);
        if (written != data.size()) {
            qWarning() << "UploadManager: Partial write - expected" << data.size() << "wrote" << written;
        }
        qf->flush(); // Ensure data is written immediately
        
        m_incoming.received += data.size();
        if (m_incoming.receivedByFile.contains(fid)) {
            qint64 soFar = m_incoming.receivedByFile.value(fid) + data.size();
            m_incoming.receivedByFile[fid] = soFar;
        }
        int filesCompleted = 0;
        QStringList completedIds;
        for (auto it = m_incoming.expectedSizes.constBegin(); it != m_incoming.expectedSizes.constEnd(); ++it) {
            qint64 expected = it.value();
            qint64 got = m_incoming.receivedByFile.value(it.key(), 0);
            if (expected > 0 && got >= expected) { filesCompleted++; completedIds.append(it.key()); }
        }
        if (m_ws && !m_incoming.senderId.isEmpty() && m_incoming.totalSize > 0) {
            int percent = static_cast<int>(std::round(m_incoming.received * 100.0 / m_incoming.totalSize));
            // Build per-file progress array (only for files in progress to reduce payload)
            QJsonArray perFileArr;
            // Include the file that just received data
            if (m_incoming.expectedSizes.contains(fid)) {
                const qint64 expected = m_incoming.expectedSizes.value(fid);
                const qint64 got = m_incoming.receivedByFile.value(fid, 0);
                int pf = 0;
                if (expected > 0) pf = static_cast<int>(std::round(got * 100.0 / expected));
                QJsonObject o; o["fileId"] = fid; o["percent"] = pf; perFileArr.append(o);
            }
            m_ws->notifyUploadProgressToSender(m_incoming.senderId, m_incoming.uploadId, percent, filesCompleted, m_incoming.totalFiles, completedIds, perFileArr);
        }
    } else if (type == "upload_complete") {
        if (message.value("uploadId").toString() != m_incoming.uploadId) return;
        const QString canvasSessionId = message.value("canvasSessionId").toString();
        // Phase 3: canvasSessionId matching - compare against incoming canvasSessionId (both should be set)
        if (!canvasSessionId.isEmpty() && m_incoming.canvasSessionId != DEFAULT_IDEA_ID && canvasSessionId != m_incoming.canvasSessionId) {
            qWarning() << "UploadManager: Ignoring upload_complete for mismatched idea" << canvasSessionId << "expected" << m_incoming.canvasSessionId;
            return;
        }
        
        // Clean up chunk tracking before closing files (for this upload)
        const QString prefix = m_incoming.uploadId + ":";
        auto itKey = m_expectedChunkIndex.begin();
        while (itKey != m_expectedChunkIndex.end()) {
            if (itKey.key().startsWith(prefix)) itKey = m_expectedChunkIndex.erase(itKey); else ++itKey;
        }
        
        for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
            if (it.value()) { it.value()->flush(); it.value()->close(); delete it.value(); }
        }
        m_incoming.openFiles.clear();

        // Preload completed video files into RAM for low-latency playback
        for (auto it = m_incoming.fileIdToExtension.constBegin(); it != m_incoming.fileIdToExtension.constEnd(); ++it) {
            const QString& fileId = it.key();
            const QString& ext = it.value();
            if (isVideoExtension(ext)) {
                m_fileManager->preloadFileIntoMemory(fileId);
            }
        }
        m_incoming.fileIdToExtension.clear();
        // Send a final 100% progress update to the sender to ensure UI reaches 100 only when target is fully done
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            const int finalPercent = 100;
            const int filesCompleted = m_incoming.totalFiles;
            // Include all fileIds as completed and per-file 100
            QStringList allIds = m_incoming.expectedSizes.keys();
            QJsonArray perFileArr;
            for (const QString& fidAll : allIds) { QJsonObject o; o["fileId"] = fidAll; o["percent"] = 100; perFileArr.append(o); }
            m_ws->notifyUploadProgressToSender(m_incoming.senderId, m_incoming.uploadId, finalPercent, filesCompleted, m_incoming.totalFiles, allIds, perFileArr);
        }
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            m_ws->notifyUploadFinishedToSender(m_incoming.senderId, m_incoming.uploadId);
        }
    } else if (type == "upload_abort") {
        const QString abortedId = message.value("uploadId").toString();
        const QString senderClientId = message.value("senderClientId").toString();
        const QString canvasSessionId = message.value("canvasSessionId").toString();
        if (!abortedId.isEmpty()) {
            m_canceledIncoming.insert(abortedId);
        }

        QString ackTarget = !m_incoming.senderId.isEmpty() ? m_incoming.senderId : senderClientId;
        if (m_ws && !ackTarget.isEmpty()) {
            m_ws->notifyAllFilesRemovedToSender(ackTarget);
        }

        QString cacheOverride = m_incoming.cacheDirPath;
        if (cacheOverride.isEmpty() && !ackTarget.isEmpty()) {
            QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (base.isEmpty()) base = QDir::homePath() + "/.cache";
            cacheOverride = base + "/Mouffette/Uploads/" + ackTarget;
        }

        cleanupIncomingSession(true, false, ackTarget, cacheOverride, abortedId, canvasSessionId);
    } else if (type == "remove_all_files") {
        const QString senderClientId = message.value("senderClientId").toString();
        const QString canvasSessionId = message.value("canvasSessionId").toString();
        QString ackTarget = !senderClientId.isEmpty() ? senderClientId : m_incoming.senderId;
        if (m_ws && !ackTarget.isEmpty()) {
            m_ws->notifyAllFilesRemovedToSender(ackTarget);
        }

        QString cacheOverride = m_incoming.cacheDirPath;
        if (cacheOverride.isEmpty() && !ackTarget.isEmpty()) {
            QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (base.isEmpty()) base = QDir::homePath() + "/.cache";
            cacheOverride = base + "/Mouffette/Uploads/" + ackTarget;
        }

        cleanupIncomingSession(true, false, ackTarget, cacheOverride, QString(), canvasSessionId);
        // Clear all expected indices; treat as a hard reset
        m_expectedChunkIndex.clear();
    } else if (type == "connection_lost_cleanup") {
        // Optional: sender notified us to clean any partials; delete cache folder for that sender
        QString senderClientId = message.value("senderClientId").toString();
        if (!senderClientId.isEmpty()) {
            QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (base.isEmpty()) base = QDir::homePath() + "/.cache";
            QString dirPath = base + "/Mouffette/Uploads/" + senderClientId;
            QDir dir(dirPath);
            if (dir.exists()) {
                dir.removeRecursively();
            }
        }
    } else if (type == "remove_file") {
        const QString senderClientId = message.value("senderClientId").toString();
        const QString fileId = message.value("fileId").toString();
        const QString canvasSessionId = message.value("canvasSessionId").toString();

        if (!senderClientId.isEmpty() && !fileId.isEmpty()) {
            bool shouldRemoveFromDisk = true;
            // Phase 3: canvasSessionId is MANDATORY - check if it's a specific idea (not DEFAULT_IDEA_ID)
            if (canvasSessionId != DEFAULT_IDEA_ID) {
                m_fileManager->dissociateFileFromIdea(fileId, canvasSessionId);
                const QSet<QString> remainingIdeas = m_fileManager->getIdeaIdsForFile(fileId);
                shouldRemoveFromDisk = remainingIdeas.isEmpty();
            }

            if (!shouldRemoveFromDisk) {
                qDebug() << "UploadManager: Retaining file" << fileId << "because other ideas still reference it";
                return;
            }

            QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (base.isEmpty()) base = QDir::homePath() + "/.cache";
            QString dirPath = base + "/Mouffette/Uploads/" + senderClientId;

            bool removedAny = false;
            const QString mappedPath = m_fileManager->getFilePathForId(fileId);
            if (!mappedPath.isEmpty()) {
                QFileInfo mappedInfo(mappedPath);
                if (mappedInfo.exists()) {
                    QFile file(mappedPath);
                    if (file.remove()) {
                        qDebug() << "UploadManager: Removed mapped cached file" << mappedPath;
                        removedAny = true;
                    } else {
                        qWarning() << "UploadManager: Failed to remove mapped cached file" << mappedPath;
                    }
                }
            }

            QDir dir(dirPath);
            if (dir.exists() && !removedAny) {
                QStringList nameFilters;
                nameFilters << fileId + "*";
                QFileInfoList files = dir.entryInfoList(nameFilters, QDir::Files);
                for (const QFileInfo& fileInfo : files) {
                    QString filePath = fileInfo.absoluteFilePath();
                    QFile file(filePath);
                    if (file.remove()) {
                        qDebug() << "UploadManager: Removed cached file" << filePath;
                        removedAny = true;
                    } else {
                        qWarning() << "UploadManager: Failed to remove cached file" << filePath;
                    }
                }

                if (files.isEmpty()) {
                    qDebug() << "UploadManager: No cached files found matching fileId" << fileId;
                }

                if (removedAny) {
                    QFileInfoList remainingFiles = dir.entryInfoList(QDir::Files);
                    if (remainingFiles.isEmpty()) {
                        if (dir.rmdir(dirPath)) {
                            qDebug() << "UploadManager: Removed empty directory" << dirPath;
                        }
                    }
                }
            }

            m_fileManager->removeReceivedFileMapping(fileId);
        }
    }
}

bool UploadManager::canAcceptNewAction() const {
    // Check minimum time interval between actions
    if (m_lastActionTime.isValid() && m_lastActionTime.elapsed() < MIN_ACTION_INTERVAL_MS) {
        return false;
    }
    
    // Check if an action is currently in progress
    if (m_actionInProgress) {
        return false;
    }
    
    return true;
}

void UploadManager::scheduleActionDebounce() {
    m_actionInProgress = true;
    m_lastActionTime.restart();
    
    if (m_actionDebounceTimer) {
        m_actionDebounceTimer->stop();
        m_actionDebounceTimer->start(ACTION_DEBOUNCE_MS);
    }
}
