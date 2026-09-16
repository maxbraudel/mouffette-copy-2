# Accurate seeking before first playback on macOS

## Cause and correction

The Qt 6.11.2 Darwin backend's `AVFMediaPlayer::setPosition()` creates its
`CMTime` using `AVPlayerItem.currentTime.timescale`. Before playback, that
timescale can be 1. Assigning the requested fractional seconds to the integer
`CMTime.value` truncates them: seeking to 1234 ms settles at 1000 ms.
Qt initially reports the requested position and drops that optimistic value
when the asynchronous native seek completes, causing the visible jump.
Starting playback changes the native timebase and hides the defect.

The fix is `CMTimeMake(pos, 1000)`: preserve QMediaPlayer's millisecond units
directly, independently of the current playback state. The existing zero
seek tolerances remain unchanged. No artificial play/pause, cursor lock,
delay, or retry is involved.

Upstream source:
[Qt 6.11.2 avfmediaplayer.mm](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/plugins/multimedia/darwin/mediaplayer/avfmediaplayer.mm#L833).

## Build and distribution

`cmake/QtDarwinMedia.cmake` downloads a hash-pinned Qt 6.11.2 source archive,
checks/applies the small conversion patch, and compiles only the Darwin
plugin against the selected Qt frameworks and private headers. The system
Qt installation is never modified. The plugin retains Qt's upstream licenses
and copyright notices.

The build puts the patched plugin in `plugins/multimedia` for CTest and in
`Mouffette.app/Contents/PlugIns/multimedia` for the app. At startup the app
prioritizes its bundled plugins before creating any multimedia objects.
The package script preserves this plugin instead of overwriting it with the
system copy; normal runtime deployment and signing still apply. FFmpeg and
Windows are unchanged. This is needed even with `QT_MEDIA_BACKEND=ffmpeg`:
Qt can fall back to Darwin if the installation does not include FFmpeg.

For an offline build, extract the pinned archive beforehand and configure
with `-DFETCHCONTENT_SOURCE_DIR_MOUFFETTE_QTMULTIMEDIA=/absolute/source/path`.
Use a dedicated writable copy because the patch is applied there.

For a Qt upgrade, inspect upstream's conversion, update the exact version
and archive SHA-256, and run the Darwin regression suite. The build fails on
an unreviewed Qt version instead of loading a plugin built against different
private interfaces. Remove the patch when the supported upstream version
contains the fix; retain the regression tests.

## Verification

`VideoPlaybackBackend::seekBeforeFirstPlay` failed against the original
backend (1234 ms became 1000 ms) and passes with the patched plugin. It
checks forward/backward/zero seeks, including a request made during loading,
after allowing native seek completion, and verifies that playback never starts.
`seekAfterPauseAndSourceReload` also covers paused playback and replacing
the source, which recreates the initial native timebase.

`VideoPlaybackDarwinSeek` explicitly selects Darwin so this regression stays
covered when a Qt installation also provides FFmpeg. The full video and
overlay suites cover playback, loops, markers, restored previews and QML.
