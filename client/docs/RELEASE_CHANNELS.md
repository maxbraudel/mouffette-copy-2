# Client release policy

All release channels use the same Qt Quick/QML application path. There is no
renderer cohort, opt-in, runtime switch, or legacy fallback.

Promotion requires the native build and CTest suite, the QML architecture
gate, deterministic input/schema checks, packaging validation, and visual
review for affected QML surfaces. Platform-specific releases additionally
require their native multi-screen, tray, lifecycle, and installer checks.
