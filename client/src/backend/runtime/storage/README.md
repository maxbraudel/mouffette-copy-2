# Runtime storage and upgrades

This directory is the entry point for **all client storage compatibility**.
Application release versions, package timestamps and build hashes do not decide
whether data is usable. Each component compares its durable schema version with
its own expected version in `StorageVersions.h`.

## Ownership inventory

Paths are relative to the selected runtime root. File names are stable locators;
legacy `-v1`/`-v2` suffixes are not compatibility versions.

| Component | Owner | Durable version | Expected | Upgrade policy |
|---|---|---|---:|---|
| `settings` | `SettingsManager` / `AppConfig` | `settings/settings.ini`, `[storage] schemaVersion` | 1 | Unversioned valid INI: migrate 0 → 1 |
| `projects` | `ProjectStore` | `projects/projects-v2.json`, `schemaVersion` | 7 | Version 6: migrate to 7; versions 1–5: explicit reset |
| `history` | `HistoryStore` | `notification-history-v1.json`, `schemaVersion` | 1 | Unknown formats: reset |
| `cache` | `RemoteCacheStore` | `cache/storage.json`, `schemaVersion` | 1 | Unknown formats: reset; session contents also purged on every launch |

The installation identity is separate from these resettable profile components.
`InstallationIdentityBootstrap` owns `installations/<channel>/storage.json` and
the private material in the native credential vault or an owner-only file in
that installation directory. The vault account preserves the historical
`<channel>:instance-1` namespace for every instance. Cache ownership includes the entire
`cache/Uploads` subtree: media, session descriptors, intents, tombstones and
cleanup state. Network protocol and canvas rendering schema versions remain
separate contracts; an incompatible persisted project change still requires a
project storage version bump. Source media outside the profile are never owned
by these components.

## Startup and compatibility decisions

`ApplicationInstanceManager` selects and locks a profile. `RuntimeStorageBootstrap`
first adopts or creates the shared installation identity under an interprocess
lock, then invokes `StorageRegistry` and `StorageUpgradeEngine` before `AppConfig` reads
persisted settings and before `ApplicationRuntime` constructs its services.

- Missing component: initialize at the expected version.
- Current version and valid content: preserve.
- Different version with a complete migration chain: migrate.
- Explicit reset transition, unknown version, missing chain or downgrade: reset
  only this component directly to its expected version.
- Corrupt content or unconvertible migration input: reset only this component.
- Read, write, lock or credential-access failure: stop with the real error;
  retry is available. Do not misclassify inaccessible data as corruption.

There are no backups, recovery acknowledgements, toasts or confirmation dialogs
for successful initialization, migration or reset. Reports/logs record the
component, discovered/expected versions, action and reason. Cache purging is
reported as maintenance, not a version reset. Unknown optional INI keys are
preserved; INI booleans accept `true`, `false`, `1`, `0` after normalization.
The storage metadata group is excluded from `AppConfig`'s settings map.

The old `storage-manifest.json` is not consulted, recreated or used to erase the
runtime. Previously shared profiles are not imported or cleaned by startup.

## Module responsibilities

- `StorageVersions.h`: single expected-version source; store constants alias it.
- `InstallationIdentityBootstrap`: shared identity adoption, serialized creation
  and validation. It never silently rotates a corrupt or inaccessible key.
- `StorageRegistry.cpp`: four profile ownership adapters, current validators, default
  initializers and an explicit transition table. It owns no application UI.
- `StorageUpgradeEngine`: resolves a complete, unambiguous route before executing
  it; validates each committed step; reports outcomes independently.
- `StorageIO`: bounded reads, exact version parsing, path checks and atomic writes.
- `migrations/<component>/vN_to_vM.cpp`: transformations only. Destination versions
  are literal historical versions, never aliases of the current version.

Business stores serialize/validate their current representations. They do not
run migrations or silently delete incompatible data. Their load errors retain
an I/O/content distinction for the bootstrap adapters. Settings use Qt's INI
parser on a fresh snapshot so its process-local filename/type cache cannot hide
what is actually on disk. INI snapshots use unique temporary directories and
closed files so `QSettings` can read or atomically create them on Windows.
Versioned whole-file writes use `QSaveFile` with direct
write fallback disabled; ordinary `QSettings::sync()` updates also preserve the
version key and use Qt's atomic synchronization.

## Crash recovery and concurrency

