# Transport, recovery and received-media storage

Protocol **v12**, timing policy **v5** require a coordinated server/client update.
Two missed heartbeats mark a transport Degraded. A silent transport is replaced
after five seconds. Sessions keep one fixed fifteen-second interruption budget,
capped by their server proof, and become terminal at expiry. Recovery does not
recreate a stopped scene. PREPARE and its acknowledgements are idempotent; a
committed scene retains its original start time.

Upload payloads and inventories use the authenticated bidirectional data socket.
An unavailable or full data socket pauses transfers. Control keeps commands and
heartbeats, with a separate bounded queue (256 KiB, with 16 KiB reserved for priority commands).
Scene preparation is limited to 236 KiB of scene/manifest content and fails
explicitly above that limit. Chunks are at most 32 KiB; recipient
credit begins at 64 KiB and the relay adjusts it within 32–256 KiB across uploads.
Progress acknowledges durable offsets using deltas. Start, resume and final
validation exchange full inventories.

RemoteCacheStore owns `cache/Uploads`, including session metadata, durable asset
manifests, cleanup intents, tombstones and `.retained`. Its metadata schema
aliases `StorageVersions::ReceivedMedia` (version 2). Bootstrap preserves current
cache metadata in a persistent primary profile; legacy manifests are reset.
The cache store fences obsolete commands, retains eligible bytes for ten minutes
and validates identity, SHA-256, size and extension before reuse. An imported
entry keeps its first expiration. Runtime upload requests may bind to a new
session with a new transfer ID; requests themselves are not persisted.

`MOUFFETTE_REMOTE_MEDIA_RETENTION_MS=600000` and
`MOUFFETTE_REMOTE_MEDIA_CACHE_MAX_MIB=10240` configure received-media retention.
Explicit removal also purges retained copies. The storage upgrade rules are
in the [storage guide](../runtime/storage/README.md).

## Network diagnostics

`MOUFFETTE_NETWORK_DIAGNOSTICS=true` enables JSONL logs in the profile's
`diagnostics` directory (resolved through `RuntimeProfile::appDataLocation`).
The worker rotates `network.0.jsonl`, `network.1.jsonl`, `network.2.jsonl`, each
bounded to 5 MiB. Its producer queue contains at most 1,024 records (4 KiB each).
A full queue or unavailable disk drops diagnostics instead of blocking transport.
Drop counts and repeated events are summarized every five seconds. Verbose mode
(`MOUFFETTE_NETWORK_DIAGNOSTICS_VERBOSE=false` by default) remains rate limited.

Records use UTC and suspend-inclusive monotonic timestamps. Correlate session,
transfer, scene, transport generation and server boot IDs across clients and the
relay. Network summaries include RTT, heartbeat/proof age, loop lag, queue sizes
and sent bytes. Upload records identify durable progress and pause/resume reasons.
Snapshot rejection distinguishes inactive session, mismatched generation, stale
sequence and invalid structure. Cache records distinguish retention, logical
quarantine and physical deletion. Payloads, paths, filenames, credentials and
free-form untrusted error strings are excluded by a scalar field allowlist.
Known socket closure reasons are encoded; unrecognized remote text is redacted.

Run `ctest --test-dir build --output-on-failure` after building, and `npm test`
in `server`. The native CI workflow runs these on macOS and Windows. Diagnostics
from the incident's two clients and relay are still needed to attribute a real
network interruption or process crash.
