# Client usernames and profile pictures

Settings has an optional Username and a circular profile picture. Changes are
drafts until Save, including imported/removed pictures. Cancel and Escape discard
the draft. The server address and the profile are committed together atomically;
an import, validation or write failure preserves the previous saved settings.

Usernames are non-unique, trimmed, limited to 64 Unicode code points, and cannot
contain control characters. Display names use username when present, otherwise
the machine hostname, followed by the existing instance number. The installation
key, endpoint ID and instance ID remain independent of the display profile.

## Image contract

Imports accept PNG, JPG/JPEG and WebP from local files, at most 20 MiB and
64 megapixels. The first frame is decoded with its EXIF orientation, cropped to
a centered square, resized smoothly to 250 × 250 and flattened onto white.
Output is always JPEG at quality 90, at most 128 KiB. Decode errors leave the
existing draft untouched. The bundled fallback is
`qrc:/icons/default-profile-picture.jpg`.

## Ownership and lifetime

Only the owner persists the profile: optional `username` and base64
`profilePictureJpeg` keys in the existing per-instance settings INI (schema 1).
Normal primary profiles survive restart. Secondary instances keep the same
temporary lifetime as their other settings. Clear storage removes the profile
while preserving installation identity.

The runtime's `ClientProfileCache` keeps peer metadata in RAM (4096 entries),
and decoded pictures in a 64 MiB LRU. Saved/draft local previews are pinned
separately. Images are exposed to QML by `ProfilePictureProvider`; remote
pictures are never written to disk. Rendering textures are managed by Qt.

The server holds username/JPEG only on the live authenticated connection.
Discovery carries username and SHA-256, and visible avatars request missing
images by endpoint/hash. Control requests use the existing authenticated
envelope, correlation and bounded retries. The client verifies hash, JPEG
format and decoded 250 × 250 dimensions. Empty fields from a live endpoint
clear the profile; absent fields on an offline presence retain only the current
process's last known profile. After an observer restarts, offline peers display
their hostname and the fallback image until their owner reconnects. Changing
servers clears the observer's peer cache.

## Presentation and history

Client lists, ongoing scenes, client status, notifications and named dialogs
use circular pictures adjacent to the display name. Profile changes refresh
already visible surfaces. Platform remains available in the connection tooltip.

New notification messages separate plain text from technical peer references
(endpoint, hostname, instance number and From/To role). Username and image are
resolved from RAM at display time; they are never saved in project references
or history. History schema 2 migrates schema 1 without resetting entries,
read state or replay suppression. Existing unstructured messages keep their
original text because their peer identity cannot be inferred reliably.

## Validation and rollout

`ClientProfile` covers conversion, EXIF, alpha, bounds, atomic persistence,
draft commits, instance isolation, stale images, offline fallback and cache
limits. `ClientInfoDisplay`, `ConnectionManager`, `RemoteSessionIntegration`,
`CanvasSelectionBackend`, `MediaOverlay`, `RuntimeStorageBootstrap` and
`NotificationCenter` cover protocol, visible updates and migration. The server
suite includes `client_profile.test.js`.

Deploy the server changes first, then the rebuilt clients. Profile fields are
additive within protocol v12; old clients continue to display hostname, and a
new client connected to an old server falls back to hostname/default remotely.
