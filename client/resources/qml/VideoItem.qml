import QtQuick 2.15
import QtMultimedia

Item {
    id: root

    property real mediaX: 0
    property real mediaY: 0
    property real mediaWidth: 0
    property real mediaHeight: 0
    property real mediaZ: 0
    property bool selected: false
    property string mediaId: ""
    property url source: ""

    signal selectRequested(string mediaId, bool additive)

    x: mediaX
    y: mediaY
    width: mediaWidth
    height: mediaHeight
    z: mediaZ

    Rectangle {
        anchors.fill: parent
        color: "#141a24"
    }

    MediaPlayer {
        id: mediaPlayer
        source: root.source
        videoOutput: videoOutput
        audioOutput: AudioOutput {
            muted: true
            volume: 0.0
        }
    }

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: root.selected ? 2 : 0
        border.color: "#4A90E2"
    }

    MouseArea {
        anchors.fill: parent
        scrollGestureEnabled: false
        onPressed: function(mouse) {
            root.selectRequested(root.mediaId, (mouse.modifiers & Qt.ShiftModifier) !== 0)
            mouse.accepted = true
        }
    }

    onVisibleChanged: {
        if (!visible) {
            mediaPlayer.pause()
            return
        }
        if (root.source && root.source.toString().length > 0)
            mediaPlayer.play()
    }

    onSourceChanged: {
        if (!visible)
            return
        if (root.source && root.source.toString().length > 0)
            mediaPlayer.play()
    }
}
