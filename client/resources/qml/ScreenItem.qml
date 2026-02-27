import QtQuick 2.15

Rectangle {
    id: root

    property real screenX: 0
    property real screenY: 0
    property real screenWidth: 0
    property real screenHeight: 0
    property bool primary: false

    // Label properties — set by the parent Repeater delegate
    property real viewScale: 1.0
    property int  screenIndex: 1
    property int  screenId: -1
    property int  pixelWidth: 0
    property int  pixelHeight: 0
    property var uiZonesModel: []

    x: screenX
    y: screenY
    width: screenWidth
    height: screenHeight
    z: -1000
    clip: false

    color: primary ? "#B44A90E2" : "#B4505050"

    // Per-screen UI zones (taskbar/menu bar/dock) rendered INSIDE this screen,
    // so they look painted into the screen background rather than global overlays.
    Item {
        id: zoneLayer
        anchors.fill: parent
        z: 0
        clip: true

        Repeater {
            model: root.uiZonesModel || []
            delegate: Rectangle {
                readonly property bool belongsToScreen: (modelData && modelData.screenId === root.screenId)
                visible: belongsToScreen

                x: belongsToScreen ? (modelData.x - root.screenX) : 0
                y: belongsToScreen ? (modelData.y - root.screenY) : 0
                width: belongsToScreen ? modelData.width : 0
                height: belongsToScreen ? modelData.height : 0

                color: belongsToScreen ? modelData.fillColor : "transparent"
                border.width: 0
            }
        }
    }

    // Zoom-invariant inner border (always 1 screen pixel)
    // Uses the same counter-scale strategy as the screen label.
    Item {
        id: innerBorderOverlay
        x: 0
        y: 0
        z: 1
        width: Math.max(0, root.screenWidth * Math.max(0.001, root.viewScale))
        height: Math.max(0, root.screenHeight * Math.max(0.001, root.viewScale))

        transform: Scale {
            xScale: 1.0 / Math.max(0.001, root.viewScale)
            yScale: 1.0 / Math.max(0.001, root.viewScale)
            origin.x: 0
            origin.y: 0
        }

        readonly property color strokeColor: "#A0A0A0"

        Rectangle {
            x: 0
            y: 0
            width: innerBorderOverlay.width
            height: 1
            color: innerBorderOverlay.strokeColor
        }
        Rectangle {
            x: 0
            y: Math.max(0, innerBorderOverlay.height - 1)
            width: innerBorderOverlay.width
            height: 1
            color: innerBorderOverlay.strokeColor
        }
        Rectangle {
            x: 0
            y: 0
            width: 1
            height: innerBorderOverlay.height
            color: innerBorderOverlay.strokeColor
        }
        Rectangle {
            x: Math.max(0, innerBorderOverlay.width - 1)
            y: 0
            width: 1
            height: innerBorderOverlay.height
            color: innerBorderOverlay.strokeColor
        }
    }

    // ── Screen name label ──────────────────────────────────────────────────
    // Rendered at a fixed screen-pixel size regardless of canvas zoom,
    // centered just above the top edge of the screen rectangle.
    // Technique: anchor a zero-size Item at the screen's top-center in scene
    // coordinates, then apply a counter-scale of 1/viewScale. Children of
    // that item are then in true screen-pixel space.
    Item {
        id: labelAnchor
        // Top-center of the screen in scene (parent) coordinates
        x: root.screenWidth / 2
        y: 0
        width: 0
        height: 0
        clip: false

        transform: Scale {
            xScale: 1.0 / Math.max(0.001, root.viewScale)
            yScale: 1.0 / Math.max(0.001, root.viewScale)
            // Scale from the anchor point (x:0, y:0 of this item)
            origin.x: 0
            origin.y: 0
        }

        // All coords inside labelAnchor are now in screen-pixel space
        Text {
            id: screenLabel
            text: "Screen " + root.screenIndex + " (" + root.pixelWidth + "x" + root.pixelHeight + ")"
            color: "#FFFFFF"
            font.family: "Arial"
            font.pixelSize: 13
            font.weight: Font.Bold
            // Horizontally center on the anchor (screen top-center)
            x: -implicitWidth / 2
            // 6 screen-pixels above the anchor (= 6 px above the screen top edge)
            y: -(implicitHeight + 6)
            // Thin dark outline for readability on any background color
            style: Text.Outline
            styleColor: "#99000000"
        }
    }
}
