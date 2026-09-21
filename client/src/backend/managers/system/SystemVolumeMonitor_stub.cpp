#include "backend/managers/system/SystemVolumeMonitorBackend.h"

namespace {
class UnavailableVolumeMonitor final : public SystemVolumeMonitorBackend
{
public:
    using SystemVolumeMonitorBackend::SystemVolumeMonitorBackend;
    void start() override { publish(-1); }
    void stop() override {}
};
}

std::unique_ptr<SystemVolumeMonitorBackend>
createSystemVolumeMonitorBackend(SystemVolumeMonitorBackend::Publish publish)
{
    return std::make_unique<UnavailableVolumeMonitor>(std::move(publish));
}
