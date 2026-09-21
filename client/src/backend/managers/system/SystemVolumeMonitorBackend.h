#pragma once

#include <QObject>
#include <functional>
#include <memory>
#include <utility>

// Native callbacks must marshal to the owning Qt thread before publishing.
class SystemVolumeMonitorBackend : public QObject
{
public:
    using Publish = std::function<void(int)>;
    explicit SystemVolumeMonitorBackend(Publish publish)
        : m_publish(std::move(publish)) {}
    virtual void start() = 0;
    virtual void stop() = 0;

protected:
    void publish(int percent) { m_publish(percent); }

private:
    Publish m_publish;
};

std::unique_ptr<SystemVolumeMonitorBackend>
createSystemVolumeMonitorBackend(SystemVolumeMonitorBackend::Publish publish);
