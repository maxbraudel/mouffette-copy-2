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
    property int videoPlaybackErrorCode: 0
    property string videoPlaybackErrorString: ""
    property bool videoHasRenderedFrame: false
    property bool videoFirstFramePrimed: false
    property bool residencyReady: false
    property bool requireInitialSkeleton: true
    readonly property bool remoteFrameMode: remoteFrameSource !== null
    readonly property bool hasLiveFrame: remoteFrameMode
                                          ? remoteFrameSurface.hasFrame : localFrameSeen
    property bool localFrameSeen: false
    contentReady: root.residencyReady && hasLiveFrame
    initialFramePresented: !requireInitialSkeleton || mediaSurface.firstFramePresented

    function restoreBoundPlayer() {
        var previousPlayer = boundMediaPlayer
        var previousSink = boundFallbackSink
        boundMediaPlayer = null
        boundFallbackSink = null
        if (!previousPlayer || !("videoOutput" in previousPlayer))
            return
        try {
            // Restore only our own binding. A newer delegate/output must never
            // be detached by destruction of an older delegate.
            if (previousPlayer.videoOutput === videoOutput)
                previousPlayer.videoOutput = previousSink
        } catch (e) { }
    }

    function bindPlayerToOutput() {
        if (!videoOutput || !cppMediaPlayer || !("videoOutput" in cppMediaPlayer)) {
            restoreBoundPlayer()
            return
        }

        if (boundMediaPlayer === cppMediaPlayer) {
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
        } catch (e) {
            boundMediaPlayer = null
            boundFallbackSink = null
        }
    }

    MediaSurface {
        id: mediaSurface
        anchors.fill: parent
        requireInitialSkeleton: root.requireInitialSkeleton
        contentReady: root.residencyReady && root.hasLiveFrame

        VideoOutput {
            id: videoOutput
            anchors.fill: parent
            z: 1
            fillMode: VideoOutput.Stretch
            visible: !root.remoteFrameMode

            onWindowChanged: function(window) {
                if (window)
                    Qt.callLater(root.bindPlayerToOutput)
            }
        }

        RemoteVideoFrameItem {
            id: remoteFrameSurface
            anchors.fill: parent
            z: 1
            visible: root.remoteFrameMode
            frameSource: root.remoteFrameSource
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
        Qt.callLater(bindPlayerToOutput)
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

    Connections {
        target: videoOutput ? videoOutput.videoSink : null
        ignoreUnknownSignals: true
        function onVideoFrameChanged(frame) {
            root.localFrameSeen = true
        }
    }

}
