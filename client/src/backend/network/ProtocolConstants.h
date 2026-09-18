#pragma once

namespace MouffetteProtocol {

// The control and upload transports use one strict wire version. Keep the
// value outside WebSocketClient so lower-level protocol consumers never need
// to duplicate a numeric literal.
inline constexpr int Version = 8;

} // namespace MouffetteProtocol
