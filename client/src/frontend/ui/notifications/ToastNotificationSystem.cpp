#include "frontend/ui/notifications/ToastNotificationSystem.h"

ToastNotificationSystem* ToastNotificationSystem::s_instance = nullptr;

ToastNotificationSystem::ToastNotificationSystem(QObject* parent)
    : QObject(parent)
    , m_notificationCenter(new NotificationCenter(this))
{
}

ToastNotificationSystem::~ToastNotificationSystem()
{
    if (s_instance == this) s_instance = nullptr;
}

void ToastNotificationSystem::showSuccess(const QString& message, int duration)
{
    showNotification(message, Type::Success, duration);
}

void ToastNotificationSystem::showError(const QString& message, int duration)
{
    showNotification(message, Type::Error, duration);
}

void ToastNotificationSystem::showWarning(const QString& message, int duration)
{
    showNotification(message, Type::Warning, duration);
}

void ToastNotificationSystem::showInfo(const QString& message, int duration)
{
    showNotification(message, Type::Info, duration);
}

void ToastNotificationSystem::showLoading(const QString& message, int duration)
{
    showNotification(message, Type::Loading, duration);
}

void ToastNotificationSystem::showNotification(const QString& message,
                                               Type type, int duration)
{
    NotificationRequest request;
    request.message = message;
    request.toastDurationMs = duration;
    switch (type) {
    case Type::Success: request.severity = NotificationSeverity::Success; break;
    case Type::Error: request.severity = NotificationSeverity::Error; break;
    case Type::Warning: request.severity = NotificationSeverity::Warning; break;
    case Type::Info: request.severity = NotificationSeverity::Info; break;
    case Type::Loading: request.severity = NotificationSeverity::Loading; break;
    }
    publishNotification(request);
}

QString ToastNotificationSystem::publishNotification(
    const NotificationRequest& request)
{
    return m_notificationCenter ? m_notificationCenter->publish(request) : QString();
}

void ToastNotificationSystem::clearAll()
{
    // Active toast lifetime is owned by ToastStack.qml. Durable history is not
    // cleared by dismissing transient notifications.
}

ToastNotificationSystem* ToastNotificationSystem::instance()
{
    return s_instance;
}

void ToastNotificationSystem::setInstance(ToastNotificationSystem* instance)
{
    s_instance = instance;
}
