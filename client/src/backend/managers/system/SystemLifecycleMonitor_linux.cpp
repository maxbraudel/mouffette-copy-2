#include "backend/managers/system/SystemLifecycleMonitorBackend.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDebug>
#include <QStringList>
#include <QVariantMap>

namespace {
constexpr auto kLoginService = "org.freedesktop.login1";
constexpr auto kManagerPath = "/org/freedesktop/login1";
constexpr auto kManagerInterface = "org.freedesktop.login1.Manager";
constexpr auto kSessionInterface = "org.freedesktop.login1.Session";
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

class LinuxSystemLifecycleMonitorBackend final
    : public QObject
    , public SystemLifecycleMonitorBackend
{
    Q_OBJECT

public:
    explicit LinuxSystemLifecycleMonitorBackend(SystemLifecycleMonitor* monitor)
        : SystemLifecycleMonitorBackend(monitor)
        , m_bus(QDBusConnection::systemBus())
    {
    }

    ~LinuxSystemLifecycleMonitorBackend() override
    {
        stop();
    }

    bool start() override
    {
        if (m_started) {
            return true;
        }
        if (!m_bus.isConnected()) {
            qWarning() << "systemd-logind lifecycle monitoring is unavailable: no system D-Bus";
            return false;
        }

        m_sleepConnected = m_bus.connect(
            QString::fromLatin1(kLoginService), QString::fromLatin1(kManagerPath),
            QString::fromLatin1(kManagerInterface), QStringLiteral("PrepareForSleep"),
            this, SLOT(onPrepareForSleep(bool)));

        QDBusInterface manager(QString::fromLatin1(kLoginService),
                               QString::fromLatin1(kManagerPath),
                               QString::fromLatin1(kManagerInterface), m_bus);
        const QDBusReply<QDBusObjectPath> sessionReply = manager.call(
            QStringLiteral("GetSessionByPID"),
            QVariant::fromValue(static_cast<uint>(QCoreApplication::applicationPid())));
        if (sessionReply.isValid()) {
            m_sessionPath = sessionReply.value().path();
            m_lockConnected = m_bus.connect(
                QString::fromLatin1(kLoginService), m_sessionPath,
                QString::fromLatin1(kSessionInterface), QStringLiteral("Lock"),
                this, SLOT(onSessionLocked()));
            m_unlockConnected = m_bus.connect(
                QString::fromLatin1(kLoginService), m_sessionPath,
                QString::fromLatin1(kSessionInterface), QStringLiteral("Unlock"),
                this, SLOT(onSessionUnlocked()));
            m_propertiesConnected = m_bus.connect(
                QString::fromLatin1(kLoginService), m_sessionPath,
                QString::fromLatin1(kPropertiesInterface), QStringLiteral("PropertiesChanged"),
                this, SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));

            // Capture an already-locked session when monitoring starts.
            QDBusInterface session(QString::fromLatin1(kLoginService), m_sessionPath,
                                   QString::fromLatin1(kSessionInterface), m_bus);
            const QVariant lockedHint = session.property("LockedHint");
            if (lockedHint.isValid()) {
                publish(lockedHint.toBool()
                            ? SystemLifecycleMonitor::NativeEvent::SessionLocked
                            : SystemLifecycleMonitor::NativeEvent::SessionUnlocked);
            }
        } else {
            qWarning() << "systemd-logind session lifecycle monitoring is unavailable:"
                       << sessionReply.error().message();
        }

        m_started = m_sleepConnected || m_lockConnected || m_unlockConnected
            || m_propertiesConnected;
        return m_started;
    }

    void stop() override
    {
        if (m_sleepConnected) {
            m_bus.disconnect(QString::fromLatin1(kLoginService),
                             QString::fromLatin1(kManagerPath),
                             QString::fromLatin1(kManagerInterface),
                             QStringLiteral("PrepareForSleep"),
                             this, SLOT(onPrepareForSleep(bool)));
        }
        if (!m_sessionPath.isEmpty()) {
            if (m_lockConnected) {
                m_bus.disconnect(QString::fromLatin1(kLoginService), m_sessionPath,
                                 QString::fromLatin1(kSessionInterface),
                                 QStringLiteral("Lock"), this, SLOT(onSessionLocked()));
            }
            if (m_unlockConnected) {
                m_bus.disconnect(QString::fromLatin1(kLoginService), m_sessionPath,
                                 QString::fromLatin1(kSessionInterface),
                                 QStringLiteral("Unlock"), this, SLOT(onSessionUnlocked()));
            }
            if (m_propertiesConnected) {
                m_bus.disconnect(QString::fromLatin1(kLoginService), m_sessionPath,
                                 QString::fromLatin1(kPropertiesInterface),
                                 QStringLiteral("PropertiesChanged"), this,
                                 SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
            }
        }
        m_sleepConnected = false;
        m_lockConnected = false;
        m_unlockConnected = false;
        m_propertiesConnected = false;
        m_sessionPath.clear();
        m_started = false;
    }

private slots:
    void onPrepareForSleep(bool sleeping)
    {
        publish(sleeping
                    ? SystemLifecycleMonitor::NativeEvent::SystemWillSleep
                    : SystemLifecycleMonitor::NativeEvent::SystemDidWake);
    }

    void onSessionLocked()
    {
        publish(SystemLifecycleMonitor::NativeEvent::SessionLocked);
    }

    void onSessionUnlocked()
    {
        publish(SystemLifecycleMonitor::NativeEvent::SessionUnlocked);
    }

    void onPropertiesChanged(const QString& interface,
                             const QVariantMap& changed,
                             const QStringList& invalidated)
    {
        Q_UNUSED(invalidated);
        if (interface != QString::fromLatin1(kSessionInterface)
            || !changed.contains(QStringLiteral("LockedHint"))) {
            return;
        }
        publish(changed.value(QStringLiteral("LockedHint")).toBool()
                    ? SystemLifecycleMonitor::NativeEvent::SessionLocked
                    : SystemLifecycleMonitor::NativeEvent::SessionUnlocked);
    }

private:
    QDBusConnection m_bus;
    QString m_sessionPath;
    bool m_started = false;
    bool m_sleepConnected = false;
    bool m_lockConnected = false;
    bool m_unlockConnected = false;
    bool m_propertiesConnected = false;
};
}

std::unique_ptr<SystemLifecycleMonitorBackend>
createSystemLifecycleMonitorBackend(SystemLifecycleMonitor* monitor)
{
    return std::make_unique<LinuxSystemLifecycleMonitorBackend>(monitor);
}

#include "SystemLifecycleMonitor_linux.moc"
