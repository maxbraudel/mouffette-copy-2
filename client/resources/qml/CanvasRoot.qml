import QtQuick 2.15
import QtQuick.Window 2.15

Rectangle {
    id: root
    color: "#10131a"

    property int screenCount: 0
    property bool remoteActive: false
    property var screensModel: []
    property var uiZonesModel: []
    property bool remoteCursorVisible: false
    property real remoteCursorX: 0
    property real remoteCursorY: 0
    property int remoteCursorDiameter: 30
    property color remoteCursorFill: "#FFFFFFFF"
    property color remoteCursorBorder: "#E6000000"
    property real remoteCursorBorderWidth: 2
    property real viewScale: 1.0
    property real panX: 0.0
    property real panY: 0.0
    property real minScale: 0.25
    property real maxScale: 4.0
    property real wheelZoomBase: 1.0015

    function clampScale(value) {
        return Math.max(minScale, Math.min(maxScale, value))
    }

    function applyZoomAt(anchorX, anchorY, factor) {
        if (!isFiniteNumber(factor) || factor <= 0.0)
            return

        var oldScale = viewScale
        var nextScale = clampScale(oldScale * factor)
        if (!isFiniteNumber(nextScale) || Math.abs(nextScale - oldScale) < 0.000001)
            return

        var worldX = (anchorX - panX) / oldScale
        var worldY = (anchorY - panY) / oldScale

        viewScale = nextScale
        panX = anchorX - worldX * nextScale
        panY = anchorY - worldY * nextScale
    }

    function recenterView() {
        panX = 0.0
        panY = 0.0
    }

    function isZoomModifier(modifiers) {
        if (Qt.platform.os === "osx") {
            return (modifiers & Qt.MetaModifier) !== 0
        }
        return (modifiers & Qt.ControlModifier) !== 0
    }

    function wheelDeltaY(wheel) {
        if (wheel.pixelDelta && wheel.pixelDelta.y !== 0)
            return wheel.pixelDelta.y
        if (wheel.angleDelta)
            return wheel.angleDelta.y / 8.0
        return 0.0
    }

    function wheelDeltaX(wheel) {
        if (wheel.pixelDelta && wheel.pixelDelta.x !== 0)
            return wheel.pixelDelta.x
        if (wheel.angleDelta)
            return wheel.angleDelta.x / 8.0
        return 0.0
    }

    function isFiniteNumber(v) {
        return v !== Infinity && v !== -Infinity && !isNaN(v)
    }

    Item {
        id: viewport
        anchors.fill: parent

        Item {
            id: contentRoot
            anchors.fill: parent
            x: panX
            y: panY
            scale: viewScale

            Rectangle {
                anchors.fill: parent
                anchors.margins: 12
                radius: 10
                color: "#151b24"
                border.width: 1
                border.color: remoteActive ? "#4a90e2" : "#2b3444"
            }

            Text {
                anchors.centerIn: parent
                color: "#7f8aa3"
                text: "Quick Canvas Shell · screens: " + screenCount
                font.pixelSize: 14
            }

            Repeater {
                model: root.screensModel
                delegate: ScreenItem {
                    screenX: modelData.x
                    screenY: modelData.y
                    screenWidth: modelData.width
                    screenHeight: modelData.height
                    primary: modelData.primary
                }
            }

            Repeater {
                model: root.uiZonesModel
                delegate: UiZone {
                    zoneX: modelData.x
                    zoneY: modelData.y
                    zoneWidth: modelData.width
                    zoneHeight: modelData.height
                    fillColor: modelData.fillColor
                }
            }

            RemoteCursor {
                cursorVisible: root.remoteCursorVisible
                cursorX: root.remoteCursorX
                cursorY: root.remoteCursorY
                diameter: root.remoteCursorDiameter
                fillColor: root.remoteCursorFill
                borderColor: root.remoteCursorBorder
                borderWidth: root.remoteCursorBorderWidth
            }
        }

        DragHandler {
            id: panDrag
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedButtons: Qt.LeftButton
            property real startPanX: 0.0
            property real startPanY: 0.0

            onActiveChanged: {
                if (active) {
                    startPanX = root.panX
                    startPanY = root.panY
                }
            }

            onTranslationChanged: {
                root.panX = startPanX + translation.x
                root.panY = startPanY + translation.y
            }
        }

        PinchHandler {
            id: pinchZoom
            target: null
            property real lastScale: 1.0

            onActiveChanged: {
                if (active)
                    lastScale = 1.0
            }

            onScaleChanged: {
                if (!active)
                    return
                var factor = scale / lastScale
                if (isFiniteNumber(factor) && factor > 0.0)
                    root.applyZoomAt(centroid.position.x, centroid.position.y, factor)
                lastScale = scale
            }
        }

        WheelHandler {
            id: wheelInput
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

            onWheel: function(event) {
                if (root.isZoomModifier(event.modifiers)) {
                    var dy = root.wheelDeltaY(event)
                    if (dy !== 0.0) {
                        var factor = Math.pow(root.wheelZoomBase, dy)
                        root.applyZoomAt(event.x, event.y, factor)
                        event.accepted = true
                        return
                    }
                }

                var dx = root.wheelDeltaX(event)
                var dyPan = root.wheelDeltaY(event)
                if (dx !== 0.0 || dyPan !== 0.0) {
                    root.panX -= dx
                    root.panY -= dyPan
                    event.accepted = true
                }
            }
        }
    }
}
