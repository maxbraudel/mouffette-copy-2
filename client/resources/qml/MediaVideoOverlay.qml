import QtQuick 2.15
import QtQuick.Layouts 1.15

// Bottom overlay panel attached below a selected video media item.
// Mirrors the legacy ResizableVideoItem controls panel:
//   Row 1: play/pause | stop | repeat | mute | volume slider
//   Row 2: progress bar (full width)
// Positioned in item-local coordinates: y = parent.height + gap
Item {
    id: root

    property string mediaId: ""
    property bool isPlaying: false
    property bool isMuted: false
    property bool isLooping: false
    property real progress: 0.0    // 0..1
    property real volume: 1.0      // 0..1

    signal playPauseRequested(string mediaId)
    signal stopRequested(string mediaId)
    signal repeatToggleRequested(string mediaId)
    signal muteToggleRequested(string mediaId)
    signal volumeChangeRequested(string mediaId, real value)
    signal seekRequested(string mediaId, real ratio)
    signal overlayHoveredChanged(bool hovered)

    readonly property real paddingH: 8
    readonly property real paddingV: 6
    readonly property real itemSpacing: 4
    readonly property real btnSize: 28
    readonly property real sliderMinWidth: 60

    // panelWidth/panelHeight exposed so CanvasRoot can read them for centering.
    readonly property real panelWidth: implicitWidth
    readonly property real panelHeight: implicitHeight

    implicitWidth:  controlsRow.implicitWidth + paddingH * 2
    implicitHeight: btnSize + 14 + itemSpacing + paddingV * 2  // row + progress + gap + padding

    // Background pill
    Rectangle {
        anchors.fill: parent
        radius: 8
        color: "#CC1e2535"
        border.color: "#40FFFFFF"
        border.width: 1
    }

    // Block all pointer events from reaching canvas below
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        onPressed: mouse.accepted = true
        onReleased: mouse.accepted = true
        onEntered: root.overlayHoveredChanged(true)
        onExited: root.overlayHoveredChanged(false)
    }

    // Controls row — defines the container width.
    // Buttons are fixed square; volume slider expands to fill remaining space.
    RowLayout {
        id: controlsRow
        x: root.paddingH
        y: root.paddingV
        // Width = container width minus padding (container sizes from our implicitWidth)
        width: root.width - root.paddingH * 2
        spacing: root.itemSpacing

        OverlayButton {
            iconSource: root.isPlaying ? "qrc:/icons/icons/pause.svg" : "qrc:/icons/icons/play.svg"
            Layout.preferredWidth: root.btnSize
            Layout.preferredHeight: root.btnSize
            onClicked: root.playPauseRequested(root.mediaId)
        }
        OverlayButton {
            iconSource: "qrc:/icons/icons/stop.svg"
            Layout.preferredWidth: root.btnSize
            Layout.preferredHeight: root.btnSize
            onClicked: root.stopRequested(root.mediaId)
        }
        OverlayButton {
            iconSource: "qrc:/icons/icons/loop.svg"
            isToggle: true
            toggled: root.isLooping
            Layout.preferredWidth: root.btnSize
            Layout.preferredHeight: root.btnSize
            onClicked: root.repeatToggleRequested(root.mediaId)
        }
        OverlayButton {
            iconSource: root.isMuted ? "qrc:/icons/icons/volume-off.svg" : "qrc:/icons/icons/volume-on.svg"
            isToggle: true
            toggled: root.isMuted
            Layout.preferredWidth: root.btnSize
            Layout.preferredHeight: root.btnSize
            onClicked: root.muteToggleRequested(root.mediaId)
        }
        // Volume slider: minimum width, expands to fill remaining space
        OverlaySlider {
            id: volumeSlider
            value: root.isMuted ? 0.0 : root.volume
            Layout.fillWidth: true
            Layout.minimumWidth: root.sliderMinWidth
            Layout.preferredHeight: root.btnSize
            onSeeked: function(r) { root.volumeChangeRequested(root.mediaId, r) }
        }
    }

    // Progress bar — full container width
    OverlaySlider {
        id: progressSlider
        value: root.progress
        x: root.paddingH
        y: root.paddingV + root.btnSize + root.itemSpacing
        width: root.width - root.paddingH * 2
        height: 14
        onDragStarted: function(r) { root.seekRequested(root.mediaId, r) }
        onSeeked:      function(r) { root.seekRequested(root.mediaId, r) }
    }
}
