# Multiple instances and stable endpoint identity (protocol v7)

An installation is scoped to one OS user and compiled channel (development or
production). Every process shares its installation key, but each numbered slot
is a separate client on the network. The slot is allocated locally before
networking, not from the order in which the server accepts connections.

## Identity and authentication

The smallest free positive slot is reserved atomically. Running processes never
change slots. Slot 1 maps to `instanceId=primary`; slot N maps to `instance-N`.
The ordinal is an integer from 1 through 2,147,483,647. The existing endpoint
hash remains `SHA-256(mouffette-endpoint-v1\n<installationId>\n<instanceId>)`,
encoded as unpadded base64url. Reusing a slot produces the same endpoint while
each process gets a new `runtimeId` and no old session or scene is resumed.

The Ed25519 authentication payload is:

```text
mouffette-v7
<serverBootId>
<nonce>
<runtimeId>
<instanceId>
<instanceOrdinal>
```

The server checks the canonical mapping and signature before admitting a
transport. Endpoint snapshots cannot change the ordinal. Presence includes
installation, endpoint, instance ID, ordinal and runtime, including retained
offline entries. Client list rows are keyed by endpoint and display
`hostname (N)`, including `(1)` in single-instance mode. Hostnames remain raw
metadata and are never identity keys.

Saved project target references retain the installation, instance ID and ordinal,
so an offline secondary still displays its correct number after restarting the
owner. Older project files without this optional tuple remain readable; their
hostname stays unnumbered until fresh authenticated presence supplies it. The
runtime ID is never restored from a saved project.

Connection generations increase globally within one server boot; they need not
be consecutive for an endpoint. Only the current transport for an endpoint may
act. A healthy endpoint cannot be displaced by another runtime. After transport
loss, the existing retry/recovery policy determines when a replacement may join.

## Profile and installation lifetimes

The primary profile remains at `runtimes/<channel>/instance-1`. Secondary
profiles have unique temporary directory names, defaults/.env settings and no
copied primary projects, cache or history. Their directory UUID is unrelated to
the stable network endpoint. Quitting releases the slot and removes the temporary
profile; closing a window only hides it. Failed deletions are logged and retried
when startup finds the abandoned profile; live profile locks prevent deletion.

The shared identity lives under `installations/<channel>`, outside every profile.
All processes use the same vault policy and the historical `channel:instance-1`
vault account. Installation preparation is serialized across processes and
precedes profile bootstrap. The existing primary key is adopted without rotation,
including atomic migration of an owner-only fallback file. Metadata missing after
interruption is rebuilt around a valid key. Corrupt/inaccessible keys stop startup
with Retry/Close rather than silently creating a different installation.

`Clear storage and close` removes only the selected profile. It preserves the
shared key and the other processes, so recreating that slot keeps the same
endpoint. Installation identity failures do not offer profile clearing.

`MOUFFETTE_ALLOW_MULTIPLE_INSTANCES` changes launch admission only. It does not
change slot-1 identity, paths, display format or protocol. No live mode switch is
introduced. Disabling one process's network connection also leaves the other
instances connected. Sessions and uploads between local instances remain allowed.

## Cut-over and validation

Close older processes and release server and clients together as v7. The primary
endpoint and its data remain stable. Old ephemeral secondary endpoints are not
merged into stable slots by hostname or guessed ordinal. Incompatible clients
receive the existing explicit compatibility failure. No scene restarts during
the cut-over.

The Qt/Node integration suite runs actual child processes through a simultaneous
launch barrier, checks a shared installation with distinct numbered endpoints,
crossed sessions, independent Disable, primary persistence, secondary clean exit
and crash recovery, slot reuse and server restart. Storage and instance suites
cover key adoption, corruption, concurrency, profile clearing and lock recovery.
The native CI matrix runs these suites on macOS and Windows.

### Local verification — 2026-09-18

The macOS 26.1 development build (Qt 6.11.2) and Qt Quick architecture check
passed. All 15 server suites passed with `npm test --prefix server`.

The 15 targeted Qt suites passed: DeviceIdentityStore, ConnectionManager,
RemoteSessionIntegration, AppConfig, ApplicationInstanceManager,
RuntimeStorageBootstrap, ClientInfoDisplay, ClientConnectionFlow,
UploadRemovalSecurity, RemoteSceneLifecycle, SceneRunCoordinator,
ProjectManager, NotificationCenter, RemoteCacheStore and RemoteCacheHistory.
Affected suites were rerun after the final bootstrap correction; this includes
fresh installations, missing-key checkpoints and inaccessible legacy paths.

The local migration and multiprocess tests use isolated owner-only key files.
They do not exercise migration against the user's macOS Keychain. Windows was
not executed locally; the existing native-vault test runs in Windows CI.
Windows results and native Keychain migration still require platform validation
before release. This targeted run does not assert that the entire graphical
test suite passes, and no server deployment was performed.
