import QtQuick 2.15
import QtMultimedia

BaseMediaItem {
    id: root

    property var cppVideoSink: null

    Rectangle {
        anchors.fill: parent
        color: "#141a24"
    }

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
        fillMode: VideoOutput.Stretch
    }

    onCppVideoSinkChanged: {
        if (root.cppVideoSink) {
            videoOutput.videoSink = root.cppVideoSink
        }
    }
}