Each JSON/INI document publishes data and schema version in the same atomic
replacement. Completed components remain completed if a later component fails.
For chained migrations, restart begins at the last committed version; it never
replays already committed steps. Intermediate serializers must validate their
output before committing it; the engine checks the resulting stored version.

The shared identity is prepared before any profile component. A valid historical
primary key is adopted, not reset when the new metadata is absent. File migration
uses an atomic owner-only write, verifies the installation ID, and publishes a
ready checkpoint. An interrupted migration reuses the valid key. Metadata can be
repaired around that key; a missing key with an existing checkpoint, an invalid
key or an inaccessible vault stops startup with Retry/Close. Profile clearing
cannot repair or replace that shared identity. The cache is disposable:
its purge is idempotent, and its new schema marker is committed after reset.

The application holds `active.lock` for the profile lifetime; bootstrap also
holds `bootstrap.lock` while inspecting and upgrading. Stale process locks are
recovered through Qt's lock protocol, without age-based expiry of live owners.
The root and ancestors inside it must not be symbolic links. Owned leaf links
may be removed, but their targets are never read or traversed by a reset.

## Development and production

Persistent roots are `runtimes/development/instance-1` and
`runtimes/production/instance-1` under the platform app-data directory. Existing
primary data and its installation key are preserved. Shared identity roots are
`installations/development` and `installations/production`; coordination also
includes the compiled channel. Secondary instances retain separate temporary
roots but share their channel's installation identity. The smallest free slot
is reused, with `primary` for slot 1 and `instance-N` for higher slots; the UUID
in a temporary directory name is never its network identity.

## Explicit full removal

Settings and the startup window after a profile storage/settings failure offer
**Clear storage and close**, without confirmation. Media and shared identity startup failures do not
offer it. This is an explicit user action, separate from component upgrades.
The controller marks the request once and asks the application to quit through
its normal reader/
renderer shutdown barrier. `main` first destroys the QML application shell while
its controller is still alive, then the controller/runtime services and their
windows, then the shared QML engine. It waits for background workers, releases
the profile file lock, then invokes
`clearProfileStorage` before returning from the process. The instance slot stays
locked through removal so another launch cannot recreate the profile midway.

Removal deletes the entire active profile directory, including hidden and
unregistered files, while preserving the shared installation directory and vault.
Other channels, other instances and external media are untouched. The next
launch starts with fresh profile data and the same network identity. Failures are written to stderr and exit with code 6;
there is no success notification, backup or settings-save validation.

## Changing a format

1. Find its owner in the table and update its current reader/writer together.
2. Increment only that component's constant in `StorageVersions.h`.
3. In `StorageRegistry`, declare the old version's transition as either
   `Migrate` with a callback or `Reset` without one. A reset barrier skips earlier
   transformations and initializes the final version directly.
4. Put transformation code in a named file under `migrations/<component>/`, and
   add that source explicitly to `MOUFFETTE_STORAGE_SOURCES` in CMake.
5. Retain historical input codecs in the migration as needed. Do not make an old
   migration write a future `StorageVersions` value. Never perform network work,
   show UI, or mutate another component in a migration.
6. Add input/output fixtures and cover successive upgrades, restart after a
   failed write, reset policy and preservation of unrelated components.
7. Update this inventory and the short README beside the affected owner.

For a new component, additionally register its owned paths, inspection and
initialization callbacks and ensure startup precedes every consumer. Components
must have disjoint ownership; layout changes involving several resources need
an explicit resumable journal rather than an assumed multi-file transaction.

## Validation

`RuntimeStorageBootstrap` tests exercise real child-process restarts (including
application-version-only changes), INI migration, all component mismatches and
corruption, missing stores, downgrade/reset barriers, chained migration failure,
identity checkpoint recovery, I/O failure, locks and external path isolation.
`ClientConnectionFlow` covers silent fresh startup and automatic project reset.
`ApplicationInstanceManager`, `DeviceIdentityStore`, `ProjectManager`,
`NotificationCenter`, `RemoteCacheStore` and `AppConfig` cover their normal APIs.

The existing native CI runs these CTest targets on macOS and Windows, in both
channels. The Windows identity test uses disposable unique Credential Manager
namespaces and deletes them afterward. POSIX permission/symlink tests are marked
as platform-specific. Run `tools/check_architecture_boundaries.sh` as well.
