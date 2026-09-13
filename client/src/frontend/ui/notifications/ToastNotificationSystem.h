#ifndef TOASTNOTIFICATIONSYSTEM_H
#define TOASTNOTIFICATIONSYSTEM_H

#include <QObject>

#include "backend/notifications/NotificationCenter.h"

// Compatibility publishing facade used by the business layer. It owns no
// visual object: NotificationCenter is the source for both durable history and
// the QML ToastStack model.
class ToastNotificationSystem final : public QObject
{
    Q_OBJECT

public:
    enum class Type { Success, Error, Warning, Info, Loading };

    explicit ToastNotificationSystem(QObject* parent = nullptr);
    ~ToastNotificationSystem() override;

    void showSuccess(const QString& message, int duration = -1);
    void showError(const QString& message, int duration = -1);
    void showWarning(const QString& message, int duration = -1);
    void showInfo(const QString& message, int duration = -1);
    void showLoading(const QString& message, int duration = -1);
    void showNotification(const QString& message, Type type, int duration = -1);
    QString publishNotification(const NotificationRequest& request);

    NotificationCenter* notificationCenter() const { return m_notificationCenter; }
    void clearAll();

    static ToastNotificationSystem* instance();
    static void setInstance(ToastNotificationSystem* instance);

private:
    NotificationCenter* m_notificationCenter = nullptr;
    static ToastNotificationSystem* s_instance;
};

#define TOAST_SUCCESS(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showSuccess(__VA_ARGS__); \
    } \
} while(0)

#define TOAST_ERROR(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showError(__VA_ARGS__); \
    } \
} while(0)

#define TOAST_WARNING(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showWarning(__VA_ARGS__); \
    } \
} while(0)

#define TOAST_INFO(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showInfo(__VA_ARGS__); \
    } \
} while(0)

#define TOAST_LOADING(...) do { \
    if (auto* system = ToastNotificationSystem::instance()) { \
        system->showLoading(__VA_ARGS__); \
    } \
} while(0)

#endif // TOASTNOTIFICATIONSYSTEM_H
