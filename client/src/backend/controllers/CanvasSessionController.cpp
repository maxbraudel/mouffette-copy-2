#include "CanvasSessionController.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/domain/models/ClientInfo.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/session/SessionManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/UploadManager.h"
#include "backend/files/FileManager.h"
#include "backend/files/FileWatcher.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QTimer>
#include <QDebug>

ClientWorkspaceController::ClientWorkspaceController(ApplicationRuntime* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

// ============================================================================
// Workspace Lookup Methods
// ============================================================================

void* ClientWorkspaceController::findCanvasSession(const QString& persistentClientId) {
    return m_mainWindow->getSessionManager()->findSession(persistentClientId);
}

const void* ClientWorkspaceController::findCanvasSession(const QString& persistentClientId) const {
    return m_mainWindow->getSessionManager()->findSession(persistentClientId);
}

void* ClientWorkspaceController::findCanvasSessionByServerClientId(const QString& serverClientId) {
    return m_mainWindow->getSessionManager()->findSessionByServerClientId(serverClientId);
}

const void* ClientWorkspaceController::findCanvasSessionByServerClientId(const QString& serverClientId) const {
    return m_mainWindow->getSessionManager()->findSessionByServerClientId(serverClientId);
}

void* ClientWorkspaceController::findCanvasSessionByIdeaId(const QString& canvasSessionId) {
    return m_mainWindow->getSessionManager()->findSessionByIdeaId(canvasSessionId);
}

// ============================================================================
// Workspace Lifecycle
// ============================================================================

void* ClientWorkspaceController::ensureCanvasSession(const ClientInfo& client) {
    QString persistentId = client.endpointId();
    if (persistentId.isEmpty()) {
        qWarning() << "ClientWorkspaceController::ensureCanvasSession: client has no persistentClientId, this should not happen";
        persistentId = client.getId();
    }
    
    // Check if session already exists
    // Use SessionManager (creates canvasSessionId automatically)
    ApplicationRuntime::CanvasSession& session = m_mainWindow->getSessionManager()->getOrCreateSession(persistentId, client);
    
    // Initialize canvas if needed (UI-specific responsibility)
    if (!session.canvas) {
        if (m_prewarmedQuickCanvasHost) {
            session.canvas = m_prewarmedQuickCanvasHost;
            m_prewarmedQuickCanvasHost = nullptr;
        } else {
            QString quickError;
            session.canvas = QuickCanvasHost::create(&quickError);
            if (session.canvas) {
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

        if (!session.canvas) {
            return &session;
        }

        session.canvas->setActiveIdeaId(session.canvasSessionId); // Use canvasSessionId from SessionManager
        session.connectionsInitialized = false;
        configureCanvasSession(&session);
    }
    
    // Update remote target
    if (!session.persistentClientId.isEmpty()) {
        session.canvas->setRemoteSceneTarget(session.persistentClientId, session.lastClientInfo.getMachineName());
    }
    
    // Update online status
    if (session.lastClientInfo.isOnline()) {
        session.remoteContentClearedOnDisconnect = false;
    }
    
    return &session;
}

void ClientWorkspaceController::prewarmQuickCanvasHost() {
    if (!m_mainWindow || m_prewarmedQuickCanvasHost) {
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

void ClientWorkspaceController::configureCanvasSession(void* sessionPtr) {
    ApplicationRuntime::CanvasSession* session = static_cast<ApplicationRuntime::CanvasSession*>(sessionPtr);
    if (!session || !session->canvas) return;

    session->canvas->setActiveIdeaId(session->canvasSessionId);
    session->canvas->setWebSocketClient(m_mainWindow->getWebSocketClient());
    session->canvas->setUploadManager(m_mainWindow->getUploadManager());
    session->canvas->setFileManager(m_mainWindow->getFileManager());
    // Connect to ApplicationRuntime signal via direct call (onRemoteSceneLaunchStateChanged is private)
    connect(session->canvas, &ICanvasHost::remoteSceneLaunchStateChanged, m_mainWindow,
            &ApplicationRuntime::onRemoteSceneLaunchStateChanged,
            Qt::UniqueConnection);

    if (!session->connectionsInitialized) {
        auto* projectAutosaveTimer = new QTimer(session->canvas);
        projectAutosaveTimer->setObjectName(
            QStringLiteral("projectAutosave_%1").arg(session->persistentClientId));
        projectAutosaveTimer->setSingleShot(true);
        projectAutosaveTimer->setInterval(300);
        connect(projectAutosaveTimer, &QTimer::timeout, m_mainWindow,
                [this, persistentId=session->persistentClientId]() {
            if (m_mainWindow) {
                m_mainWindow->persistProjectCanvas(persistentId);
            }
        });
        if (session->canvas->document()) {
            connect(session->canvas->document(), &CanvasDocument::documentChanged,
                    projectAutosaveTimer, [projectAutosaveTimer] {
                        projectAutosaveTimer->start();
                    });
        }
        connect(session->canvas, &ICanvasHost::mediaItemAdded, m_mainWindow,
                [this, persistentId=session->persistentClientId](CanvasMedia* mediaItem) {
                    if (m_mainWindow->getFileWatcher() && mediaItem && !mediaItem->sourcePath().isEmpty()) {
                        m_mainWindow->getFileWatcher()->watchMediaItem(mediaItem);
                        qDebug() << "ClientWorkspaceController: source watch added for mediaId"
                                 << mediaItem->mediaId();
                    }
                    ApplicationRuntime::CanvasSession* sess = m_mainWindow->getSessionManager()->findSession(persistentId);
                    if (sess) {
                        sess->lastClientInfo.setFromMemory(true);
                    }
                    // Update upload button state immediately when media is added
                    if (m_mainWindow->getUploadManager()) {
                        emit m_mainWindow->getUploadManager()->uiStateChanged();
                    }
                    if (m_mainWindow->getAutoUploadImportedMedia() && m_mainWindow->getUploadManager() && 
                        !m_mainWindow->getUploadManager()->isUploading() && !m_mainWindow->getUploadManager()->isCancelling()) {
                        QTimer::singleShot(0, m_mainWindow, [this]() { m_mainWindow->onUploadButtonClicked(); });
                    }
                });
        
        connect(session->canvas, &ICanvasHost::mediaItemRemoved, m_mainWindow,
                [this, persistentId=session->persistentClientId](CanvasMedia* mediaItem) {
                    if (m_mainWindow->getFileWatcher() && mediaItem) {
                        m_mainWindow->getFileWatcher()->unwatchMediaItem(mediaItem);
                    }
                    // Update upload button state immediately when media is removed
                    if (m_mainWindow->getUploadManager()) {
                        emit m_mainWindow->getUploadManager()->uiStateChanged();
                    }
                    if (m_mainWindow) {
                        QTimer::singleShot(0, m_mainWindow,
                            [this, persistentId]() {
                                if (m_mainWindow) {
                                    m_mainWindow->persistProjectCanvas(persistentId);
                                }
                            });
                    }
                });
    }

    session->connectionsInitialized = true;
}

void ClientWorkspaceController::switchToCanvasSession(const QString& persistentClientId) {
    // Navigation between clients should NOT trigger unload - uploads persist per session
    // Unload only happens when explicitly requested via button or when remote disconnects
    
    void* sessionPtr = findCanvasSession(persistentClientId);
    ApplicationRuntime::CanvasSession* session = static_cast<ApplicationRuntime::CanvasSession*>(sessionPtr);
    if (!session || !session->canvas) return;

    m_mainWindow->setActiveSessionIdentity(persistentClientId);
    m_mainWindow->setActiveCanvas(session->canvas);
    if (m_mainWindow->getNavigationManager()) {
        m_mainWindow->getNavigationManager()->setActiveCanvas(session->canvas);
    }

    // Use persistentClientId for server communication
    if (!session->persistentClientId.isEmpty()) {
        session->canvas->setRemoteSceneTarget(session->persistentClientId, session->lastClientInfo.getMachineName());
    }

    // Project editing and remote commands are independent capabilities. The
    // runtime applies both after the visual workspace has been selected.
    m_mainWindow->updateWorkspaceCapabilities(persistentClientId);
    updateUploadButtonForSession(session);
}

void ClientWorkspaceController::rotateSessionIdea(void* sessionPtr) {
    ApplicationRuntime::CanvasSession* session = static_cast<ApplicationRuntime::CanvasSession*>(sessionPtr);
    if (!session) return;
    
    const QString oldIdeaId = session->canvasSessionId;
    
    session->canvasSessionId = m_mainWindow->createIdeaId();
    session->expectedIdeaFileIds.clear();
    session->knownRemoteFileIds.clear();
    if (session->canvas) {
        session->canvas->setActiveIdeaId(session->canvasSessionId);
    }
    m_mainWindow->getFileManager()->removeIdeaAssociations(oldIdeaId);

    if (m_mainWindow->getUploadManager()) {
        if (m_mainWindow->getActiveSessionIdentity() == session->persistentClientId) {
            m_mainWindow->getUploadManager()->setActiveIdeaId(session->canvasSessionId);
        }
    }
    
}

// ============================================================================
// Upload Management
// ============================================================================

void ClientWorkspaceController::updateUploadButtonForSession(void* sessionPtr) {
    ApplicationRuntime::CanvasSession* session = static_cast<ApplicationRuntime::CanvasSession*>(sessionPtr);
    if (!session) return;
    
    if (m_mainWindow->getUploadManager()) {
        emit m_mainWindow->getUploadManager()->uiStateChanged();
    }
}

void ClientWorkspaceController::clearUploadTracking(void* sessionPtr) {
    ApplicationRuntime::CanvasSession* session = static_cast<ApplicationRuntime::CanvasSession*>(sessionPtr);
    if (!session) return;
    
    session->upload.itemsByFileId.clear();
    session->upload.currentUploadFileOrder.clear();
    session->upload.serverCompletedFileIds.clear();
    session->upload.perFileProgress.clear();
    session->upload.receivingFilesToastShown = false;
    if (!session->upload.activeUploadId.isEmpty()) {
        m_mainWindow->removeUploadSessionByUploadId(session->upload.activeUploadId);
        session->upload.activeUploadId.clear();
    }
    if (m_mainWindow->getActiveUploadSessionIdentity() == session->persistentClientId) {
        m_mainWindow->setActiveUploadSessionIdentity(QString());
    }
    if (m_mainWindow->getUploadManager() && m_mainWindow->getUploadManager()->activeSessionIdentity() == session->persistentClientId) {
        m_mainWindow->getUploadManager()->setActiveSessionIdentity(QString());
    }
}

void* ClientWorkspaceController::sessionForActiveUpload() {
    if (!m_mainWindow->getActiveUploadSessionIdentity().isEmpty()) {
        if (void* sessionPtr = findCanvasSession(m_mainWindow->getActiveUploadSessionIdentity())) {
            return sessionPtr;
        }
    }
    if (m_mainWindow->getUploadManager()) {
        const QString clientId = m_mainWindow->getUploadManager()->activeUploadTargetClientId();
        if (!clientId.isEmpty()) {
            if (void* sessionPtr = findCanvasSessionByServerClientId(clientId)) {
                return sessionPtr;
            }
        }
    }
    return nullptr;
}

void* ClientWorkspaceController::sessionForUploadId(const QString& uploadId) {
    if (!uploadId.isEmpty()) {
        const QString identity = m_mainWindow->getUploadSessionByUploadId(uploadId);
        if (!identity.isEmpty()) {
            if (void* sessionPtr = findCanvasSession(identity)) {
                return sessionPtr;
            }
        }
    }
    return sessionForActiveUpload();
}
