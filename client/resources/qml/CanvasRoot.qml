import QtQuick 2.15
import QtQuick.Window 2.15

Rectangle {
    id: root
    color: "#10131a"

    signal mediaSelectRequested(string mediaId, bool additive)
    signal mediaMoveEnded(string mediaId, real sceneX, real sceneY)
    signal mediaResizeRequested(string mediaId, string handleId, real sceneX, real sceneY, bool snap)
    signal mediaResizeEnded(string mediaId)
    signal textCommitRequested(string mediaId, string text)
    signal textCreateRequested(real viewX, real viewY)

    property int screenCount: 0
    property bool remoteActive: false
    property var screensModel: []
    property var uiZonesModel: []
    property var mediaModel: []
    property var selectionChromeModel: []
    property var snapGuidesModel: []
    // Live drag tracking — updated every frame during a move drag, purely in QML
    property string liveDragMediaId: ""
    property real liveDragViewOffsetX: 0.0
    property real liveDragViewOffsetY: 0.0
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

            // Each media item is a native QML Item with its own local x/y/scale.
            // A DragHandler lives here (inside contentRoot, which has scale:viewScale)
            // so DragHandler translation is already in scene coordinates — no C++ per frame.
            Repeater {
                model: root.mediaModel
                delegate: Item {
                    id: mediaDelegate
                    property var media: modelData

                    // Local position/scale — tracks model when idle, free during drag
                    property real localX: media ? media.x : 0.0
                    property real localY: media ? media.y : 0.0
                    property real localScale: media ? (media.scale || 1.0) : 1.0
                    property bool localDragging: false
                    readonly property string currentMediaId: media ? (media.mediaId || "") : ""

                    onMediaChanged: {
                        if (!localDragging && media) {
                            var syncedX = Math.abs(localX - media.x) < 0.01
                            var syncedY = Math.abs(localY - media.y) < 0.01
                            localX = media.x
                            localY = media.y
                            localScale = media.scale || 1.0
                            if (root.liveDragMediaId === currentMediaId) {
                                if (syncedX && syncedY) {
                                    root.liveDragMediaId = ""
                                    root.liveDragViewOffsetX = 0.0
                                    root.liveDragViewOffsetY = 0.0
                                }
                            }
                        }
                    }

                    x: localX
                    y: localY
                    width: media ? Math.max(1, media.width) : 1
                    height: media ? Math.max(1, media.height) : 1
                    scale: localScale
                    transformOrigin: Item.TopLeft
                    z: media ? media.z : 0
                    visible: !!media

                    // DragHandler inside contentRoot: translation is in scene coords (contentRoot
                    // has scale:viewScale, so 1 unit here = 1 scene unit, not viewport pixel).
                    DragHandler {
                        id: mediaDrag
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        acceptedButtons: Qt.LeftButton
                        enabled: !!mediaDelegate.media
                                 && !!mediaDelegate.media.selected
                                 && !mediaDelegate.media.textEditable
                                 && !root.textToolActive

                        property real startX: 0.0
                        property real startY: 0.0

                        onActiveChanged: {
                            if (active) {
                                startX = mediaDelegate.localX
                                startY = mediaDelegate.localY
                                mediaDelegate.localDragging = true
                                root.liveDragMediaId = mediaDelegate.currentMediaId
                            } else {
                                mediaDelegate.localDragging = false
                                if (mediaDelegate.currentMediaId !== "") {
                                    root.liveDragMediaId = mediaDelegate.currentMediaId
                                    root.mediaMoveEnded(mediaDelegate.currentMediaId,
                                                        mediaDelegate.localX,
                                                        mediaDelegate.localY)
                                } else {
                                    root.liveDragMediaId = ""
                                    root.liveDragViewOffsetX = 0.0
                                    root.liveDragViewOffsetY = 0.0
                                }
                            }
                        }

                        onTranslationChanged: {
                            if (!active) return
                            // Use explicit scene-position mapping to avoid any coordinate
                            // ambiguity from contentRoot's scale transform.
                            var cur   = mediaDrag.centroid.scenePosition
                            var press = mediaDrag.centroid.scenePressPosition
                            var curScene   = contentRoot.mapFromItem(null, cur.x,   cur.y)
                            var pressScene = contentRoot.mapFromItem(null, press.x, press.y)
                            mediaDelegate.localX = startX + (curScene.x - pressScene.x)
                            mediaDelegate.localY = startY + (curScene.y - pressScene.y)
                            // Chrome viewport offset = scene delta * viewScale
                            root.liveDragViewOffsetX = cur.x - press.x
                            root.liveDragViewOffsetY = cur.y - press.y
                        }
                    }

                    Loader {
                        property var media: mediaDelegate.media
                        sourceComponent: {
                            if (!mediaDelegate.media) return null
                            if (mediaDelegate.media.mediaType === "video") return videoDelegate
                            if (mediaDelegate.media.mediaType === "text") return textDelegate
                            return imageDelegate
                        }
                    }
                }
            }

            // Components receive mediaX:0/mediaY:0 and mediaScale:1 — the parent
            // mediaDelegate Item handles all position/scale/z transforms.
            Component {
                id: imageDelegate
                ImageItem {
                    mediaId: parent.media ? (parent.media.mediaId || "") : ""
                    mediaX: 0
                    mediaY: 0
                    mediaWidth: parent.media ? parent.media.width : 0
                    mediaHeight: parent.media ? parent.media.height : 0
                    mediaScale: 1.0
                    mediaZ: 0
                    selected: !!(parent.media && parent.media.selected)
                    imageSource: root.mediaSourceUrl(parent.media ? parent.media.sourcePath : "")
                    onSelectRequested: function(mediaId, additive) {
                        root.mediaSelectRequested(mediaId, additive)
                    }
                }
            }

            Component {
                id: videoDelegate
                VideoItem {
                    mediaId: parent.media ? (parent.media.mediaId || "") : ""
                    mediaX: 0
                    mediaY: 0
                    mediaWidth: parent.media ? parent.media.width : 0
                    mediaHeight: parent.media ? parent.media.height : 0
                    mediaScale: 1.0
                    mediaZ: 0
                    selected: !!(parent.media && parent.media.selected)
                    source: root.mediaSourceUrl(parent.media ? parent.media.sourcePath : "")
                    onSelectRequested: function(mediaId, additive) {
                        root.mediaSelectRequested(mediaId, additive)
                    }
                }
            }

            Component {
                id: textDelegate
                TextItem {
                    mediaId: parent.media ? (parent.media.mediaId || "") : ""
                    mediaX: 0
                    mediaY: 0
                    mediaWidth: parent.media ? parent.media.width : 0
                    mediaHeight: parent.media ? parent.media.height : 0
                    mediaScale: 1.0
                    mediaZ: 0
                    selected: !!(parent.media && parent.media.selected)
                    textContent: parent.media ? (parent.media.textContent || "") : ""
                    horizontalAlignment: parent.media ? (parent.media.textHorizontalAlignment || "center") : "center"
                    verticalAlignment: parent.media ? (parent.media.textVerticalAlignment || "center") : "center"
                    fitToTextEnabled: !!(parent.media && parent.media.fitToTextEnabled)
                    fontFamily: parent.media ? (parent.media.textFontFamily || "Arial") : "Arial"
                    fontPixelSize: Math.max(1, parent.media ? (parent.media.textFontPixelSize || 22) : 22)
                    fontWeight: parent.media ? (parent.media.textFontWeight || 400) : 400
                    fontItalic: !!(parent.media && parent.media.textItalic)
                    fontUnderline: !!(parent.media && parent.media.textUnderline)
                    fontUppercase: !!(parent.media && parent.media.textUppercase)
                    textColor: parent.media ? (parent.media.textColor || "#FFFFFFFF") : "#FFFFFFFF"
                    outlineWidthPercent: parent.media ? (parent.media.textOutlineWidthPercent || 0.0) : 0.0
                    outlineColor: parent.media ? (parent.media.textOutlineColor || "#FF000000") : "#FF000000"
                    highlightEnabled: !!(parent.media && parent.media.textHighlightEnabled)
                    highlightColor: parent.media ? (parent.media.textHighlightColor || "#00000000") : "#00000000"
                    textEditable: !!(parent.media && parent.media.textEditable)
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

        SnapGuides {
            id: snapGuides
            anchors.fill: parent
            contentItem: contentRoot
            viewportItem: viewport
            guidesModel: root.snapGuidesModel
            z: 89000
        }

        SelectionChrome {
            id: selectionChrome
            anchors.fill: parent
            contentItem: contentRoot
            viewportItem: viewport
            selectionModel: root.selectionChromeModel
            // Live drag offset: chrome visually follows the moving item without model repush
            draggedMediaId: root.liveDragMediaId
            dragOffsetViewX: root.liveDragViewOffsetX
            dragOffsetViewY: root.liveDragViewOffsetY
            z: 90000
            onResizeRequested: function(mediaId, handleId, sceneX, sceneY, snap) {
                root.mediaResizeRequested(mediaId, handleId, sceneX, sceneY, snap)
            }
            onResizeEnded: function(mediaId) {
                root.mediaResizeEnded(mediaId)
            }
        }

        DragHandler {
            id: panDrag
            enabled: !selectionChrome.interacting && root.liveDragMediaId === ""
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
