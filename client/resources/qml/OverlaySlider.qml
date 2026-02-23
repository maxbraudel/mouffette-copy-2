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

    // Track background — gOverlayBorderColor = QColor(100,100,100,255)
    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        height: 4
        radius: 2
        color: "#FF646464"
    }

    // Fill — gOverlayActiveBackgroundColor = QColor(52,87,128,240)
    Rectangle {
        id: fill
        anchors.verticalCenter: track.verticalCenter
        anchors.left: track.left
        width: Math.max(0, Math.min(track.width, root.value * track.width))
        height: track.height
        radius: track.radius
        color: "#F2345780"
    }

    // Thumb
    Rectangle {
        id: thumb
        width: 12
        height: 12
        radius: 6
        color: "#FFFFFF"
        anchors.verticalCenter: track.verticalCenter
        x: Math.max(0, Math.min(track.width - width, root.value * track.width - width * 0.5))
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
