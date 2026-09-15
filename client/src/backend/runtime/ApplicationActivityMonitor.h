#pragma once

#include <QObject>

#include <functional>

class ApplicationActivityMonitor final : public QObject
{
    Q_OBJECT

public:
    explicit ApplicationActivityMonitor(QObject* parent = nullptr);

    bool isActive() const { return m_effectiveActive; }
    qint64 inactiveSinceMs() const { return m_inactiveSinceMs; }

    void setPointerInside(bool inside);
    void setControlWindowVisible(bool visible);
    void setSystemSuspended(bool suspended);
    void setNowProviderForTesting(std::function<qint64()> provider);

signals:
    void inactivityStarted(qint64 epochMs);
    void activityResumed(qint64 epochMs);

private:
    qint64 nowMs() const;
    void reconcile();

    bool m_pointerInside = false;
    bool m_controlWindowVisible = false;
    bool m_systemSuspended = false;
    bool m_effectiveActive = false;
    qint64 m_inactiveSinceMs = -1;
    std::function<qint64()> m_nowProvider;
};
