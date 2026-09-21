# Notification persistence

HistoryStore owns notification-history-v1.json and aliases StorageVersions::History. Bootstrap validates and upgrades the history before NotificationCenter is constructed. History changes must declare a central migration or reset; corruption does not reset projects, settings or identity.

See the [central storage architecture and migration guide](../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.

History schema 2 adds `peers` to each entry: technical `endpointId`, fallback
`machineName`, `instanceOrdinal`, and optional `From`/`To` role. The notification
body no longer embeds a peer's display name. UI models expose these references
for live resolution against the in-memory profiles; username and image data are
never serialized here. Schema 1 migrates with empty peer lists, preserving legacy
phrases, read state, order, identifiers and terminal replay tombstones.
