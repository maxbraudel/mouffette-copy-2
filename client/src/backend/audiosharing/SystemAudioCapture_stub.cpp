#include "backend/audiosharing/SystemAudioCapture.h"
namespace {
class UnavailableCapture final : public SystemAudioCapture {
public:
    void start(Pcm, State state) override { state(false, QStringLiteral("System audio sharing is not supported on this platform")); }
    void stop() override {}
};
}
std::unique_ptr<SystemAudioCapture> createSystemAudioCapture() { return std::make_unique<UnavailableCapture>(); }
