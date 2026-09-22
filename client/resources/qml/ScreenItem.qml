import QtQuick
import Mouffette.Canvas
import Mouffette.App as AppStyle
Rectangle {
    id: root

    property real screenX: 0
    property real screenY: 0
    property real screenWidth: 0
    property real screenHeight: 0
    property bool primary: false

    // Label properties — set by the parent Repeater delegate
    property real viewScale: 1.0
    readonly property real safeViewScale: viewScale > 0.0 ? viewScale : 1.0
    property int  screenIndex: 1
    property int  screenId: -1
    property int  pixelWidth: 0
    property int  pixelHeight: 0
    property var uiZonesModel: []
    property var frameSource: null

    x: screenX
    y: screenY
    width: screenWidth
    height: screenHeight
    z: -1000
    clip: false

    color: root.primary ? AppStyle.Theme.canvasPrimaryScreenBackground : AppStyle.Theme.canvasScreenBackground

    // Per-screen UI zones (taskbar/menu bar/dock) rendered INSIDE this screen,
    // so they look painted into the screen background rather than global overlays.
    Item {
        id: zoneLayer
        anchors.fill: parent
        z: 0
        clip: true
        visible: !screenVideo.hasFrame

        Repeater {
            model: root.uiZonesModel || []
            delegate: Rectangle {
                readonly property bool belongsToScreen: (modelData && modelData.screenId === root.screenId)
                readonly property string zoneType: belongsToScreen ? String(modelData.type || "").toLowerCase() : ""
                readonly property bool systemZone: zoneType === "taskbar" || zoneType === "dock" || zoneType === "menu_bar"
                visible: belongsToScreen

                x: belongsToScreen ? (modelData.x - root.screenX) : 0
                y: belongsToScreen ? (modelData.y - root.screenY) : 0
                width: belongsToScreen ? modelData.width : 0
                height: belongsToScreen ? modelData.height : 0

                color: !belongsToScreen ? "transparent"
                     : systemZone ? AppStyle.Theme.uiZoneSystemFill : AppStyle.Theme.uiZoneFill
                border.width: 0
            }
        }
    }

    // The immutable QVideoFrame stays in C++; the existing video scene-graph
    // item imports its YUV planes directly into the Metal/D3D render pass.
    RemoteVideoFrameItem {
        id: screenVideo
        objectName: "remoteScreenVideo"
        anchors.fill: parent
        z: 0
        frameSource: root.frameSource
        visible: hasFrame
    }

    // Zoom-invariant inner border (always 1 screen pixel)
    // Uses the same counter-scale strategy as the screen label.
    Item {
        id: innerBorderOverlay
        x: 0
        y: 0
        z: 1
        width: Math.max(0, root.screenWidth * root.safeViewScale)
        height: Math.max(0, root.screenHeight * root.safeViewScale)

        transform: Scale {
            xScale: 1.0 / root.safeViewScale
            yScale: 1.0 / root.safeViewScale
            origin.x: 0
            origin.y: 0
        }

        readonly property color strokeColor: AppStyle.Theme.canvasScreenBorder

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
            xScale: 1.0 / root.safeViewScale
            yScale: 1.0 / root.safeViewScale
            // Scale from the anchor point (x:0, y:0 of this item)
            origin.x: 0
            origin.y: 0
        }

        // All coords inside labelAnchor are now in screen-pixel space
        Text {
            id: screenLabel
            text: "Screen " + root.screenIndex + " (" + root.pixelWidth + "x" + root.pixelHeight + ")"
            color: AppStyle.Theme.canvasScreenText
            font.family: "Arial"
            font.pixelSize: 13
            font.weight: Font.Bold
            // Horizontally center on the anchor (screen top-center)
            x: -implicitWidth / 2
            // 6 screen-pixels above the anchor (= 6 px above the screen top edge)
            y: -(implicitHeight + 6)
            // Theme-aware outline keeps the label legible over overlapping media.
            style: Text.Outline
            styleColor: AppStyle.Theme.canvasLabelShadow
        }
    }
}
