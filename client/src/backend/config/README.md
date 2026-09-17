# Configuration and storage bootstrap

Only the persisted user settings layer is versioned by storage bootstrap. Environment variables, embedded env files and CLI arguments keep AppConfig precedence and validation. initializePreApplication must not read user settings before bootstrap.

See the [central storage architecture and migration guide](../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.
