# Project persistence

ProjectStore owns projects/projects-v2.json. Its schemaVersion comes from StorageVersions::Projects; the file name is a stable locator. Bootstrap validates this component independently. Incompatible project changes require a central migration or explicit reset transition; ProjectStore only reads and writes the current schema.

See the [central storage architecture and migration guide](../../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.

Timeline projects use schema 7 / render schema 5. Schema 6 migrates at bootstrap without deleting projects: images and text receive full-scene clips and video tracks are preserved. Schemas 1–5 retain their reset policy. The timeline maximum duration and slot cadence are captured when a project is created. Scrubbing and temporary element drafts never enter the durable authoring snapshot.
