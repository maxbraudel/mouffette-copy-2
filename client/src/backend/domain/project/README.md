# Project persistence

ProjectStore owns projects/projects-v2.json. Its schemaVersion comes from StorageVersions::Projects; the file name is a stable locator. Bootstrap validates this component independently. Incompatible project changes require a central migration or explicit reset transition; ProjectStore only reads and writes the current schema.

See the [central storage architecture and migration guide](../../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.

Timeline projects use schema 5 / render schema 3. Earlier project schemas reset automatically; there is no conversion of scene automation. The timeline maximum duration is captured when a project is created. Scrubbing and temporary element drafts never enter the durable authoring snapshot.
