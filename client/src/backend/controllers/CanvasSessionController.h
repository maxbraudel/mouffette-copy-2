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
 * @brief Controller for managing canvas sessions lifecycle and state
 * 
 * Handles:
 * - Session creation and lookup
 * - Session configuration and switching
 * - Upload state management per session
 * - Session cleanup and rotation
 * 
 * Note: This controller works with ApplicationRuntime::CanvasSession which is
 * a nested type. We use void* in the interface to avoid circular dependencies,
 * and cast to the proper type in the implementation.
 */
class CanvasSessionController : public QObject
{
    Q_OBJECT

public:
    explicit CanvasSessionController(ApplicationRuntime* mainWindow, QObject* parent = nullptr);
    ~CanvasSessionController() override = default;

    // Session lookup methods - return void* to avoid exposing nested type in header
    void* findCanvasSession(const QString& persistentClientId);
    const void* findCanvasSession(const QString& persistentClientId) const;
    void* findCanvasSessionByServerClientId(const QString& serverClientId);
    const void* findCanvasSessionByServerClientId(const QString& serverClientId) const;
    void* findCanvasSessionByIdeaId(const QString& canvasSessionId);

    // Session lifecycle
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

#endif // CANVASSESSIONCONTROLLER_H
