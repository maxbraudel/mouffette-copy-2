import QtQuick 2.15
import QtMultimedia

Item {
    id: root

    property real mediaX: 0
    property real mediaY: 0
    property real mediaWidth: 0
    property real mediaHeight: 0
    property real mediaScale: 1.0
    property real mediaZ: 0
    property bool selected: false
    property string mediaId: ""
    property url source: ""

    signal selectRequested(string mediaId, bool additive)

    x: mediaX
    y: mediaY
    width: mediaWidth
    height: mediaHeight
    scale: mediaScale
    transformOrigin: Item.TopLeft
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

    MouseArea {
        anchors.fill: parent
        scrollGestureEnabled: false
        onPressed: function(mouse) {
            var additive = (mouse.modifiers & Qt.ShiftModifier) !== 0
            var shouldSelect = additive || !root.selected
            if (shouldSelect)
                root.selectRequested(root.mediaId, additive)
            mouse.accepted = shouldSelect
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
