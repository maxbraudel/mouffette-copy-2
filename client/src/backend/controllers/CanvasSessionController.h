#ifndef CANVASSESSIONCONTROLLER_H
#define CANVASSESSIONCONTROLLER_H

#include <QObject>
#include <QString>
#include <QSet>

// Forward declarations
class ApplicationRuntime;
class ClientInfo;
class QFont;
class QuickCanvasHost;

/**
 * @brief Controller for managing per-client workspaces and their canvases
 * 
 * Handles:
 * - Workspace creation and lookup
 * - Canvas configuration and switching
 * - Runtime upload correlation per workspace
 * - Ephemeral document identity rotation
 * 
 * Note: This controller works with ApplicationRuntime::ClientWorkspace which is
 * a nested type. We use void* in the interface to avoid circular dependencies,
 * and cast to the proper type in the implementation.
 */
class ClientWorkspaceController : public QObject
{
    Q_OBJECT

public:
    explicit ClientWorkspaceController(ApplicationRuntime* mainWindow, QObject* parent = nullptr);
    ~ClientWorkspaceController() override = default;

    // Workspace lookup methods - return void* to avoid exposing nested type in header
    void* findCanvasSession(const QString& persistentClientId);
    const void* findCanvasSession(const QString& persistentClientId) const;
    void* findCanvasSessionByServerClientId(const QString& serverClientId);
    const void* findCanvasSessionByServerClientId(const QString& serverClientId) const;
    void* findCanvasSessionByIdeaId(const QString& canvasSessionId);

    // Workspace/canvas lifecycle
    void* ensureCanvasSession(const ClientInfo& client);
    void prewarmQuickCanvasHost();
    void configureCanvasSession(void* session);
    void switchToCanvasSession(const QString& persistentClientId);
    void rotateSessionIdea(void* session);

    // Upload management
    void updateUploadButtonForSession(void* session);
    void clearUploadTracking(void* session);
    void* sessionForActiveUpload();
    void* sessionForUploadId(const QString& uploadId);

private:
    ApplicationRuntime* m_mainWindow;
    QuickCanvasHost* m_prewarmedQuickCanvasHost = nullptr;
    QString m_lastQuickInitError;
};

// Transitional source compatibility for extensions compiled against the old
// name. Runtime code uses ClientWorkspaceController exclusively.
using CanvasSessionController = ClientWorkspaceController;

#endif // CANVASSESSIONCONTROLLER_H
