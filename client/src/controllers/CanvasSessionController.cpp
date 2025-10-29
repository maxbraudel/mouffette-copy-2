#include "CanvasSessionController.h"
#include "MainWindow.h"
#include "ClientInfo.h"
#include "ScreenCanvas.h"
#include "SessionManager.h"
#include "WebSocketClient.h"
#include "UploadManager.h"
#include "FileManager.h"
#include "FileWatcher.h"
#include "WatchManager.h"
#include "ScreenNavigationManager.h"
#include "ui/pages/ClientListPage.h"
#include "ui/pages/CanvasViewPage.h"
#include "ResizableMediaItems.h"
#include <QStackedWidget>
#include <QPushButton>
#include <QTimer>
#include <QDebug>

CanvasSessionController::CanvasSessionController(MainWindow* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

// ============================================================================
// Session Lookup Methods
// ============================================================================

void* CanvasSessionController::findCanvasSession(const QString& persistentClientId) {
    return m_mainWindow->getSessionManager()->findSession(persistentClientId);
}

const void* CanvasSessionController::findCanvasSession(const QString& persistentClientId) const {
    return m_mainWindow->getSessionManager()->findSession(persistentClientId);
}

void* CanvasSessionController::findCanvasSessionByServerClientId(const QString& serverClientId) {
    return m_mainWindow->getSessionManager()->findSessionByServerClientId(serverClientId);
}

const void* CanvasSessionController::findCanvasSessionByServerClientId(const QString& serverClientId) const {
    return m_mainWindow->getSessionManager()->findSessionByServerClientId(serverClientId);
}

void* CanvasSessionController::findCanvasSessionByIdeaId(const QString& canvasSessionId) {
    return m_mainWindow->getSessionManager()->findSessionByIdeaId(canvasSessionId);
}

// ============================================================================
// Session Lifecycle
// ============================================================================

void* CanvasSessionController::ensureCanvasSession(const ClientInfo& client) {
    QString persistentId = client.clientId();
    if (persistentId.isEmpty()) {
        qWarning() << "CanvasSessionController::ensureCanvasSession: client has no persistentClientId, this should not happen";
        persistentId = client.getId();
    }
    
    // Check if session already exists
    bool isNewSession = !m_mainWindow->getSessionManager()->hasSession(persistentId);
    
    // Use SessionManager (creates canvasSessionId automatically)
    MainWindow::CanvasSession& session = m_mainWindow->getSessionManager()->getOrCreateSession(persistentId, client);
    
    // Notify server of canvas creation (CRITICAL for canvasSessionId validation)
    if (isNewSession && m_mainWindow->getWebSocketClient()) {
        m_mainWindow->getWebSocketClient()->sendCanvasCreated(persistentId, session.canvasSessionId);
    }
    
    // Initialize canvas if needed (UI-specific responsibility)
    if (!session.canvas) {
        QStackedWidget* canvasHostStack = m_mainWindow->getCanvasViewPage() ? m_mainWindow->getCanvasViewPage()->getCanvasHostStack() : nullptr;
        if (!canvasHostStack) {
            qWarning() << "Cannot create canvas: CanvasViewPage not initialized";
            return &session;
        }
        session.canvas = new ScreenCanvas(canvasHostStack);
        session.canvas->setActiveIdeaId(session.canvasSessionId); // Use canvasSessionId from SessionManager
        session.connectionsInitialized = false;
        configureCanvasSession(&session);
        canvasHostStack->addWidget(session.canvas);
    }
    
    // Update remote target
    if (!session.persistentClientId.isEmpty()) {
        session.canvas->setRemoteSceneTarget(session.persistentClientId, session.lastClientInfo.getMachineName());
    }
    
    // Update online status
    if (session.lastClientInfo.isOnline()) {
        session.remoteContentClearedOnDisconnect = false;
    }
    
    // Refresh ongoing scenes via ClientListPage
    if (m_mainWindow->getClientListPage()) {
        m_mainWindow->getClientListPage()->refreshOngoingScenesList();
    }
    return &session;
}

void CanvasSessionController::configureCanvasSession(void* sessionPtr) {
    MainWindow::CanvasSession* session = static_cast<MainWindow::CanvasSession*>(sessionPtr);
    if (!session || !session->canvas) return;

    session->canvas->setActiveIdeaId(session->canvasSessionId);
    session->canvas->setWebSocketClient(m_mainWindow->getWebSocketClient());
    session->canvas->setUploadManager(m_mainWindow->getUploadManager());
    session->canvas->setFileManager(m_mainWindow->getFileManager());
    session->canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    session->canvas->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    session->canvas->setFocusPolicy(Qt::StrongFocus);
    session->canvas->installEventFilter(m_mainWindow);

    // Connect to MainWindow signal via direct call (onRemoteSceneLaunchStateChanged is private)
    connect(session->canvas, &ScreenCanvas::remoteSceneLaunchStateChanged, m_mainWindow,
            &MainWindow::onRemoteSceneLaunchStateChanged,
            Qt::UniqueConnection);

    if (session->canvas->viewport()) {
        QWidget* viewport = session->canvas->viewport();
        viewport->setAttribute(Qt::WA_StyledBackground, true);
        viewport->setAutoFillBackground(true);
        viewport->setStyleSheet("background: palette(base); border: none; border-radius: 5px;");
        viewport->installEventFilter(m_mainWindow);
    }

    if (!session->connectionsInitialized) {
        connect(session->canvas, &ScreenCanvas::mediaItemAdded, m_mainWindow,
                [this, persistentId=session->persistentClientId](ResizableMediaBase* mediaItem) {
                    if (m_mainWindow->getFileWatcher() && mediaItem && !mediaItem->sourcePath().isEmpty()) {
                        m_mainWindow->getFileWatcher()->watchMediaItem(mediaItem);
                        qDebug() << "CanvasSessionController: Added media item to file watcher:" << mediaItem->sourcePath();
                    }
                    MainWindow::CanvasSession* sess = m_mainWindow->getSessionManager()->findSession(persistentId);
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
        
        connect(session->canvas, &ScreenCanvas::mediaItemRemoved, m_mainWindow,
                [this](ResizableMediaBase*) {
                    // Update upload button state immediately when media is removed
                    if (m_mainWindow->getUploadManager()) {
                        emit m_mainWindow->getUploadManager()->uiStateChanged();
                    }
                });
    }

    if (QPushButton* overlayBtn = session->canvas->getUploadButton()) {
        if (session->uploadButton != overlayBtn) {
            connect(overlayBtn, &QPushButton::clicked, m_mainWindow, &MainWindow::onUploadButtonClicked, Qt::UniqueConnection);
        }
        session->uploadButton = overlayBtn;
        session->uploadButtonInOverlay = true;
        session->uploadButtonDefaultFont = overlayBtn->font();
    } else {
        session->uploadButton = nullptr;
        session->uploadButtonInOverlay = false;
        session->uploadButtonDefaultFont = QFont();
    }

    session->connectionsInitialized = true;
}

void CanvasSessionController::switchToCanvasSession(const QString& persistentClientId) {
    // Navigation between clients should NOT trigger unload - uploads persist per session
    // Unload only happens when explicitly requested via button or when remote disconnects
    
    void* sessionPtr = findCanvasSession(persistentClientId);
    MainWindow::CanvasSession* session = static_cast<MainWindow::CanvasSession*>(sessionPtr);
    if (!session || !session->canvas) return;

    m_mainWindow->setActiveSessionIdentity(persistentClientId);
    m_mainWindow->setActiveCanvas(session->canvas);
    if (m_mainWindow->getNavigationManager()) {
        m_mainWindow->getNavigationManager()->setActiveCanvas(session->canvas);
    }

    QStackedWidget* canvasHostStack = m_mainWindow->getCanvasViewPage() ? m_mainWindow->getCanvasViewPage()->getCanvasHostStack() : nullptr;
    if (canvasHostStack) {
        if (canvasHostStack->indexOf(session->canvas) == -1) {
            canvasHostStack->addWidget(session->canvas);
        }
        canvasHostStack->setCurrentWidget(session->canvas);
    }

    session->canvas->setFocus(Qt::OtherFocusReason);
    // Use persistentClientId for server communication
    if (!session->persistentClientId.isEmpty()) {
        session->canvas->setRemoteSceneTarget(session->persistentClientId, session->lastClientInfo.getMachineName());
    }

    // Set upload manager target to restore per-session upload state
    if (m_mainWindow->getUploadManager()) {
        m_mainWindow->getUploadManager()->setTargetClientId(session->persistentClientId);
        m_mainWindow->getUploadManager()->setActiveIdeaId(session->canvasSessionId);
    }
    updateUploadButtonForSession(session);

    m_mainWindow->refreshOverlayActionsState(session->lastClientInfo.isOnline());
}

void CanvasSessionController::rotateSessionIdea(void* sessionPtr) {
    MainWindow::CanvasSession* session = static_cast<MainWindow::CanvasSession*>(sessionPtr);
    if (!session) return;
    
    const QString oldIdeaId = session->canvasSessionId;
    
    // Notify server of canvas deletion before rotation (CRITICAL)
    if (m_mainWindow->getWebSocketClient() && !session->persistentClientId.isEmpty()) {
        m_mainWindow->getWebSocketClient()->sendCanvasDeleted(session->persistentClientId, oldIdeaId);
    }
    
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
    
    // Notify server of new canvas creation after rotation (CRITICAL)
    if (m_mainWindow->getWebSocketClient() && !session->persistentClientId.isEmpty()) {
        m_mainWindow->getWebSocketClient()->sendCanvasCreated(session->persistentClientId, session->canvasSessionId);
    }
}

// ============================================================================
// Upload Management
// ============================================================================

void CanvasSessionController::updateUploadButtonForSession(void* sessionPtr) {
    MainWindow::CanvasSession* session = static_cast<MainWindow::CanvasSession*>(sessionPtr);
    if (!session) return;
    
    m_mainWindow->setUploadButton(session->uploadButton);
    m_mainWindow->setUploadButtonInOverlay(session->uploadButtonInOverlay);
    if (session->uploadButtonDefaultFont != QFont()) {
        m_mainWindow->setUploadButtonDefaultFont(session->uploadButtonDefaultFont);
    }
    if (m_mainWindow->getUploadManager()) {
        emit m_mainWindow->getUploadManager()->uiStateChanged();
    }
}

void CanvasSessionController::unloadUploadsForSession(void* sessionPtr, bool attemptRemote) {
    MainWindow::CanvasSession* session = static_cast<MainWindow::CanvasSession*>(sessionPtr);
    if (!session || !m_mainWindow->getUploadManager()) return;
    
    // Use persistentClientId for server communication
    const QString targetId = session->persistentClientId;
    if (targetId.isEmpty()) {
        session->remoteContentClearedOnDisconnect = true;
        return;
    }

    m_mainWindow->getUploadManager()->setTargetClientId(targetId);
    m_mainWindow->getUploadManager()->setActiveIdeaId(session->canvasSessionId);

    if (attemptRemote && m_mainWindow->getWebSocketClient() && m_mainWindow->getWebSocketClient()->isConnected()) {
        if (m_mainWindow->getUploadManager()->isUploading() || m_mainWindow->getUploadManager()->isFinalizing()) {
            m_mainWindow->getUploadManager()->requestCancel();
        } else if (m_mainWindow->getUploadManager()->hasActiveUpload()) {
            m_mainWindow->getUploadManager()->requestUnload();
        } else {
            m_mainWindow->getUploadManager()->requestRemoval(targetId);
        }
        
        if (m_mainWindow->getUploadButton()) {
            m_mainWindow->getUploadButton()->setFont(m_mainWindow->getUploadButtonDefaultFont());
        }

        m_mainWindow->getWebSocketClient()->sendRemoteSceneStop(targetId);
    }

    m_mainWindow->getFileManager()->unmarkAllForClient(targetId);

    if (session->canvas && session->canvas->scene()) {
        const QList<QGraphicsItem*> items = session->canvas->scene()->items();
        for (QGraphicsItem* item : items) {
            if (auto* media = dynamic_cast<ResizableMediaBase*>(item)) {
                media->setUploadNotUploaded();
            }
        }
    }

    session->remoteContentClearedOnDisconnect = true;

    if (m_mainWindow->getUploadManager()) {
        QPushButton* previousButton = m_mainWindow->getUploadButton();
        bool previousOverlayFlag = m_mainWindow->getUploadButtonInOverlay();
        QFont previousDefaultFont = m_mainWindow->getUploadButtonDefaultFont();

        const bool hasSessionButton = (session->uploadButton != nullptr);
        const bool pointerAlreadySession = hasSessionButton && (m_mainWindow->getUploadButton() == session->uploadButton);

        if (hasSessionButton && !pointerAlreadySession) {
            m_mainWindow->setUploadButton(session->uploadButton);
            m_mainWindow->setUploadButtonInOverlay(session->uploadButtonInOverlay);
            if (session->uploadButtonDefaultFont != QFont()) {
                m_mainWindow->setUploadButtonDefaultFont(session->uploadButtonDefaultFont);
            }
        }

        m_mainWindow->getUploadManager()->forceResetForClient(targetId);

        if (hasSessionButton && !pointerAlreadySession) {
            m_mainWindow->setUploadButton(previousButton);
            m_mainWindow->setUploadButtonInOverlay(previousOverlayFlag);
            m_mainWindow->setUploadButtonDefaultFont(previousDefaultFont);
            if (m_mainWindow->getUploadManager() && previousButton) {
                emit m_mainWindow->getUploadManager()->uiStateChanged();
            }
        }
    }
}

void CanvasSessionController::clearUploadTracking(void* sessionPtr) {
    MainWindow::CanvasSession* session = static_cast<MainWindow::CanvasSession*>(sessionPtr);
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

void* CanvasSessionController::sessionForActiveUpload() {
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

void* CanvasSessionController::sessionForUploadId(const QString& uploadId) {
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
