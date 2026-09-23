# Capture exclusion validation — 2026-09-23

The publishing Mouffette instance is excluded from both desktop and system-audio
capture, including its received scenes, control windows, previews and monitoring.
The viewer's Canvas renders scene media above the desktop and plays scene audio
independently of its screen and system-audio buttons. Separate Mouffette instances
are separate processes and are outside this exclusion on macOS; Windows audio
also excludes descendants of the publishing process.

Update the viewer and publisher together. An older publisher still includes its
scene; an older viewer still suppresses its local scene. Saved project and wire
formats are unchanged.

## Verified on macOS

The Debug application and affected Qt test targets build on macOS 26.1, arm64,
with Qt 6.11.2. Architecture checks and `git diff --check` pass.

- AudioEngine, AudioStereo, AudioOutputRouting, AudioCaptureStability,
  AudioPlaybackStability, AudioSharingContinuity, AudioCapturePipeline,
  AudioTransport and ScreenAudioClock pass.
- WindowCaptureExclusion, ScreenCaptureLayers, ScreenStreamCodec,
  MacScreenCaptureLifecycle and QmlArchitectureGate pass. Windows-specific
  tests skip on macOS; the default codec run skips opt-in native recording.
- ResidentMedia and ScreenSharingService pass, including playback, capture
  lifecycle, remote transport, consent and independent audio/screen availability.
- ClientConnectionFlow and RemoteSceneControllerLifecycle pass with native
  Cocoa/Metal. MediaOverlay and MediaOverlayScaled pass with the software/offscreen
  renderer. Their initial native runs failed only while waiting for window
  activation, before the corresponding functional assertions.
- The broad WindowPresentation run passed scene stacking and window recreation,
  but failed the normal fullscreen transition and cross-application raise.
  Both failed cases pass in isolated native runs (3.66 s and 2.19 s).
- The image/video/text compositing regression
  `CanvasInteraction::remoteScreensRemainBehindCanvasMedia` passes with native
  Cocoa/Metal at scale factors 1 and 2. A native window-activation timeout in the
  first text run passed when rerun alone; its pixel assertions passed.
- The final native RemoteSceneLifecycle run passes all four screen/audio toggle
  combinations and all four termination paths, including real QML visibility,
  media mute/volume, stable playback position and unchanged project content.
  The text and hidden cached-video preparation cases also pass: 12 passes,
  no failures or skips, including setup/cleanup, in 14.1 s.
  The full software/offscreen lifecycle run passed 44 cases and exposed two
  failures: an obsolete exact-one PREPARED assertion and the hidden video's
  presentation barrier. The first now accepts the protocol's existing retries
  while retaining the deadline and identity checks; both cases pass in the final
  native rerun. The hidden-video case also passed the earlier full native run.
- The native audio probe `applicationPlaybackIsExcluded:native-capture-opt-in`
  passes without skips. Actual local media and monitoring play test tones while
  their frequencies remain absent from the captured Opus stream, including after
  mute, volume, pause and replay changes.
- The native screen probe
  `nativeProcessExclusionOmitsScenesAndControlsWhenExplicitlyEnabled` passes
  without skips. A separate process paints a blue window; scene/control windows
  in the capturing process cover it locally but the captured pixels stay blue.
  Recreating the scene and opening a new dialog during capture remain excluded.

Both native probes used existing recording permission and stored no captured
desktop or audio files. Their commands, from the build directory, are:

```sh
QT_QPA_PLATFORM=cocoa MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE=1 ./tst_AudioEngine applicationPlaybackIsExcluded:native-capture-opt-in
QT_QPA_PLATFORM=cocoa MOUFFETTE_TEST_SCREEN_CAPTURE=1 ./tst_ScreenStreamCodec nativeProcessExclusionOmitsScenesAndControlsWhenExplicitlyEnabled
```

## Remaining platform qualification

Windows compilation and native DXGI/WGC capture are not exercised on this Mac.
Run the native affinity/creation/stacking tests on Windows and verify transparent
scenes, common dialogs, monitor changes and device changes on two computers.
Native system-audio capture of another application and end-to-end acoustic timing
also require that two-computer qualification; the automated external-system
audio input uses a synthetic capture source.

Linux and other backends without application exclusion refuse screen publication
before creating a capture worker. Their new rejection test is conditional on
those platforms and was not run here. Receiving remote screens and scene editing
remain available.
