#pragma once

// Storage compatibility, independent of the application/release version.
// Every incompatible bump MUST declare a migration or reset in StorageRegistry.
namespace StorageVersions {
inline constexpr int Settings = 1;
inline constexpr int Projects = 6;
inline constexpr int History = 1;
inline constexpr int Identity = 1;
inline constexpr int ReceivedMedia = 1;
}
