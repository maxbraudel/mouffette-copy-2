import QtQuick 2.15
import QtMultimedia

BaseMediaItem {
    id: root
    property url source: ""

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
