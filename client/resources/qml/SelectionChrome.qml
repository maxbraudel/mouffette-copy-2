import QtQuick 2.15

// Renders the selection border and resize handles as a scene-space overlay.
// Move drag is handled natively by each media item's DragHandler in contentRoot.
// This component only handles resize (8 handles) and provides visual chrome.
Item {
    id: root

    property var interactionController: null
    property var selectionModel: []
    property var mediaModel: []
    property var mediaIndexById: ({})
    property Item contentItem: null
    property Item viewportItem: null
    property int handleSize: 10
    property int handleHitboxSize: 24
    property bool interacting: false
    property var handleDefs: [
        { ux: 0.0, uy: 0.0, handleId: "top-left" },
        { ux: 0.5, uy: 0.0, handleId: "top-mid" },
        { ux: 1.0, uy: 0.0, handleId: "top-right" },
        { ux: 0.0, uy: 0.5, handleId: "left-mid" },
        { ux: 1.0, uy: 0.5, handleId: "right-mid" },
        { ux: 0.0, uy: 1.0, handleId: "bottom-left" },
        { ux: 0.5, uy: 1.0, handleId: "bottom-mid" },
        { ux: 1.0, uy: 1.0, handleId: "bottom-right" }
    ]
    property string hoveredMediaId: ""
    property string hoveredHandleId: ""
    property string activeResizeMediaId: ""
    property string activeResizeHandleId: ""
    property real pressEntryX: 0.0
    property real pressEntryY: 0.0
    property real pressEntryW: 0.0
    property real pressEntryH: 0.0
    readonly property bool handlePriorityActive: interacting || pointerOnHandle
    readonly property bool pointerOnHandle: hoveredHandleId !== ""

    // Live drag offset injected from CanvasRoot — chrome follows content without model repush
    property string draggedMediaId: ""
    property real dragOffsetViewX: 0.0
    property real dragOffsetViewY: 0.0

    signal resizeRequested(string mediaId, string handleId, real sceneX, real sceneY, bool snap)
    signal resizeEnded(string mediaId)

    onMediaModelChanged: {
        var index = ({})
        if (mediaModel) {
            for (var i = 0; i < mediaModel.length; ++i) {
                var candidate = mediaModel[i]
                if (candidate && candidate.mediaId)
                    index[candidate.mediaId] = candidate
            }
        }
        mediaIndexById = index
    }

    function mediaEntryById(mediaId) {
        if (!mediaId)
            return null
        if (mediaIndexById && mediaIndexById[mediaId])
            return mediaIndexById[mediaId]
        return null
    }

    function resolveEntryGeometry(entry) {
        var mediaId = entry ? (entry.mediaId || "") : ""
        var liveMedia = mediaEntryById(mediaId)
        var usesLiveResize = !!interactionController
                          && !!entry
                          && !!interactionController.liveResizeActive
                          && (interactionController.liveResizeMediaId || "") === mediaId
        var sceneX = usesLiveResize
                   ? (interactionController.liveResizeX || 0)
                   : (liveMedia ? (liveMedia.x || 0) : (entry ? (entry.x || 0) : 0))
        var sceneY = usesLiveResize
                   ? (interactionController.liveResizeY || 0)
                   : (liveMedia ? (liveMedia.y || 0) : (entry ? (entry.y || 0) : 0))
        var sceneW = usesLiveResize
                   ? Math.max(1, (liveMedia ? (liveMedia.width || 1) : (entry ? (entry.width || 1) : 1))
                                  * (interactionController.liveResizeScale || 1.0))
                   : (liveMedia
                       ? Math.max(1, (liveMedia.width || 1) * (liveMedia.scale || 1.0))
                       : Math.max(1, entry ? (entry.width || 1) : 1))
        var sceneH = usesLiveResize
                   ? Math.max(1, (liveMedia ? (liveMedia.height || 1) : (entry ? (entry.height || 1) : 1))
                                  * (interactionController.liveResizeScale || 1.0))
                   : (liveMedia
                       ? Math.max(1, (liveMedia.height || 1) * (liveMedia.scale || 1.0))
                       : Math.max(1, entry ? (entry.height || 1) : 1))

        return {
            mediaId: mediaId,
            sceneX: sceneX,
            sceneY: sceneY,
            sceneW: sceneW,
            sceneH: sceneH
        }
    }

    function sceneRectToViewportRect(geometry) {
        var viewScale = contentItem ? contentItem.scale : 1.0
        var contentX = contentItem ? contentItem.x : 0.0
        var contentY = contentItem ? contentItem.y : 0.0
        var p1x = contentX + geometry.sceneX * viewScale
        var p1y = contentY + geometry.sceneY * viewScale
        var p2x = contentX + (geometry.sceneX + geometry.sceneW) * viewScale
        var p2y = contentY + (geometry.sceneY + geometry.sceneH) * viewScale
        var x = Math.round(Math.min(p1x, p2x))
        var y = Math.round(Math.min(p1y, p2y))
        var w = Math.max(1, Math.round(Math.abs(p2x - p1x)))
        var h = Math.max(1, Math.round(Math.abs(p2y - p1y)))
        return { x: x, y: y, width: w, height: h }
    }

    function hitTestHandle(viewX, viewY) {
        if (!selectionModel || !contentItem || !viewportItem)
            return null

        var radius = Math.max(handleSize, handleHitboxSize) * 0.5
        for (var entryIndex = selectionModel.length - 1; entryIndex >= 0; --entryIndex) {
            var entry = selectionModel[entryIndex]
            if (!entry)
                continue

            var geom = resolveEntryGeometry(entry)
            if (!geom.mediaId)
                continue

            var rect = sceneRectToViewportRect(geom)
            for (var i = 0; i < handleDefs.length; ++i) {
                var handleDef = handleDefs[i]
                var cx = rect.x + handleDef.ux * rect.width
                var cy = rect.y + handleDef.uy * rect.height
                if (Math.abs(viewX - cx) <= radius && Math.abs(viewY - cy) <= radius) {
                    return {
                        mediaId: geom.mediaId,
                        handleId: handleDef.handleId,
                        sceneX: geom.sceneX,
                        sceneY: geom.sceneY,
                        sceneW: geom.sceneW,
                        sceneH: geom.sceneH
                    }
                }
            }
        }

        return null
    }

    function updateHoveredHandle(viewX, viewY) {
        if (interacting)
            return
        if (interactionController && !interactionController.isInteractionIdle()) {
            hoveredMediaId = ""
            hoveredHandleId = ""
            return
        }
        var hit = hitTestHandle(viewX, viewY)
        if (hit) {
            hoveredMediaId = hit.mediaId
            hoveredHandleId = hit.handleId
        } else {
            hoveredMediaId = ""
            hoveredHandleId = ""
        }
    }

    function clearHoveredHandle() {
        if (interacting)
            return
        hoveredMediaId = ""
        hoveredHandleId = ""
    }

    HoverHandler {
        id: globalHover
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

        onHoveredChanged: {
            if (!hovered)
                root.clearHoveredHandle()
        }

        onPointChanged: {
            if (!point)
                return
            root.updateHoveredHandle(point.position.x, point.position.y)
        }
    }

    DragHandler {
        id: globalResizeDrag
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        enabled: globalResizeDrag.active
                 || (root.hoveredHandleId !== ""
                     && (!root.interactionController
                         || root.interactionController.isInteractionIdle()))
        dragThreshold: 0

        onActiveChanged: {
            if (active) {
                var pressPoint = globalResizeDrag.centroid.scenePressPosition
                var pressHit = root.hitTestHandle(pressPoint.x, pressPoint.y)
                if (!pressHit) {
                    root.interacting = false
                    root.activeResizeMediaId = ""
                    root.activeResizeHandleId = ""
                    return
                }

                root.activeResizeMediaId = pressHit.mediaId
                root.activeResizeHandleId = pressHit.handleId
                root.pressEntryX = pressHit.sceneX
                root.pressEntryY = pressHit.sceneY
                root.pressEntryW = pressHit.sceneW
                root.pressEntryH = pressHit.sceneH

                if (root.interactionController
                        && !root.interactionController.beginInteraction("resize", root.activeResizeMediaId)) {
                    root.interacting = false
                    root.activeResizeMediaId = ""
                    root.activeResizeHandleId = ""
                    return
                }

                root.interacting = true
                root.hoveredMediaId = root.activeResizeMediaId
                root.hoveredHandleId = root.activeResizeHandleId
            } else {
                var finalMediaId = root.activeResizeMediaId
                root.interacting = false
                if (root.interactionController) {
                    root.interactionController.endInteraction("resize", finalMediaId)
                }
                root.resizeEnded(finalMediaId)
                root.activeResizeMediaId = ""
                root.activeResizeHandleId = ""
            }
        }

        onCanceled: {
            var canceledMediaId = root.activeResizeMediaId
            root.interacting = false
            if (root.interactionController) {
                root.interactionController.endInteraction("resize", canceledMediaId)
            }
            root.resizeEnded(canceledMediaId)
            root.activeResizeMediaId = ""
            root.activeResizeHandleId = ""
        }

        onTranslationChanged: {
            if (!active || !root.interacting || !root.contentItem || !root.viewportItem)
                return

            var cur = globalResizeDrag.centroid.scenePosition
            var press = globalResizeDrag.centroid.scenePressPosition
            var curScene = root.contentItem.mapFromItem(null, cur.x, cur.y)
            var pressScene = root.contentItem.mapFromItem(null, press.x, press.y)

            var ux = 0.0
            var uy = 0.0
            for (var i = 0; i < root.handleDefs.length; ++i) {
                if (root.handleDefs[i].handleId === root.activeResizeHandleId) {
                    ux = root.handleDefs[i].ux
                    uy = root.handleDefs[i].uy
                    break
                }
            }

            var centerSceneX = (root.pressEntryX + ux * root.pressEntryW) + (curScene.x - pressScene.x)
            var centerSceneY = (root.pressEntryY + uy * root.pressEntryH) + (curScene.y - pressScene.y)
            var snapEnabled = (Qt.application.keyboardModifiers & Qt.ShiftModifier) !== 0
            root.resizeRequested(root.activeResizeMediaId,
                                 root.activeResizeHandleId,
                                 centerSceneX,
                                 centerSceneY,
                                 snapEnabled)
        }
    }

    Repeater {
        model: root.selectionModel

        delegate: Item {
            id: chrome
            parent: root.contentItem ? root.contentItem : root
            property var entry: modelData
            readonly property var geometry: root.resolveEntryGeometry(entry)
            readonly property real sceneX: geometry.sceneX
            readonly property real sceneY: geometry.sceneY
            readonly property real sceneW: geometry.sceneW
            readonly property real sceneH: geometry.sceneH
            readonly property real _viewScale: root.contentItem ? root.contentItem.scale : 1.0
            readonly property bool beingDragged: !!entry && root.draggedMediaId !== "" && root.draggedMediaId === entry.mediaId

            enabled: !!entry
            visible: !!entry
            z: 90000

            x: sceneX + (beingDragged ? (root.dragOffsetViewX / _viewScale) : 0)
            y: sceneY + (beingDragged ? (root.dragOffsetViewY / _viewScale) : 0)
            width: Math.max(1, sceneW)
            height: Math.max(1, sceneH)

            // Use 4 separate opaque strips rather than a single transparent-fill Rectangle.
            // A transparent-fill Rectangle with an opaque border can enter the opaque render
            // batch in Qt Quick's scene graph — writes depth for the entire item bounds at
            // chrome z=90000, causing the underlying Image (z≈0) to fail the depth test and
            // render as black at high zoom levels. Pure opaque strips carry no fill, so they
            // only occlude the exact edge pixels they cover.
            readonly property real _bw: 1.0 / chrome._viewScale

            // Top
            Rectangle { x: 0; y: 0;                              width: chrome.width;  height: chrome._bw; color: "#4A90E2"; antialiasing: false }
            // Bottom
            Rectangle { x: 0; y: chrome.height - chrome._bw;     width: chrome.width;  height: chrome._bw; color: "#4A90E2"; antialiasing: false }
            // Left
            Rectangle { x: 0; y: chrome._bw;                     width: chrome._bw;    height: Math.max(0, chrome.height - 2 * chrome._bw); color: "#4A90E2"; antialiasing: false }
            // Right
            Rectangle { x: chrome.width - chrome._bw; y: chrome._bw; width: chrome._bw; height: Math.max(0, chrome.height - 2 * chrome._bw); color: "#4A90E2"; antialiasing: false }

            Repeater {
                model: root.handleDefs

                delegate: Rectangle {
                    width: root.handleSize / chrome._viewScale
                    height: root.handleSize / chrome._viewScale
                    radius: 1.0 / chrome._viewScale
                    color: "#FFFFFF"
                    border.width: 1.0 / chrome._viewScale
                    border.color: "#4A90E2"
                    antialiasing: false
                    x: (modelData.ux * chrome.width) - width * 0.5
                    y: (modelData.uy * chrome.height) - height * 0.5
                }
            }
        }
    }
}
