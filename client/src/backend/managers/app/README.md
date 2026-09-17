# Settings persistence

SettingsManager writes settings/settings.ini, including the central settings schema version. AppConfig applies runtime precedence after bootstrap. Compatibility and the unversioned INI migration belong to the central storage subsystem; never reject a boolean solely because QSettings read it as a QString.

See the [central storage architecture and migration guide](../../runtime/storage/README.md) for the
version inventory, ownership boundaries, upgrade procedure and tests.

The settings dialog also exposes **Clear storage and close**. Its request goes
through `ApplicationController` to the process shutdown owner in `main`, which
removes the active profile after all services and QML have been destroyed.
SettingsManager must never delete the runtime while writers are still alive.
