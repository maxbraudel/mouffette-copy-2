# Build and release operations

## Contract

Mouffette has three deliberately separate layers:

1. **Build** compiles source code into an OS/configuration-specific directory.
2. **Stage** installs the application and deploys all runtime dependencies into
   a fresh standalone tree.
3. **Package** signs and archives that staging tree for distribution.

Never distribute an application directly from `out/build`. Never deploy Qt
libraries into a build directory.

## Command matrix

| Operation | macOS | Windows |
|---|---|---|
| Development build | `./scripts/build-development.sh` | `.\scripts\build-development.ps1` |
| Production build | `./scripts/build-release.sh` | `.\scripts\build-release.ps1` |
| Run development | `./scripts/run-development.sh` | `.\scripts\run-development.ps1` |
| Run Release | `./scripts/run-release.sh` | `.\scripts\run-release.ps1` |
| Run standalone stage | `./scripts/run-packaged-release.sh` | `.\scripts\run-packaged-release.ps1` |
| Package | `./scripts/package-release.sh` | `.\scripts\package-release.ps1` |

The current host determines the target OS. Produce macOS releases on macOS and
Windows releases on Windows.

## Build identity and runtime environment

`dev` selects CMake Debug and application channel `development`. Its bundle
identifier is `com.mouffette.client.dev`, so it is distinguishable from a
packaged Release, while the primary profile retains the existing `Mouffette`
data paths and `Mouffette/Client` QSettings identity.

`prod` selects CMake Release and application channel `production`. It uses the
final `Mouffette` and `com.mouffette.client` identities. The instance lock and
IPC channel include the build channel, so Development and Release do not block
one another even though their primary profile remains backward-compatible.

Build type does not select the WebSocket server. Runtime configuration keeps
the documented precedence:

```text
compiled defaults < embedded .env < profile QSettings < process environment < CLI
< keys present in embedded .env.production (production channel only)
```

The production override is key-wise and final: an absent key inherits the
common value, while a present key cannot be superseded at runtime. In
particular, it permanently disables multiple instances for every Release.
Before publishing, review the tracked `.env` and `.env.production`. A server URL on a public host
must use `wss://`. Operational overrides can use `MOUFFETTE_ENV_FILE` or
`--env-file` without recompiling.

## Release gate

Both package scripts perform this sequence:

```text
architecture check -> Release configure/build -> CTest -> clean stage
-> CMake install -> Qt runtime deployment -> signing -> archive -> checksum
```

`--skip-tests`/`-SkipTests` exists for diagnosing packaging locally and must not
be used for a published release.

### macOS

The package script uses `macdeployqt` directly after `cmake --install` (this
preserves QML paths containing spaces). Deployment occurs in an ASCII-only
temporary path to avoid `macdeployqt` Unicode-normalization errors, after which
the validated relocatable app is copied to `out/stage`. Homebrew's split Qt
formula symlinks are materialized, and remaining Homebrew install names are
normalized to the bundle's `@rpath`. It additionally:

- validates the bundle signature;
- inspects every Mach-O file and rejects references to Homebrew, `/usr/local`,
  or the source tree;
- signs with Hardened Runtime when a Developer ID is configured;
- submits with `notarytool` and staples the ticket when a notary profile exists.

For a public build, use `./scripts/package-release.sh --require-signing`. The command fails if
the signing or notarization configuration is incomplete.

The default deployment target is the active macOS version because Homebrew may
build Qt modules for that host only. For wider compatibility, use an official
or custom Qt distribution whose frameworks support the desired target, then
set `MOUFFETTE_MACOS_DEPLOYMENT_TARGET` explicitly. Never lower the target while
ignoring linker compatibility warnings.

### Windows

The staged executable is deployed using Qt's generated CMake deployment script,
then the matching `windeployqt` performs an explicit QML import scan. The run
script does not add MSYS2 to `PATH` in packaged mode.

For a public build, set `MOUFFETTE_WINDOWS_CERTIFICATE` to a protected PFX file,
provide its password through the process environment, and use
`.\scripts\package-release.ps1 -RequireSigning`. Secrets and certificates must never be stored
in this repository.

## Versioning

`project(MouffetteClient VERSION ...)` in `CMakeLists.txt` is the single version
source. CMake propagates it to:

- the C++ application version;
- macOS bundle metadata;
- Windows VERSIONINFO;
- package filenames.

Increase it before cutting a release. A CI/release job should attach the
artifact and `.sha256` file to a tag with the same version.

## Continuous integration

`.github/workflows/client-native-build.yml` exercises the same public commands
on native GitHub runners. It builds and tests both channels on macOS and
Windows, then runs each production packager without publication credentials.
The resulting unsigned/ad-hoc artifacts are retained as CI evidence; they are
not public releases. Signing and notarization remain protected release steps.

## Clean-machine acceptance

Before promotion, install or extract each artifact on a machine that has never
had Qt, MSYS2, Homebrew, or the compiler toolchain installed. Validate at least:

- first launch and application identity;
- connection and reconnect;
- tray/window lifecycle;
- QML canvas rendering;
- image/video playback and FFmpeg backend;
- settings persistence and upgrade from the previous production version;
- Gatekeeper or Windows signature status.
