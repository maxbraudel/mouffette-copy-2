import QtQuick
import QtMultimedia
import Mouffette.Canvas
BaseMediaItem {
    id: root

    property var cppMediaPlayer: null
    property var cppVideoSink: null
    property var remoteFrameSource: null
    property var boundMediaPlayer: null
    property var boundFallbackSink: null
    property var boundVideoOutput: null
    property int videoPlaybackErrorCode: 0
    property string videoPlaybackErrorString: ""
    property bool videoHasRenderedFrame: false
    property bool videoFirstFramePrimed: false
    property bool residencyReady: false
    property bool requireInitialSkeleton: true
    readonly property bool remoteFrameMode: remoteFrameSource !== null
    readonly property bool hasLiveFrame: remoteFrameMode
        ? !!remoteFrameLoader.item && remoteFrameLoader.item.hasFrame
        : !!localVideoLoader.item && localFrameSeen
    property bool localFrameSeen: false
    contentReady: root.residencyReady && hasLiveFrame
    initialFramePresented: mediaSurface.renderingAllowed

    function restoreBoundPlayer() {
        var previousPlayer = boundMediaPlayer
        var previousSink = boundFallbackSink
        var previousOutput = boundVideoOutput
        boundMediaPlayer = null
        boundFallbackSink = null
        boundVideoOutput = null
        localFrameSeen = false
        if (!previousPlayer || !("videoOutput" in previousPlayer))
            return
        try {
            // Restore only our own binding. A newer delegate/output must never
            // be detached by destruction of an older delegate.
            if (previousPlayer.videoOutput === previousOutput)
                previousPlayer.videoOutput = previousSink
        } catch (e) { }
    }

    function bindPlayerToOutput() {
        var videoOutput = localVideoLoader.item
        if (!videoOutput || !cppMediaPlayer || !("videoOutput" in cppMediaPlayer)) {
            restoreBoundPlayer()
            return
        }

        if (boundMediaPlayer === cppMediaPlayer && boundVideoOutput === videoOutput) {
            try {
                if (cppMediaPlayer.videoOutput === videoOutput) {
                    // The fallback role can settle one event-loop turn after
                    // the player role. Keep the restore target current even
                    // when no rebinding is otherwise necessary.
                    boundFallbackSink = cppVideoSink
                    return
                }
            } catch (e) { }
        }

        restoreBoundPlayer()
        try {
            // VideoOutput.videoSink is read-only in Qt 6. Give the output to
            // the resident player, which presents its decoded frames there.
            cppMediaPlayer.videoOutput = videoOutput
            boundMediaPlayer = cppMediaPlayer
            boundFallbackSink = cppVideoSink
            boundVideoOutput = videoOutput
            // setVideoOutput can deliver the poster synchronously before the
            // loader's signal connections have settled. Inspect the real sink.
            updateLocalFrameSeen(videoOutput)
        } catch (e) {
            restoreBoundPlayer()
        }
    }

    function updateLocalFrameSeen(videoOutput) {
        if (!videoOutput || videoOutput !== localVideoLoader.item)
            return
        var size = videoOutput.videoSink.videoSize
        localFrameSeen = size.width > 0 && size.height > 0
    }

    MediaSurface {
        id: mediaSurface
        anchors.fill: parent
        requireInitialSkeleton: root.requireInitialSkeleton
        residencyReady: root.residencyReady
        contentReady: root.contentReady

        Loader {
            id: localVideoLoader
            anchors.fill: parent
            z: 1
            active: !root.remoteFrameMode && root.residencyReady && root.cppVideoSink !== null
                    && mediaSurface.renderingAllowed
            onItemChanged: {
                root.localFrameSeen = false
                Qt.callLater(root.bindPlayerToOutput)
            }
            onLoaded: root.bindPlayerToOutput()
            sourceComponent: VideoOutput {
                id: videoOutput
                fillMode: VideoOutput.Stretch

                onWindowChanged: function(window) {
                    if (window)
                        Qt.callLater(root.bindPlayerToOutput)
                }

                Component.onDestruction: {
                    if (root.boundVideoOutput === videoOutput)
                        root.restoreBoundPlayer()
                }

                Connections {
                    target: videoOutput.videoSink
                    function onVideoFrameChanged(frame) {
                        root.updateLocalFrameSeen(videoOutput)
                    }
                }
            }
        }

        Loader {
            id: remoteFrameLoader
            anchors.fill: parent
            z: 1
            // Local video uses VideoOutput only. Passive remote spans create a
            // painted surface once their shared source holds a resident frame.
            active: root.remoteFrameMode && root.residencyReady
                    && root.remoteFrameSource.hasFrame === true
                    && mediaSurface.renderingAllowed
            sourceComponent: RemoteVideoFrameItem {
                frameSource: root.remoteFrameSource
            }
        }
    }

    onResidencyReadyChanged: {
        if (!residencyReady) localFrameSeen = false
        else Qt.callLater(bindPlayerToOutput)
    }

    onCppMediaPlayerChanged: {
        // Delegate role updates are not atomic: consume the player/sink pair
        // after both bindings have settled for this event-loop turn.
        Qt.callLater(bindPlayerToOutput)
    }

    onCppVideoSinkChanged: {
        Qt.callLater(bindPlayerToOutput)
    }

    onVisibleChanged: {
        if (visible)
            bindPlayerToOutput()
    }

    Component.onCompleted: {
        // Initial properties are settled here. Restore a resident frame before
        // the first canvas render; later role changes still coalesce above.
        bindPlayerToOutput()
    }

    Component.onDestruction: {
        restoreBoundPlayer()
    }

    // Playback state is still observed for resetting the loading affordance.
    Connections {
        target: root.cppMediaPlayer
        ignoreUnknownSignals: true
        function onPlaybackStateChanged(state) {
            if (!root.residencyReady)
                root.localFrameSeen = false
        }
    }

}
