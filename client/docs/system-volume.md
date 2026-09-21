# Remote system volume

The displayed percentage is the default output volume of the remote machine.
Its operating system reads the value; the controller's operating system does
not participate in that read. The percentage represents the volume slider,
not sound amplitude or the per-application mixer. The existing protocol does
not carry a separate mute flag.

## Diagnosis

Previously, both Windows and macOS detected changes only every 1200 ms.
macOS launched `osascript` for each read. Windows recreated its Core Audio
interfaces for every query and silently returned `-1` for every API failure.
`-1` becomes JSON `null` and the UI displays a dash. Without the failing
Windows call and HRESULT, the exact cause on an affected machine cannot be
established from the dash alone.

The volume already travels over the control WebSocket used by the remote
cursor. Changed snapshots bypass the five-second duplicate suppression, so
that suppression was not the polling delay.

## Current behavior

- Read the initial value when monitoring starts, before initial registration.
- Windows retains `IMMDeviceEnumerator` and `IAudioEndpointVolume`, subscribes
  to volume and device notifications, and rebinds when the default output
  changes. It uses the multimedia output role, with the console role as a
  fallback when no multimedia default exists.
- macOS uses CoreAudio property listeners on the default output, master or
  channel volume controls, and device/service changes.
- Native callbacks set shared atomic flags. The Qt thread drains those flags
  every 16 ms and reads the latest level only when notified. This coalesces
  bursts and keeps callbacks independent of Qt object lifetimes.
- The existing `MOUFFETTE_SYSTEM_VOLUME_POLL_INTERVAL_MS` setting (1200 ms)
  remains a recovery/fallback interval. Normal notifications do not wait for it.
- A changed integer percentage updates the cached full device snapshot and
  is sent immediately, without recapturing screens or reading audio twice.
  Session snapshots take priority; discovery/profile snapshots are coalesced
  after 100 ms of quiet so repeated volume changes do not repeatedly enqueue
  the profile JPEG ahead of session updates or suppress cursor delivery.
  Existing session authentication, snapshot sequencing, cleanup fences and
  transport backpressure remain in effect. Network congestion can still delay
  delivery; only the latest pending snapshot is retained.
- An absent or unreadable output explicitly publishes unknown. A later
  device/API recovery restores the percentage. Stop followed by start works.
- Connection loss hides the percentage while retaining the authenticated
  workspace reading. Returning to command-ready state restores that reading,
  including recovery through lease-state updates alone. An Active RESUME
  snapshot replaces it with the latest volume, including an explicit unknown
  value; unchanged follow-up snapshots cannot leave the indicator blank.
  Idempotent replies that replay an older initial snapshot retain the latest
  accepted value instead of rolling the project back.
- The status card has no trailing loading spinner or reserved spinner space.

Windows errors now include the failing API and HRESULT, for example
`System volume: Windows Core Audio: GetDefaultAudioEndpoint (HRESULT 0x...)`.
Repeated identical failures are suppressed until recovery. These logs
distinguish a missing default output from activation, service or read failures;
the implementation does not substitute a fabricated percentage.

## Validation

Automated coverage in `tst_SystemMonitor` verifies the initial sample,
changes, unknown values, duplicate suppression, boundaries and restart.
It also exercises the real native backend's initial read, thread ownership,
stop/restart and teardown without changing the machine's volume.
`tst_ClientConnectionFlow` verifies immediate snapshot publication, preserved
screen topology, unavailable values, publication invalidation and live label
updates. Server protocol tests cover snapshot validation and session routing.

The macOS build and native read-only smoke can be checked locally. Windows
requires its native build and a real audio output to verify volume-key changes,
switching speakers/headphones, output removal/reappearance, and service
recovery. Rebuild the remote Windows client to apply its native monitoring
change; rebuilding only the macOS controller cannot change Windows detection.
No server protocol change is required.

Native callback contracts:
[Windows volume notifications](https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nn-endpointvolume-iaudioendpointvolumecallback)
and
[Windows device notifications](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient).
