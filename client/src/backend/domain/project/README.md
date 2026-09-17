# Project persistence

ProjectStore owns projects/projects-v2.json. Its schemaVersion comes from StorageVersions::Projects; the file name is a stable locator. Bootstrap validates this component independently. Incompatible project changes require a central migration or explicit reset transition; ProjectStore only reads and writes the current schema.

See the [central storage architecture and migration guide](../../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.
