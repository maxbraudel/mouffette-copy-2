import QtQuick 2.15
import QtQuick.Window 2.15

Rectangle {
    id: root
    color: "#10131a"

    signal mediaSelectRequested(string mediaId, bool additive)
    signal mediaMoveStarted(string mediaId, real sceneX, real sceneY)
    signal mediaMoveUpdated(string mediaId, real sceneX, real sceneY)
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
    // Transient live-resize overlay state (avoids full model mutation per pointer tick)
    property bool liveResizeActive: false
    property string liveResizeMediaId: ""
    property real liveResizeX: 0.0
    property real liveResizeY: 0.0
    property real liveResizeScale: 1.0
    readonly property string interactionMode: interactionArbiter.mode
    readonly property string interactionOwnerId: interactionArbiter.ownerId

    InteractionArbiter {
        id: interactionArbiter
    }

    function isInteractionIdle() {
        return interactionArbiter.isIdle()
    }

    function canStartInteraction(mode, ownerId) {
        return interactionArbiter.canStart(mode, ownerId)
    }

    function beginInteraction(mode, ownerId) {
        return interactionArbiter.begin(mode, ownerId)
    }

    function endInteraction(mode, ownerId) {
        interactionArbiter.end(mode, ownerId)
    }

    function requestMediaSelection(mediaId, additive) {
        if (!mediaId || mediaId.length === 0)
            return
        root.mediaSelectRequested(mediaId, !!additive)
    }

    function canStartMediaMove(media, contentItem, dragActive, mediaId) {
        if (!media)
            return false
        if (!media.selected)
            return false
        if (media.textEditable)
            return false
        if (contentItem && contentItem.editing === true)
            return false
        if (root.textToolActive)
            return false
        if (selectionChrome.handlePriorityActive)
            return false
        return dragActive || root.canStartInteraction("move", mediaId)
    }

    function canStartCanvasPan(panActive) {
        return panActive || (root.isInteractionIdle()
            && !root.textToolActive
            && !selectionChrome.interacting
            && !selectionChrome.handlePriorityActive
            && root.liveDragMediaId === "")
    }

    function canStartTextToolTap() {
        return root.isInteractionIdle()
            && root.textToolActive
            && !selectionChrome.interacting
            && !selectionChrome.handlePriorityActive
            && root.liveDragMediaId === ""
    }

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

    function canProcessCameraWheel() {
        if (isInteractionIdle())
            return true
        return interactionMode === "pan"
            && (interactionOwnerId === "" || interactionOwnerId === "canvas")
    }

    function mediaSourceUrl(path) {
        if (!path || path.length === 0)
            return ""
        if (path.indexOf("file:") === 0 || path.indexOf("qrc:") === 0 || path.indexOf("http:") === 0 || path.indexOf("https:") === 0)
            return path
        return "file://" + path
    }

    function applyLiveResizeGeometry(mediaId, sceneX, sceneY, scale) {
        if (!mediaId)
            return false

        liveResizeActive = true
        liveResizeMediaId = mediaId
        liveResizeX = sceneX
        liveResizeY = sceneY
        liveResizeScale = scale
        return true
    }

    function commitMediaGeometry(mediaId, sceneX, sceneY, scale) {
        if (!mediaId || !mediaModel || mediaModel.length === 0)
            return false

        for (var index = 0; index < mediaModel.length; ++index) {
            var entry = mediaModel[index]
            if (!entry || entry.mediaId !== mediaId)
                continue

            var updated = ({})
            for (var key in entry)
                updated[key] = entry[key]

            updated.x = sceneX
            updated.y = sceneY
            updated.scale = scale

            var next = mediaModel.slice(0)
            next[index] = updated
            mediaModel = next
            return true
        }

        return false
    }

    function beginLiveResize(mediaId) {
        if (!mediaId)
            return false
        liveResizeActive = true
        liveResizeMediaId = mediaId
        return true
    }

    function endLiveResize(mediaId, sceneX, sceneY, scale) {
        if (!mediaId)
            return false

        var committed = commitMediaGeometry(mediaId, sceneX, sceneY, scale)
        liveResizeActive = false
        liveResizeMediaId = ""
        liveResizeX = 0.0
        liveResizeY = 0.0
        liveResizeScale = 1.0
        return committed
    }

    function commitMediaTransform(mediaId, sceneX, sceneY, scale) {
        return commitMediaGeometry(mediaId, sceneX, sceneY, scale)
    }

    CanvasViewport {
        id: viewport
        anchors.fill: parent
        panX: root.panX
        panY: root.panY
        viewScale: root.viewScale

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

            MediaLayer {
                id: mediaLayer
                anchors.fill: parent
                mediaModel: root.mediaModel
                contentItem: viewport.contentRootItem
                sourceUrlResolver: root.mediaSourceUrl
                onMediaSelectRequested: function(mediaId, additive) {
                    root.mediaSelectRequested(mediaId, additive)
                }
                onMediaMoveEnded: function(mediaId, sceneX, sceneY) {
                    root.mediaMoveEnded(mediaId, sceneX, sceneY)
                }
                onTextCommitRequested: function(mediaId, text) {
                    root.textCommitRequested(mediaId, text)
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

                    width: media ? Math.max(1, media.width) : 1
                    height: media ? Math.max(1, media.height) : 1
                    readonly property bool usesLiveResize: root.liveResizeActive
                                                         && root.liveResizeMediaId === currentMediaId
                                                         && !localDragging
                    x: usesLiveResize ? root.liveResizeX : localX
                    y: usesLiveResize ? root.liveResizeY : localY
                    scale: usesLiveResize ? root.liveResizeScale : localScale
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
                        enabled: root.canStartMediaMove(mediaDelegate.media,
                                                        mediaContentLoader.item,
                                                        mediaDrag.active,
                                                        mediaDelegate.currentMediaId)
                        dragThreshold: 4

                        property real startX: 0.0
                        property real startY: 0.0
                        property real dragOriginSceneX: 0.0
                        property real dragOriginSceneY: 0.0
                        property real dragOriginViewX: 0.0
                        property real dragOriginViewY: 0.0
                        property string activeMoveMediaId: ""

                        onActiveChanged: {
                            if (active) {
                                activeMoveMediaId = mediaDelegate.currentMediaId
                                if (!root.beginInteraction("move", activeMoveMediaId)) {
                                    activeMoveMediaId = ""
                                    mediaDelegate.localDragging = false
                                    root.liveDragMediaId = ""
                                    root.liveDragViewOffsetX = 0.0
                                    root.liveDragViewOffsetY = 0.0
                                    return
                                }
                                startX = mediaDelegate.localX
                                startY = mediaDelegate.localY
                                var startPoint = mediaDrag.centroid.scenePosition
                                var startScene = viewport.contentRootItem.mapFromItem(null, startPoint.x, startPoint.y)
                                dragOriginSceneX = startScene.x
                                dragOriginSceneY = startScene.y
                                dragOriginViewX = startPoint.x
                                dragOriginViewY = startPoint.y
                                mediaDelegate.localDragging = true
                                root.liveDragMediaId = activeMoveMediaId
                                root.mediaMoveStarted(activeMoveMediaId,
                                                      mediaDelegate.localX,
                                                      mediaDelegate.localY)
                            } else {
                                var finalMediaId = activeMoveMediaId
                                mediaDelegate.localDragging = false
                                dragOriginSceneX = 0.0
                                dragOriginSceneY = 0.0
                                dragOriginViewX = 0.0
                                dragOriginViewY = 0.0
                                root.liveDragMediaId = ""
                                root.liveDragViewOffsetX = 0.0
                                root.liveDragViewOffsetY = 0.0
                                root.endInteraction("move", finalMediaId)
                                if (finalMediaId !== "") {
                                    root.mediaMoveEnded(finalMediaId,
                                                        mediaDelegate.localX,
                                                        mediaDelegate.localY)
                                }
                                activeMoveMediaId = ""
                            }
                        }

                        onTranslationChanged: {
                            if (!active || !mediaDelegate.localDragging) return
                            // Use explicit scene-position mapping to avoid any coordinate
                            // ambiguity from contentRoot's scale transform.
                            var cur = mediaDrag.centroid.scenePosition
                            var curScene = viewport.contentRootItem.mapFromItem(null, cur.x, cur.y)
                            mediaDelegate.localX = startX + (curScene.x - dragOriginSceneX)
                            mediaDelegate.localY = startY + (curScene.y - dragOriginSceneY)
                            if (activeMoveMediaId !== "") {
                                root.mediaMoveUpdated(activeMoveMediaId,
                                                      mediaDelegate.localX,
                                                      mediaDelegate.localY)
                            }
                            // Chrome viewport offset = scene delta * viewScale
                            root.liveDragViewOffsetX = cur.x - dragOriginViewX
                            root.liveDragViewOffsetY = cur.y - dragOriginViewY
                        }

                        onCanceled: {
                            mediaDelegate.localDragging = false
                            dragOriginSceneX = 0.0
                            dragOriginSceneY = 0.0
                            dragOriginViewX = 0.0
                            dragOriginViewY = 0.0
                            root.liveDragMediaId = ""
                            root.liveDragViewOffsetX = 0.0
                            root.liveDragViewOffsetY = 0.0
                            root.endInteraction("move", activeMoveMediaId)
                            activeMoveMediaId = ""
                        }
                    }

                    Loader {
                        id: mediaContentLoader
                        property var media: mediaDelegate.media
                        sourceComponent: {
                            if (!mediaDelegate.media) return null
                            if (mediaDelegate.media.mediaType === "video") return videoDelegate
                            if (mediaDelegate.media.mediaType === "text") return textDelegate
                            return imageDelegate
                        }
                    }

                    // Connections lives inside mediaDelegate so both `mediaDelegate` and
                    // `root` ids are in scope — Components defined outside the Repeater
                    // cannot access the delegate's id directly.
                    Connections {
                        target: mediaContentLoader.item
                        ignoreUnknownSignals: true
                        function onSelectRequested(mediaId, additive) {
                            root.requestMediaSelection(mediaId, additive)
                        }
                        function onTextCommitRequested(mediaId, text) {
                            root.textCommitRequested(mediaId, text)
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
                        mediaZ: parent.media.z
                        selected: !!parent.media.selected
                        imageSource: root.mediaSourceUrl(parent.media ? parent.media.sourcePath : "")
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
                        mediaZ: parent.media.z
                        selected: !!parent.media.selected
                        source: root.mediaSourceUrl(parent.media ? parent.media.sourcePath : "")
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
                        mediaZ: parent.media.z
                        selected: !!parent.media.selected
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

        SelectionLayer {
            id: selectionLayer
            parent: viewport
            anchors.fill: parent
            contentItem: viewport.contentRootItem
            viewportItem: viewport
            interactionController: root
            mediaModel: root.mediaModel
            selectionModel: root.selectionChromeModel
            snapGuidesModel: root.snapGuidesModel
            draggedMediaId: root.liveDragMediaId
            dragOffsetViewX: root.liveDragViewOffsetX
            dragOffsetViewY: root.liveDragViewOffsetY
            onMediaResizeRequested: function(mediaId, handleId, sceneX, sceneY, snap) {
                root.mediaResizeRequested(mediaId, handleId, sceneX, sceneY, snap)
            }
            onMediaResizeEnded: function(mediaId) {
                root.mediaResizeEnded(mediaId)
            }

            SnapGuides {
                id: snapGuides
                anchors.fill: parent
                contentItem: viewport.contentRootItem
                viewportItem: viewport
                guidesModel: root.snapGuidesModel
                z: 89000
            }

            SelectionChrome {
                id: selectionChrome
                anchors.fill: parent
                contentItem: viewport.contentRootItem
                viewportItem: viewport
                interactionController: root
                mediaModel: root.mediaModel
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
        }

        InputLayer {
            id: inputLayer
            parent: viewport
            anchors.fill: parent
            interactionController: root
            textToolActive: root.textToolActive
            onTextCreateRequested: function(viewX, viewY) {
                root.textCreateRequested(viewX, viewY)
            }

            DragHandler {
                id: panDrag
                // Background pan must never preempt media press-selection.
                enabled: root.canStartCanvasPan(panDrag.active)
                target: null
                acceptedDevices: PointerDevice.Mouse
                acceptedButtons: Qt.LeftButton
                dragThreshold: 16
                grabPermissions: PointerHandler.TakeOverForbidden
                property real startPanX: 0.0
                property real startPanY: 0.0
                property bool panSessionActive: false

                onActiveChanged: {
                    if (active) {
                        if (!root.beginInteraction("pan", "canvas")) {
                            panSessionActive = false
                            return
                        }
                        panSessionActive = true
                        startPanX = root.panX
                        startPanY = root.panY
                    } else {
                        panSessionActive = false
                        root.endInteraction("pan", "canvas")
                    }
                }

                onTranslationChanged: {
                    if (!panSessionActive)
                        return
                    root.panX = startPanX + translation.x
                    root.panY = startPanY + translation.y
                }

                onCanceled: {
                    panSessionActive = false
                    root.endInteraction("pan", "canvas")
                }
            }

            PinchHandler {
                id: pinchZoom
                target: null
                enabled: root.isInteractionIdle()
                property real lastScale: 1.0

                onActiveChanged: {
                    if (active) {
                        lastScale = 1.0
                    }
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
                // Text-create tap is only active in text tool mode.
                enabled: root.canStartTextToolTap()
                acceptedButtons: Qt.LeftButton

                onTapped: function(eventPoint) {
                    root.textCreateRequested(eventPoint.position.x, eventPoint.position.y)
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                hoverEnabled: false
                scrollGestureEnabled: true
                preventStealing: false

                onWheel: function(event) {
                    if (!root.canProcessCameraWheel()) {
                        event.accepted = true
                        return
                    }

                    if (root.isZoomModifier(event.modifiers)) {
                        var dy = root.wheelDeltaY(event)
                        if (dy !== 0.0) {
                            var factor = Math.pow(root.wheelZoomBase, dy)
                            root.applyZoomAt(event.x, event.y, factor)
                        }
                        event.accepted = true
                        return
                    }

                    var dx = root.wheelDeltaX(event)
                    var dyPan = root.wheelDeltaY(event)
                    if (dx !== 0.0 || dyPan !== 0.0) {
                        root.panX += dx
                        root.panY += dyPan
                    }

                    event.accepted = true
                }
            }
        }
    }
}
