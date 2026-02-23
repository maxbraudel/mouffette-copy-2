import QtQuick 2.15

// Reusable overlay icon button matching the legacy OverlayButtonElement style.
// Blocks pointer events from reaching the canvas DragHandler/PointHandler beneath it.
Item {
    id: root

    property string iconSource: ""
    property bool isToggle: false
    property bool toggled: false
    property bool enabled: true

    signal clicked()

    implicitWidth: 36
    implicitHeight: 36

    // Background pill
    Rectangle {
        id: bg
        anchors.fill: parent
        radius: 6
        color: {
            if (!root.enabled)
                return "#55111827"
            if (root.isToggle && root.toggled)
                return "#CC4A90E2"
            if (pressArea.containsPress)
                return "#CC4A90E2"
            if (pressArea.containsMouse)
                return "#CC2a3a50"
            return "#CC1e2535"
        }
        border.color: "#40FFFFFF"
        border.width: 1

        Behavior on color { ColorAnimation { duration: 80 } }
    }

    // SVG icon — rasterized at 4× source size so it stays sharp at any
    // display scale. Displayed at 16×16 logical px, downscaled smoothly.
    Image {
        id: icon
        anchors.centerIn: parent
        width: 16
        height: 16
        source: root.iconSource
        sourceSize: Qt.size(64, 64)
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        opacity: root.enabled ? 1.0 : 0.35
    }

    // Input capture — blocks drag/pan handlers beneath
    MouseArea {
        id: pressArea
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        acceptedButtons: Qt.LeftButton
        onClicked: {
            root.clicked()
        }
        // Consume the event so it never reaches the canvas DragHandler
        onPressed: mouse.accepted = true
        onReleased: mouse.accepted = true
    }
}
