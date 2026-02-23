import QtQuick 2.15

// Horizontal slider matching the legacy OverlaySliderElement style.
// Track + fill + draggable thumb. Value range [0, 1].
// Blocks pointer events from reaching canvas handlers beneath it.
Item {
    id: root

    property real value: 0.0       // 0..1, read/write
    property bool enabled: true

    signal seeked(real ratio)      // emitted on drag/click with new value
    signal dragStarted(real ratio)
    signal dragEnded(real ratio)

    implicitWidth: 120
    implicitHeight: 16

    // Track background
    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        height: 4
        radius: 2
        color: "#40FFFFFF"
    }

    // Fill
    Rectangle {
        id: fill
        anchors.verticalCenter: track.verticalCenter
        anchors.left: track.left
        width: Math.max(0, Math.min(track.width, root.value * track.width))
        height: track.height
        radius: track.radius
        color: sliderArea.pressed ? "#CC4A90E2" : "#99FFFFFF"
        Behavior on color { ColorAnimation { duration: 80 } }
    }

    // Thumb
    Rectangle {
        id: thumb
        width: 12
        height: 12
        radius: 6
        color: sliderArea.pressed ? "#4A90E2" : "#FFFFFF"
        anchors.verticalCenter: track.verticalCenter
        x: Math.max(0, Math.min(track.width - width, root.value * track.width - width * 0.5))
        Behavior on color { ColorAnimation { duration: 80 } }
    }

    // Input capture
    MouseArea {
        id: sliderArea
        anchors.fill: parent
        enabled: root.enabled
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true

        function ratioFromX(mx) {
            return Math.max(0.0, Math.min(1.0, mx / track.width))
        }

        onPressed: function(mouse) {
            mouse.accepted = true
            var r = ratioFromX(mouse.x)
            root.value = r
            root.dragStarted(r)
            root.seeked(r)
        }

        onPositionChanged: function(mouse) {
            if (!pressed) return
            var r = ratioFromX(mouse.x)
            root.value = r
            root.seeked(r)
        }

        onReleased: function(mouse) {
            mouse.accepted = true
            root.dragEnded(root.value)
        }
    }
}
