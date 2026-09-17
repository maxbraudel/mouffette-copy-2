# Settings persistence

SettingsManager writes settings/settings.ini, including the central settings schema version. AppConfig applies runtime precedence after bootstrap. Compatibility and the unversioned INI migration belong to the central storage subsystem; never reject a boolean solely because QSettings read it as a QString.

See the [central storage architecture and migration guide](../../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.
