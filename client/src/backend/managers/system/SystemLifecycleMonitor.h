#ifndef SYSTEMLIFECYCLEMONITOR_H
#define SYSTEMLIFECYCLEMONITOR_H

#include <QObject>
#include <memory>

class SystemLifecycleMonitorBackend;

/**
 * Normalizes native sleep/wake and session lock/unlock notifications.
 *
 * A system is considered suspended while at least one native cause remains
 * active. This matters when, for example, a machine wakes before its login
 * session has been unlocked: the aggregate signal must stay true until both
 * causes have cleared.
 */
class SystemLifecycleMonitor final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool systemSuspended READ isSystemSuspended NOTIFY systemSuspendedChanged)

public:
    enum class NativeEvent {
        SystemWillSleep,
        SystemDidWake,
        SessionLocked,
        SessionUnlocked
    };
    Q_ENUM(NativeEvent)

    explicit SystemLifecycleMonitor(QObject* parent = nullptr);
    ~SystemLifecycleMonitor() override;

    bool isSystemSuspended() const noexcept { return m_systemSleeping || m_sessionLocked; }
    bool isSystemSleeping() const noexcept { return m_systemSleeping; }
    bool isSessionLocked() const noexcept { return m_sessionLocked; }
    bool isNativeMonitoringActive() const noexcept { return m_nativeMonitoringActive; }

    // Start explicitly after connecting systemSuspendedChanged, so an initial
    // native lock-state observation cannot be missed by the owner.
    bool startNativeMonitoring();
    void stopNativeMonitoring();

    // Deterministic seam for unit tests and platform smoke-test harnesses.
    // It deliberately follows the exact same aggregation path as native events.
    void simulateNativeEventForTesting(NativeEvent event);

signals:
    void systemSuspendedChanged(bool suspended);

private:
    friend class SystemLifecycleMonitorBackend;
    void applyNativeEvent(NativeEvent event);

    std::unique_ptr<SystemLifecycleMonitorBackend> m_backend;
    bool m_systemSleeping = false;
    bool m_sessionLocked = false;
    bool m_nativeMonitoringActive = false;
};

#endif // SYSTEMLIFECYCLEMONITOR_H
