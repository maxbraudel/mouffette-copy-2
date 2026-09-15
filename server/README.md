# Mouffette Server

Node.js WebSocket coordinator for Mouffette protocol v4.

## Run and test

```bash
npm install
npm test
npm start
```

The checked-in `.env` listens on `0.0.0.0:8080`. Process environment values
override it. Invalid critical values fail startup.

## Protocol envelope

The server sends `auth_challenge` first. The client signs
`mouffette-v4\n<serverBootId>\n<nonce>\n<runtimeId>\n<instanceId>` with its
Ed25519 installation key and returns the SPKI public key and signature as
base64url. The SHA-256 of the SPKI key is the stable `installationId`; the
server domain-separates and hashes `installationId + instanceId` to derive and
verify the targetable `endpointId`.

Every subsequent message uses:

```json
{
  "type": "message_type",
  "protocolVersion": 3,
  "serverBootId": "uuid-from-welcome",
  "messageId": "unique-uuid",
  "connectionGeneration": 1
}
```

The server rejects messages from an earlier boot or protocol version. It derives
the sender from the authenticated socket; sender IDs supplied by a client are
never authoritative.

## Remote sessions

An owner opens `remote_session_open` with `targetEndpointId`. The server returns a
`remoteSessionId`, generation, and same-runtime resume token. A target has one
incoming session at most. Heartbeats maintain a strict 3-second lease; a resume
at or after the deadline is terminal. Session teardown remains pending until the
target confirms that its scene stopped, uploads aborted, and cache was
quarantined. Until that acknowledgement is committed, the server retains the
session binding and replays the same teardown transaction to the target with a
bounded exponential backoff.

Configuration enforces that the retained OPEN-request window never exceeds the
terminal-tombstone window. Production also gives terminal tombstones at least
the same cardinality budget as retained OPEN requests. Tombstones referenced by
retained requests are pinned against expiry and capacity eviction, so
idempotent request replay cannot outlive its terminal evidence.

Session messages are `remote_session_open`, `remote_session_resume`,
`remote_session_close`, and `remote_session_teardown_ack`. Server results are
`remote_session_opened`, `remote_session_resumed`, `remote_session_lease_state`,
`remote_session_terminating`, and `remote_session_closed`.

An authenticated replacement transport from the same endpoint and runtime may
close the exact live session generation without first resuming it. This advances
only terminal-state delivery to the current connection generation; it never
rebinds command authority. A different runtime or an older transport generation
is rejected.

Replaying an OPEN request also never migrates command authority. `Grace`
requires the proof-bearing Resume path; an `Opening` stranded on an older owner
transport is moved into cleanup instead of being re-offered with a stale
transport tuple.

After an authenticated `endpoint_snapshot` is accepted, its
`endpoint_snapshot_applied` response and every replayable terminal session state
are enqueued before the following `client_list` on that same WebSocket. That
ordered list is a terminal-reconciliation barrier, not a session inventory:
discovery remains presence-only.

## Uploads

Uploads are bound to one `remoteSessionId` and generation. `upload_start`
declares normalized assets containing `assetId`, `fileId`, `sha256`, `size`,
`extension`, and `mediaIds`. Only PNG, JPG/JPEG, WebP, AVIF, and MP4 metadata is
accepted; both endpoints remain responsible for decoding and content checks.

`upload_chunk` contains `assetId`, contiguous byte `offset`, chunk `size`, file
`sha256`, and base64 data. Target `upload_progress` reports a durable contiguous
offset for each asset. `upload_resume` rewinds the relay to that durable offset.
`upload_complete` enters final validation only after every asset's durable offset
equals its declared size; bytes merely queued or relayed stay behind this barrier.
The server permits two concurrent outgoing uploads per endpoint and one per remote
session. Only an exact target `upload_finished` acknowledgement enters the
session asset inventory.

## Scene runs

`scene_prepare` carries an immutable revision, normalized manifest, scene, and
digest:

```text
SHA-256(canonical JSON { manifest, revision, scene })
```

Every manifest asset must be present in the exact session generation inventory.
If an asset is missing, the owner receives a correlated
`scene_asset_not_validated` error before a run is created. Once the run is
registered and the preparation request has reached the target, the server sends
the owner an aggregate `prepare_progress` acknowledgement with
`stage: "accepted"`; the owner waits for this barrier before reporting its own
progress or readiness.

Both endpoints then send `prepared` with a complete checklist and `armed` with
clock uncertainty no greater than the advertised policy. After both are armed,
the server emits `commit` for its monotonic time 500 ms in the future by default
(`MOUFFETTE_SCENE_ACTIVATION_LEAD_MS` remains configurable up to 10 seconds).
Both endpoints confirm the first presented frame with `started` within five
seconds of that deadline. Clock-estimation uncertainty remains capped
independently at 50 ms; real compositor presentation may differ by up to 750 ms
before the run is considered unsafe. A run becomes live only after both
confirmations.

The remaining v4 scene messages are `prepare_progress`, `state_snapshot`, `stop`, and
`stopped`. The server derives both endpoints from the session, bounds payloads,
rejects stale generations, and preserves terminal tombstones for idempotent
retries. Removed `remote_scene_*` message routes do not exist.
