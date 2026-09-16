# Scene playback

Test scenes and the local participant of remote scenes use the same
`SceneMediaPlayback` timeline in `QuickCanvasHost`. The remote renderer follows
the same timing rules after the synchronized activation barrier.

Local test playback is independent of the remote session. Peer loss, expired
connection leases, terminal session messages and session inactivity cleanup
stop only remote playback. Local tests retain their preparation, timers, video
playback, draft state, editing lock and RAM protection until explicitly stopped.
Invalid local media, project deletion, application shutdown and sustained memory
pressure still stop the affected local test.

| Setting | Reference time |
| --- | --- |
| Display delay | Scene activation; starts the visual fade-in |
| Hide delay | Start of appearance, including the fade-in |
| Play delay | Scene activation |
| Pause delay | Automatic playback start |
| Unmute / mute delay | Scene activation; starts the corresponding audio fade |
| Hide / mute when video ends | Final playback end, after all repeats, using the end marker when present |

When an end action is enabled, its hide/mute delay becomes an offset from the
end: negative runs before the end, zero at the end, positive after it. Without
an end action, negative delays are treated as zero. Disabled automatic display,
playback and unmute remain disabled even if their delay fields retain values.
For simultaneous automatic unmute and mute, mute runs last.

Visual opacity is the configured opacity multiplied by the animated envelope.
The configured audio volume remains stable while the device volume is animated.
The logical mute state changes at the start of its fade; the device is muted
only after a fade-out completes. Periodic state snapshots must preserve active
envelopes rather than jump to their final values.

All local timers and animations belong to the scene lifetime. Stop destroys
them before restoring draft visibility, position, playback and audio state.
Scene preparation locks editing synchronously. QML loaders unload the toolbar,
settings panel, selection chrome, guides and media controls while locked.
The media list, remote cursor and scene stop actions remain available.

Regression coverage lives in `tst_RemoteSceneLifecycle`,
`tst_VideoPlaybackBackend`, `tst_RemoteSceneControllerLifecycle` and
`tst_MediaOverlay`. `tst_ClientConnectionFlow` exercises local test survival through
actual remote-session cleanup and lease-expiry events. The server scene protocol
tests cover signed end offsets and reject malformed delays before remote
preparation.
