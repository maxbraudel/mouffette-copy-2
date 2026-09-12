#include "backend/managers/system/SystemLifecycleMonitorBackend.h"

namespace {
class StubSystemLifecycleMonitorBackend final : public SystemLifecycleMonitorBackend
{
public:
    using SystemLifecycleMonitorBackend::SystemLifecycleMonitorBackend;

    bool start() override { return false; }
    void stop() override {}
};
}

std::unique_ptr<SystemLifecycleMonitorBackend>
createSystemLifecycleMonitorBackend(SystemLifecycleMonitor* monitor)
{
    return std::make_unique<StubSystemLifecycleMonitorBackend>(monitor);
}
