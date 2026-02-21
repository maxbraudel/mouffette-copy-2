import QtQuick 2.15
import QtQuick.Window 2.15

Rectangle {
    id: root
    color: "#10131a"

    signal mediaSelectRequested(string mediaId, bool additive)
    signal textCommitRequested(string mediaId, string text)
    signal textCreateRequested(real viewX, real viewY)

    property int screenCount: 0
    property bool remoteActive: false
    property var screensModel: []
    property var uiZonesModel: []
    property var mediaModel: []
    property bool textToolActive: false
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

    function fitToScreens(marginPx) {
        if (!screensModel || screensModel.length === 0)
            return false

        var minX = Number.POSITIVE_INFINITY
        var minY = Number.POSITIVE_INFINITY
        var maxX = Number.NEGATIVE_INFINITY
        var maxY = Number.NEGATIVE_INFINITY

        for (var i = 0; i < screensModel.length; ++i) {
            var s = screensModel[i]
            if (!s)
                continue
            minX = Math.min(minX, s.x)
            minY = Math.min(minY, s.y)
            maxX = Math.max(maxX, s.x + s.width)
            maxY = Math.max(maxY, s.y + s.height)
        }

        if (!isFiniteNumber(minX) || !isFiniteNumber(minY) || !isFiniteNumber(maxX) || !isFiniteNumber(maxY))
            return false

        var boundsW = Math.max(1.0, maxX - minX)
        var boundsH = Math.max(1.0, maxY - minY)
        var margin = Math.max(0.0, marginPx || 0.0)
        var availW = Math.max(1.0, viewport.width - margin * 2.0)
        var availH = Math.max(1.0, viewport.height - margin * 2.0)

        var sx = availW / boundsW
        var sy = availH / boundsH
        var fitScale = Math.min(sx, sy)
        if (!isFiniteNumber(fitScale) || fitScale <= 0.0)
            return false

        var cx = (minX + maxX) * 0.5
        var cy = (minY + maxY) * 0.5

        viewScale = fitScale
        panX = viewport.width * 0.5 - cx * fitScale
        panY = viewport.height * 0.5 - cy * fitScale
        return true
    }

    function recenterView(marginPx) {
        if (fitToScreens(marginPx === undefined ? 53 : marginPx))
            return
        panX = 0.0
        panY = 0.0
        viewScale = 1.0
    }

    function fitToBounds(boundsX, boundsY, boundsW, boundsH, marginPx) {
        if (!isFiniteNumber(boundsW) || !isFiniteNumber(boundsH) || boundsW <= 0.0 || boundsH <= 0.0) {
            recenterView()
            return
        }

        var margin = Math.max(0.0, marginPx || 0.0)
        var availW = Math.max(1.0, viewport.width - margin * 2.0)
        var availH = Math.max(1.0, viewport.height - margin * 2.0)
        var sx = availW / boundsW
        var sy = availH / boundsH
        var targetScale = clampScale(Math.min(sx, sy))

        if (!isFiniteNumber(targetScale) || targetScale <= 0.0)
            targetScale = 1.0

        viewScale = targetScale

        var contentW = boundsW * targetScale
        var contentH = boundsH * targetScale
        var left = margin + (availW - contentW) * 0.5
        var top = margin + (availH - contentH) * 0.5

        panX = left - boundsX * targetScale
        panY = top - boundsY * targetScale
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

    function mediaSourceUrl(path) {
        if (!path || path.length === 0)
            return ""
        if (path.indexOf("file:") === 0 || path.indexOf("qrc:") === 0 || path.indexOf("http:") === 0 || path.indexOf("https:") === 0)
            return path
        return "file://" + path
    }

    Item {
        id: viewport
        anchors.fill: parent
        clip: true

        Item {
            id: contentRoot
            width: viewport.width
            height: viewport.height
            x: panX
            y: panY
            scale: viewScale
            transformOrigin: Item.TopLeft

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

            Repeater {
                model: root.mediaModel
                delegate: Loader {
                    property var media: modelData
                    sourceComponent: {
                        if (!media)
                            return null
                        if (media.mediaType === "video")
                            return videoDelegate
                        if (media.mediaType === "text")
                            return textDelegate
                        return imageDelegate
                    }
                }
            }

            Component {
                id: imageDelegate
                ImageItem {
                    mediaId: parent.media.mediaId || ""
                    mediaX: parent.media.x
                    mediaY: parent.media.y
                    mediaWidth: parent.media.width
                    mediaHeight: parent.media.height
                    mediaZ: parent.media.z
                    selected: !!parent.media.selected
                    imageSource: root.mediaSourceUrl(parent.media.sourcePath)
                    onSelectRequested: function(mediaId, additive) {
                        root.mediaSelectRequested(mediaId, additive)
                    }
                }
            }

            Component {
                id: videoDelegate
                VideoItem {
                    mediaId: parent.media.mediaId || ""
                    mediaX: parent.media.x
                    mediaY: parent.media.y
                    mediaWidth: parent.media.width
                    mediaHeight: parent.media.height
                    mediaZ: parent.media.z
                    selected: !!parent.media.selected
                    source: root.mediaSourceUrl(parent.media.sourcePath)
                    onSelectRequested: function(mediaId, additive) {
                        root.mediaSelectRequested(mediaId, additive)
                    }
                }
            }

            Component {
                id: textDelegate
                TextItem {
                    mediaId: parent.media.mediaId || ""
                    mediaX: parent.media.x
                    mediaY: parent.media.y
                    mediaWidth: parent.media.width
                    mediaHeight: parent.media.height
                    mediaZ: parent.media.z
                    selected: !!parent.media.selected
                    textContent: parent.media.textContent || ""
                    horizontalAlignment: parent.media.textHorizontalAlignment || "center"
                    verticalAlignment: parent.media.textVerticalAlignment || "center"
                    fitToTextEnabled: !!parent.media.fitToTextEnabled
                    fontFamily: parent.media.textFontFamily || "Arial"
                    fontPixelSize: Math.max(1, parent.media.textFontPixelSize || 22)
                    fontWeight: parent.media.textFontWeight || 400
                    fontItalic: !!parent.media.textItalic
                    fontUnderline: !!parent.media.textUnderline
                    fontUppercase: !!parent.media.textUppercase
                    textColor: parent.media.textColor || "#FFFFFFFF"
                    outlineWidthPercent: parent.media.textOutlineWidthPercent || 0.0
                    outlineColor: parent.media.textOutlineColor || "#FF000000"
                    highlightEnabled: !!parent.media.textHighlightEnabled
                    highlightColor: parent.media.textHighlightColor || "#00000000"
                    textEditable: !!parent.media.textEditable
                    onSelectRequested: function(mediaId, additive) {
                        root.mediaSelectRequested(mediaId, additive)
                    }
                    onTextCommitRequested: function(mediaId, text) {
                        root.textCommitRequested(mediaId, text)
                    }
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

        TapHandler {
            id: textToolTap
            target: null
            acceptedButtons: Qt.LeftButton

            onTapped: function(eventPoint) {
                if (!root.textToolActive)
                    return
                root.textCreateRequested(eventPoint.position.x, eventPoint.position.y)
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
                    root.panX += dx
                    root.panY += dyPan
                    event.accepted = true
                }
            }
        }
    }
}
