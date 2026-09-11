import QtQuick 2.15
import QtMultimedia
import Mouffette.Canvas 1.0

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
    property bool videoHasPosterFrame: false
    property bool videoFirstFramePrimed: false
    readonly property bool remoteFrameMode: remoteFrameSource !== null
    readonly property bool showFallbackOverlay: !hasLiveFrame
    readonly property bool hasLiveFrame: remoteFrameMode
                                          ? remoteFrameSurface.hasFrame
                                          : (videoHasRenderedFrame || videoFirstFramePrimed || localFrameSeen)
    property bool localFrameSeen: false
    contentReady: hasLiveFrame

    function fallbackStatusText() {
        if (videoPlaybackErrorCode !== 0)
            return "Video error"
        if (!cppMediaPlayer)
            return "Video player unavailable"
        if (videoHasPosterFrame)
            return "Preparing video frame..."
        if (videoFirstFramePrimed)
            return "Waiting for video frame..."
        return "Loading video..."
    }

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
            // VideoOutput.videoSink is intentionally read-only in Qt 6. The
            // supported, accelerated path is to give the VideoOutput object to
            // QMediaPlayer and let Qt wire its internal sink.
            cppMediaPlayer.videoOutput = videoOutput
            boundMediaPlayer = cppMediaPlayer
            boundFallbackSink = cppVideoSink
        } catch (e) {
            boundMediaPlayer = null
            boundFallbackSink = null
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#141a24"
    }

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
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
        visible: root.remoteFrameMode
        frameSource: root.remoteFrameSource
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
            if (state === 0)
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

    Rectangle {
        anchors.fill: parent
        visible: root.showFallbackOverlay && !root.remoteFrameMode
        color: "#141a24"
        border.width: 1
        border.color: "#2a3240"

        Text {
            anchors.centerIn: parent
            color: "#d7deea"
            font.pixelSize: 12
            text: root.fallbackStatusText()
        }
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 4
        visible: root.videoPlaybackErrorCode !== 0 && root.videoPlaybackErrorString.length > 0
        color: "#ffb4b4"
        font.pixelSize: 10
        text: root.videoPlaybackErrorString
        elide: Text.ElideRight
        width: Math.max(0, parent.width - 12)
    }
}
