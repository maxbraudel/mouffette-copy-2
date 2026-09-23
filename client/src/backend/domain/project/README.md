# Project persistence

ProjectStore owns projects/projects-v2.json. Its schemaVersion comes from StorageVersions::Projects; the file name is a stable locator. Bootstrap validates this component independently. Incompatible project changes require a central migration or explicit reset transition; ProjectStore only reads and writes the current schema.

See the [central storage architecture and migration guide](../../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.

Timeline projects use schema 8 / render schema 6. Each instance owns exactly one clip, an absolute keyframe timeline and a zero-based track index. Track 0 is rendered above later tracks; layer order is not a keyframe property. Internal empty tracks are retained and the editor derives one trailing empty track.

Previous project schemas are explicitly reset at bootstrap for this change, including versions 6 and 7. This reset affects saved projects only: source files, settings, notification history and installation identity are preserved. The timeline maximum duration and slot cadence are captured when a project is created. Scrubbing and temporary element drafts never enter the durable authoring snapshot.

## Last screen previews

`ProjectScreenPreviewStore` owns the optional `projects/screen-previews-v1`
sidecar tree. Each project/screen has only its latest PNG, encoded and atomically
saved on a serial worker. This is a compatible addition to project schema 8:
projects without snapshots still load normally; neither authoring JSON nor
remote-session credentials contain pixels. The directory format is versioned
independently by its locator, and a small atomic generation manifest makes Hide
invalidate old images before background deletion. Reads are fenced against
new frames, Hide and project removal. Project deletion and startup pruning
clean up sidecars; an invalid image never invalidates the rest of a project.
