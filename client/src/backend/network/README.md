# Received-media storage

RemoteCacheStore owns cache/Uploads, including media, session metadata, cleanup intents and tombstones. Its MetadataSchemaVersion aliases StorageVersions::ReceivedMedia. Bootstrap owns cache/storage.json and purges Uploads every launch as session maintenance. Compatibility changes are registered centrally; protocol versions remain a separate contract.

See the [central storage architecture and migration guide](../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.
