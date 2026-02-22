import QtQuick 2.15
import QtQuick.Shapes 1.15

// Renders the selection border and resize handles as a viewport-space overlay.
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
    property int handleHoverCount: 0
    readonly property bool pointerOnHandle: handleHoverCount > 0
    readonly property bool handlePriorityActive: interacting || pointerOnHandle

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

    Repeater {
        model: root.selectionModel

        delegate: Item {
            id: chrome
            property var entry: modelData
            readonly property var liveMedia: root.mediaEntryById(entry ? (entry.mediaId || "") : "")
            readonly property bool usesLiveResize: !!root.interactionController
                                                   && !!entry
                                                   && !!root.interactionController.liveResizeActive
                                                   && (root.interactionController.liveResizeMediaId || "") === (entry.mediaId || "")
            readonly property real sceneX: usesLiveResize
                                         ? (root.interactionController.liveResizeX || 0)
                                         : (liveMedia ? (liveMedia.x || 0) : (entry ? (entry.x || 0) : 0))
            readonly property real sceneY: usesLiveResize
                                         ? (root.interactionController.liveResizeY || 0)
                                         : (liveMedia ? (liveMedia.y || 0) : (entry ? (entry.y || 0) : 0))
            readonly property real sceneW: usesLiveResize
                                         ? Math.max(1, (liveMedia ? (liveMedia.width || 1) : (entry ? (entry.width || 1) : 1))
                                                        * (root.interactionController.liveResizeScale || 1.0))
                                         : (liveMedia
                                             ? Math.max(1, (liveMedia.width || 1) * (liveMedia.scale || 1.0))
                                             : Math.max(1, entry ? (entry.width || 1) : 1))
            readonly property real sceneH: usesLiveResize
                                         ? Math.max(1, (liveMedia ? (liveMedia.height || 1) : (entry ? (entry.height || 1) : 1))
                                                        * (root.interactionController.liveResizeScale || 1.0))
                                         : (liveMedia
                                             ? Math.max(1, (liveMedia.height || 1) * (liveMedia.scale || 1.0))
                                             : Math.max(1, entry ? (entry.height || 1) : 1))
            readonly property real _viewScale: root.contentItem ? root.contentItem.scale : 1.0
            readonly property real _contentX: root.contentItem ? root.contentItem.x : 0.0
            readonly property real _contentY: root.contentItem ? root.contentItem.y : 0.0
            readonly property bool beingDragged: !!entry && root.draggedMediaId !== "" && root.draggedMediaId === entry.mediaId

            readonly property point p1: {
                var _vs = _viewScale
                var _cx = _contentX
                var _cy = _contentY
                if (!root.contentItem || !root.viewportItem || !entry)
                    return Qt.point(0, 0)
                return Qt.point(_cx + sceneX * _vs, _cy + sceneY * _vs)
            }

            readonly property point p2: {
                var _vs = _viewScale
                var _cx = _contentX
                var _cy = _contentY
                if (!root.contentItem || !root.viewportItem || !entry)
                    return Qt.point(1, 1)
                return Qt.point(_cx + (sceneX + sceneW) * _vs,
                                _cy + (sceneY + sceneH) * _vs)
            }

            enabled: !!entry
            visible: !!entry
            z: 0

            x: Math.round(Math.min(p1.x, p2.x)) + (beingDragged ? root.dragOffsetViewX : 0)
            y: Math.round(Math.min(p1.y, p2.y)) + (beingDragged ? root.dragOffsetViewY : 0)
            width: Math.max(1, Math.round(Math.abs(p2.x - p1.x)))
            height: Math.max(1, Math.round(Math.abs(p2.y - p1.y)))

            Shape {
                anchors.fill: parent
                antialiasing: false
                preferredRendererType: Shape.GeometryRenderer

                ShapePath {
                    strokeWidth: 1
                    strokeColor: "#FFFFFF"
                    fillColor: "transparent"
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [4, 4]
                    dashOffset: 0
                    joinStyle: ShapePath.MiterJoin
                    capStyle: ShapePath.FlatCap
                    startX: 0.5
                    startY: 0.5
                    PathLine { x: Math.max(0.5, chrome.width - 0.5); y: 0.5 }
                    PathLine { x: Math.max(0.5, chrome.width - 0.5); y: Math.max(0.5, chrome.height - 0.5) }
                    PathLine { x: 0.5; y: Math.max(0.5, chrome.height - 0.5) }
                    PathLine { x: 0.5; y: 0.5 }
                }

                ShapePath {
                    strokeWidth: 1
                    strokeColor: "#4A90E2"
                    fillColor: "transparent"
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [4, 4]
                    dashOffset: 4
                    joinStyle: ShapePath.MiterJoin
                    capStyle: ShapePath.FlatCap
                    startX: 0.5
                    startY: 0.5
                    PathLine { x: Math.max(0.5, chrome.width - 0.5); y: 0.5 }
                    PathLine { x: Math.max(0.5, chrome.width - 0.5); y: Math.max(0.5, chrome.height - 0.5) }
                    PathLine { x: 0.5; y: Math.max(0.5, chrome.height - 0.5) }
                    PathLine { x: 0.5; y: 0.5 }
                }
            }

            Repeater {
                model: [
                    { ux: 0.0, uy: 0.0, handleId: "top-left" },
                    { ux: 0.5, uy: 0.0, handleId: "top-mid" },
                    { ux: 1.0, uy: 0.0, handleId: "top-right" },
                    { ux: 0.0, uy: 0.5, handleId: "left-mid" },
                    { ux: 1.0, uy: 0.5, handleId: "right-mid" },
                    { ux: 0.0, uy: 1.0, handleId: "bottom-left" },
                    { ux: 0.5, uy: 1.0, handleId: "bottom-mid" },
                    { ux: 1.0, uy: 1.0, handleId: "bottom-right" }
                ]

                delegate: Item {
                    width: Math.max(root.handleSize, root.handleHitboxSize)
                    height: Math.max(root.handleSize, root.handleHitboxSize)
                    enabled: true

                    x: Math.round((modelData.ux * chrome.width) - width * 0.5)
                    y: Math.round((modelData.uy * chrome.height) - height * 0.5)

                    Rectangle {
                        width: root.handleSize
                        height: root.handleSize
                        radius: 1
                        color: "#FFFFFF"
                        border.width: 1
                        border.color: "#4A90E2"
                        antialiasing: false
                        x: Math.round((parent.width - width) * 0.5)
                        y: Math.round((parent.height - height) * 0.5)
                    }

                    HoverHandler {
                        id: handleHover
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onHoveredChanged: {
                            if (hovered) {
                                root.handleHoverCount += 1
                            } else {
                                root.handleHoverCount = Math.max(0, root.handleHoverCount - 1)
                            }
                        }
                    }

                    Component.onDestruction: {
                        if (handleHover.hovered)
                            root.handleHoverCount = Math.max(0, root.handleHoverCount - 1)
                    }

                    DragHandler {
                        id: resizeDrag
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        acceptedButtons: Qt.LeftButton
                        // Always enabled while an entry exists (or while already active).
                        // Complex canStartInteraction() checks here caused a grab-competition
                        // regression: after a move the Repeater rebuilds fresh handlers while
                        // interactionMode is still "move", so canStartInteraction() returned
                        // false and the handler was disabled at creation time.  Combined with
                        // TakeOverForbidden this prevented resizeDrag from ever claiming the
                        // exclusive grab against mediaDrag's passive grab.
                        // Mutual exclusion is enforced correctly by interactionMode: once
                        // resizeDrag goes active it calls beginInteraction("resize"), which
                        // sets interactionMode = "resize", making mediaDrag.enabled → false
                        // (isInteractionIdle() returns false) so mediaDrag releases its passive
                        // grab automatically.  No TakeOverForbidden needed.
                        enabled: resizeDrag.active || !!entry
                        dragThreshold: 0

                        // Snapshot of entry geometry captured at drag-start.
                        // Using live entry.x/y/width/height is unsafe: if the model is ever
                        // repushed mid-drag the Repeater would rebuild and destroy this
                        // DragHandler.  Snapshotting ensures stable baseline values for the
                        // entire gesture even if entry updates.
                        property real pressEntryX: 0
                        property real pressEntryY: 0
                        property real pressEntryW: 0
                        property real pressEntryH: 0
                        property string activeResizeMediaId: ""
                        property string activeResizeHandleId: ""

                        onActiveChanged: {
                            if (active) {
                                activeResizeMediaId = entry ? (entry.mediaId || "") : ""
                                activeResizeHandleId = modelData ? (modelData.handleId || "") : ""
                                if (root.interactionController
                                        && !root.interactionController.beginInteraction("resize", activeResizeMediaId)) {
                                    activeResizeMediaId = ""
                                    activeResizeHandleId = ""
                                    root.interacting = false
                                    return
                                }
                                root.interacting = true
                                // Capture entry geometry at press time.
                                pressEntryX = entry ? entry.x     : 0
                                pressEntryY = entry ? entry.y     : 0
                                pressEntryW = entry ? entry.width  : 0
                                pressEntryH = entry ? entry.height : 0
                            } else {
                                root.interacting = false
                                if (root.interactionController) {
                                    root.interactionController.endInteraction("resize", activeResizeMediaId)
                                }
                                root.resizeEnded(activeResizeMediaId)
                                activeResizeMediaId = ""
                                activeResizeHandleId = ""
                            }
                        }
                        onCanceled: {
                            root.interacting = false
                            if (root.interactionController) {
                                root.interactionController.endInteraction("resize", activeResizeMediaId)
                            }
                            root.resizeEnded(activeResizeMediaId)
                            activeResizeMediaId = ""
                            activeResizeHandleId = ""
                        }
                        onTranslationChanged: {
                            if (!active || !root.interacting || !root.contentItem || !root.viewportItem)
                                return
                            var cur   = resizeDrag.centroid.scenePosition
                            var press = resizeDrag.centroid.scenePressPosition
                            var curScene   = root.contentItem.mapFromItem(null, cur.x,   cur.y)
                            var pressScene = root.contentItem.mapFromItem(null, press.x, press.y)
                            // Base the handle origin on the press-time snapshot, not live entry,
                            // so the math stays consistent throughout the whole drag gesture.
                            var centerSceneX = (resizeDrag.pressEntryX + modelData.ux * resizeDrag.pressEntryW) + (curScene.x - pressScene.x)
                            var centerSceneY = (resizeDrag.pressEntryY + modelData.uy * resizeDrag.pressEntryH) + (curScene.y - pressScene.y)
                            var snapEnabled = (Qt.application.keyboardModifiers & Qt.ShiftModifier) !== 0
                            root.resizeRequested(activeResizeMediaId,
                                                activeResizeHandleId,
                                                centerSceneX,
                                                centerSceneY,
                                                snapEnabled)
                        }
                    }
                }
            }
        }
    }
}
