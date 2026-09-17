# Notification persistence

HistoryStore owns notification-history-v1.json and aliases StorageVersions::History. Bootstrap validates and upgrades the history before NotificationCenter is constructed. History changes must declare a central migration or reset; corruption does not reset projects, settings or identity.

See the [central storage architecture and migration guide](../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.
