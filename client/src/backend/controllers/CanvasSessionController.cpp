#include "CanvasSessionController.h"
#include "MainWindow.h"
#include "backend/domain/models/ClientInfo.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/rendering/canvas/LegacySceneMirror.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "backend/domain/session/SessionManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/UploadManager.h"
#include "backend/files/FileManager.h"
#include "backend/files/FileWatcher.h"
#include "backend/managers/app/MigrationTelemetryManager.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/ui/pages/ClientListPage.h"
#include "frontend/ui/pages/CanvasViewPage.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include "backend/domain/media/MediaItems.h"
#include <QStackedWidget>
#include <QGraphicsScene>
#include <QPushButton>
#include <QTimer>
#include <QDebug>

namespace {
LegacySceneMirror* createExplicitLegacyMirror(QWidget* parentWidget) {
    if (!parentWidget) {
        return nullptr;
    }
    ScreenCanvas* mediaCanvas = new ScreenCanvas(parentWidget);
    return new LegacySceneMirror(mediaCanvas, parentWidget);
}
}

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
    QString persistentId = client.endpointId();
    if (persistentId.isEmpty()) {
        qWarning() << "CanvasSessionController::ensureCanvasSession: client has no persistentClientId, this should not happen";
        persistentId = client.getId();
    }
    
    // Check if session already exists
    // Use SessionManager (creates canvasSessionId automatically)
    MainWindow::CanvasSession& session = m_mainWindow->getSessionManager()->getOrCreateSession(persistentId, client);
    
    // Initialize canvas if needed (UI-specific responsibility)
    if (!session.canvas) {
        QStackedWidget* canvasHostStack = m_mainWindow->getCanvasViewPage() ? m_mainWindow->getCanvasViewPage()->getCanvasHostStack() : nullptr;
        if (!canvasHostStack) {
            qWarning() << "Cannot create canvas: CanvasViewPage not initialized";
            return &session;
        }

        const bool quickRequested = true;
        QString appliedRenderer = QStringLiteral("quick_canvas_shell");
        QString reason;

        if (m_prewarmedQuickCanvasHost) {
            session.canvas = m_prewarmedQuickCanvasHost;
            m_prewarmedQuickCanvasHost = nullptr;
            reason = QStringLiteral("mandatory_quick_prewarmed_shell");
        } else {
            QString quickError;
            LegacySceneMirror* legacyBridge = createExplicitLegacyMirror(canvasHostStack);
            session.canvas = QuickCanvasHost::create(canvasHostStack, legacyBridge, &quickError);
            if (!session.canvas && legacyBridge) {
                delete legacyBridge;
            }
            if (session.canvas) {
                reason = QStringLiteral("mandatory_quick_explicit_model_bridge");
                m_lastQuickInitError.clear();
            } else {
                appliedRenderer = QStringLiteral("quick_canvas_initialization_error");
                reason = QStringLiteral("mandatory_quick_init_failed");
                m_lastQuickInitError = quickError.isEmpty()
                    ? QStringLiteral("Qt Quick canvas failed to initialize") : quickError;
                qCritical() << "CanvasSessionController: mandatory Qt Quick canvas unavailable."
                            << "error=" << m_lastQuickInitError;
                TOAST_ERROR(QStringLiteral("Canvas initialization failed: %1")
                                .arg(m_lastQuickInitError),
                            5000);
            }
        }

        MigrationTelemetryManager::logRendererPathResolved(
            QStringLiteral("CanvasSessionController::ensureCanvasSession"),
            quickRequested,
            appliedRenderer,
            reason);

        if (!session.canvas) {
            return &session;
        }

        session.canvas->setActiveIdeaId(session.canvasSessionId); // Use canvasSessionId from SessionManager
        session.connectionsInitialized = false;
        configureCanvasSession(&session);
        if (canvasHostStack->indexOf(session.canvas->asWidget()) == -1) {
            canvasHostStack->addWidget(session.canvas->asWidget());
        }
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

void CanvasSessionController::prewarmQuickCanvasHost() {
    if (!m_mainWindow || m_prewarmedQuickCanvasHost) {
        return;
    }

    QStackedWidget* canvasHostStack = m_mainWindow->getCanvasViewPage() ? m_mainWindow->getCanvasViewPage()->getCanvasHostStack() : nullptr;
    if (!canvasHostStack) {
        return;
    }

    QString quickError;
    LegacySceneMirror* legacyBridge = createExplicitLegacyMirror(canvasHostStack);
    QuickCanvasHost* prewarmedHost = QuickCanvasHost::create(canvasHostStack, legacyBridge, &quickError);
    if (!prewarmedHost) {
        if (legacyBridge) {
            delete legacyBridge;
        }
        m_lastQuickInitError = quickError;
        if (!quickError.isEmpty()) {
            qWarning() << "CanvasSessionController: Quick prewarm failed:" << quickError;
        } else {
            qWarning() << "CanvasSessionController: Quick prewarm failed with unknown error";
        }
        return;
    }

    m_lastQuickInitError.clear();
    m_prewarmedQuickCanvasHost = prewarmedHost;
    if (canvasHostStack->indexOf(prewarmedHost->asWidget()) == -1) {
        canvasHostStack->addWidget(prewarmedHost->asWidget());
    }
    prewarmedHost->setOverlayActionsEnabled(false);
}

void CanvasSessionController::configureCanvasSession(void* sessionPtr) {
    MainWindow::CanvasSession* session = static_cast<MainWindow::CanvasSession*>(sessionPtr);
    if (!session || !session->canvas) return;

    session->canvas->setActiveIdeaId(session->canvasSessionId);
    session->canvas->setWebSocketClient(m_mainWindow->getWebSocketClient());
    session->canvas->setUploadManager(m_mainWindow->getUploadManager());
    session->canvas->setFileManager(m_mainWindow->getFileManager());
    session->canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (dynamic_cast<QuickCanvasHost*>(session->canvas) != nullptr) {
        session->canvas->setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    } else {
        session->canvas->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    }
    session->canvas->setFocusPolicy(Qt::StrongFocus);
    session->canvas->installEventFilter(m_mainWindow);

    // Connect to MainWindow signal via direct call (onRemoteSceneLaunchStateChanged is private)
    connect(session->canvas, &ICanvasHost::remoteSceneLaunchStateChanged, m_mainWindow,
            &MainWindow::onRemoteSceneLaunchStateChanged,
            Qt::UniqueConnection);

    if (session->canvas->viewportWidget()) {
        QWidget* viewport = session->canvas->viewportWidget();
        viewport->setAttribute(Qt::WA_StyledBackground, true);
        viewport->setAutoFillBackground(true);
        viewport->setStyleSheet("background: palette(base); border: none; border-radius: 5px;");
        viewport->installEventFilter(m_mainWindow);
    }

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
        if (session->canvas->scene()) {
            connect(session->canvas->scene(), &QGraphicsScene::changed,
                    projectAutosaveTimer,
                    [projectAutosaveTimer](const QList<QRectF>&) {
                projectAutosaveTimer->start();
            });
        }
        connect(session->canvas, &ICanvasHost::mediaItemAdded, m_mainWindow,
                [this, persistentId=session->persistentClientId](ResizableMediaBase* mediaItem) {
                    if (m_mainWindow->getFileWatcher() && mediaItem && !mediaItem->sourcePath().isEmpty()) {
                        m_mainWindow->getFileWatcher()->watchMediaItem(mediaItem);
                        qDebug() << "CanvasSessionController: source watch added for mediaId"
                                 << mediaItem->mediaId();
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
        
        connect(session->canvas, &ICanvasHost::mediaItemRemoved, m_mainWindow,
                [this, persistentId=session->persistentClientId](ResizableMediaBase* mediaItem) {
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
        if (canvasHostStack->indexOf(session->canvas->asWidget()) == -1) {
            canvasHostStack->addWidget(session->canvas->asWidget());
        }
        canvasHostStack->setCurrentWidget(session->canvas->asWidget());
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
