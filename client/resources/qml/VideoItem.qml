import QtQuick 2.15
import QtMultimedia

BaseMediaItem {
    id: root

    property var cppMediaPlayer: null
    property var cppVideoSink: null
    property var boundMediaPlayer: null
    property bool boundViaSinkPath: false   // true when VideoOutput.videoSink was used
    property int videoPlaybackErrorCode: 0
    property string videoPlaybackErrorString: ""
    property bool videoHasRenderedFrame: false
    property bool videoHasPosterFrame: false
    property bool videoFirstFramePrimed: false
    readonly property bool showFallbackOverlay: !hasLiveFrame
    readonly property bool hasLiveFrame: videoHasRenderedFrame || videoFirstFramePrimed || localFrameSeen
    property bool localFrameSeen: false

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

    function bindPlayerToOutput() {
        if (!videoOutput)
            return

        if (boundViaSinkPath && cppVideoSink && videoOutput.videoSink === cppVideoSink)
            return

        if (!boundViaSinkPath && boundMediaPlayer === cppMediaPlayer && cppMediaPlayer && ("videoOutput" in cppMediaPlayer)) {
            try {
                if (cppMediaPlayer.videoOutput === videoOutput)
                    return
            } catch (e) { }
        }

        // Clear any old binding
        if (boundViaSinkPath) {
            try { videoOutput.videoSink = null } catch (e) { }
        } else if (boundMediaPlayer && ("videoOutput" in boundMediaPlayer)) {
            try { boundMediaPlayer.videoOutput = null } catch (e) { }
        }
        boundMediaPlayer = null
        boundViaSinkPath = false

        // Primary path: assign C++ QVideoSink directly to the VideoOutput so it
        // renders every frame the C++ QMediaPlayer delivers to m_sink.  Read-only
        // in some Qt6 builds – fall through silently in that case.
        if (cppVideoSink) {
            try {
                videoOutput.videoSink = cppVideoSink
                boundMediaPlayer = cppMediaPlayer
                boundViaSinkPath = true
                return
            } catch (e) {
                // videoSink is read-only in this Qt build; use player.videoOutput below
            }
        }

        // Fallback: QMediaPlayer.videoOutput = VideoOutput.  Qt internally calls
        // setVideoSink(videoOutput.videoSink) internally.  The VideoOutput's internal
        // QVideoSink is only initialised after the item is placed in a Window (the
        // scenegraph creates it on first render).  If it is still null we defer
        // and retry; once it is non-null the assignment wires up the video pipeline.
        if (!cppMediaPlayer)
            return
        if (!("videoOutput" in cppMediaPlayer))
            return

        // Guard: if the VideoOutput sink is not yet ready, retry after current
        // event loop iteration (window may not have rendered yet).
        if (!videoOutput.videoSink) {
            Qt.callLater(bindPlayerToOutput)
            return
        }

        try {
            cppMediaPlayer.videoOutput = videoOutput
            boundMediaPlayer = cppMediaPlayer
            boundViaSinkPath = false
        } catch (e) {
            boundMediaPlayer = null
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

        // Retrigger binding when the VideoOutput enters a Window and the
        // scenegraph initialises its internal QVideoSink.
        onWindowChanged: {
            if (window)
                Qt.callLater(root.bindPlayerToOutput)
        }
    }

    onCppMediaPlayerChanged: {
        bindPlayerToOutput()
    }

    onCppVideoSinkChanged: {
        bindPlayerToOutput()
    }

    onVisibleChanged: {
        if (visible)
            bindPlayerToOutput()
    }

    Component.onCompleted: {
        // Defer one event-loop tick so the VideoOutput is placed in its
        // parent Window and its internal QVideoSink is non-null.
        Qt.callLater(bindPlayerToOutput)
    }

    Component.onDestruction: {
        // Undo only what we set; avoid touching the C++ player's sink
        // when we went through the primary videoOutput.videoSink path.
        if (boundViaSinkPath) {
            try { videoOutput.videoSink = null } catch (e) { }
        } else if (boundMediaPlayer && ("videoOutput" in boundMediaPlayer)) {
            try { boundMediaPlayer.videoOutput = null } catch (e) { }
        }
    }

    // Primary path (videoOutput.videoSink = cppVideoSink worked): the C++ m_sink
    // still receives every decoded frame, so watch it directly.
    Connections {
        target: root.boundViaSinkPath ? root.cppVideoSink : null
        ignoreUnknownSignals: true
        function onVideoFrameChanged(frame) {
            root.localFrameSeen = true
        }
    }

    // Fallback path (cppMediaPlayer.videoOutput = videoOutput): the player was
    // internally rewired to the VideoOutput's own sink so cppVideoSink no longer
    // fires.  Use positionChanged (fires every ~100 ms during playback) to detect
    // that frames are actually being delivered and rendered.
    Connections {
        target: !root.boundViaSinkPath ? root.cppMediaPlayer : null
        ignoreUnknownSignals: true
        function onPositionChanged(position) {
            if (position >= 0)
                root.localFrameSeen = true
        }
        // Reset when the user stops/rewinds so the overlay re-appears if needed
        function onPlaybackStateChanged(state) {
            // 0 = StoppedState – re-arm so overlay shows on next play start
            if (state === 0)
                root.localFrameSeen = false
        }
    }

    Connections {
        target: (!root.boundViaSinkPath && videoOutput && videoOutput.videoSink) ? videoOutput.videoSink : null
        ignoreUnknownSignals: true
        function onVideoFrameChanged(frame) {
            root.localFrameSeen = true
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.showFallbackOverlay
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
