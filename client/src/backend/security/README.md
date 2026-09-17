# Device identity persistence

DeviceIdentityStore owns native credential material and the owner-only fallback in identity/. Bootstrap owns identity/storage.json and its resetting/creating/ready recovery checkpoints. Credential namespaces include the channel and profile ID. Access errors stop startup instead of rotating the identity. Add identity migrations and reset policies centrally.

See the [central storage architecture and migration guide](../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.
