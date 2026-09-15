import QtQuick
// Renders the selection border and resize handles as a scene-space overlay.
// Move drag is handled natively by each media item's DragHandler in contentRoot.
// This component only handles resize (8 handles) and provides visual chrome.
Item {
    id: root

    property var interactionController: null
    readonly property bool editingEnabled: !interactionController || interactionController.editingEnabled

    property var inputCoordinator: null
    property var selectionModel: []
    property var mediaModel: []
    property var mediaIndexById: ({})
    property Item contentItem: null
    property Item viewportItem: null
    property int handleSize: 10
    property int handleHitboxSize: 24
    property bool interacting: false
    // `interacting` is application state; DragHandler.active is the native
    // pointer lease. CanvasRoot compares both so a lost ungrab cannot leave
    // the whole canvas permanently blocked after a resize.
    readonly property bool resizeHandlerActive: globalResizeDrag.active
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
    // Hover is derived feedback, not a second owner store. Re-evaluate when
    // selection, live geometry or the camera changes under a stationary cursor.
    readonly property var hoveredHandleHit: !interacting && globalHover.hovered
        && inputCoordinator && inputCoordinator.isIdle()
        ? hitTestHandle(globalHover.point.position.x, globalHover.point.position.y)
        : null
    readonly property string hoveredMediaId: interacting ? activeResizeMediaId
        : (hoveredHandleHit ? hoveredHandleHit.mediaId : "")
    readonly property string hoveredHandleId: interacting ? activeResizeHandleId
        : (hoveredHandleHit ? hoveredHandleHit.handleId : "")
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

    signal resizeRequested(string mediaId, string handleId, real sceneX, real sceneY, bool snap, bool altPressed)
    signal resizeEnded(string mediaId)

    function finishResizeSession(abandonPointer) {
        var mediaId = activeResizeMediaId
        // Release our state before synchronous backend/model callbacks. Both
        // native cancellation and release can arrive; only the first commits.
        activeResizeMediaId = ""
        activeResizeHandleId = ""
        interacting = false
        if (!mediaId)
            return
        if (inputCoordinator) {
            if (abandonPointer)
                inputCoordinator.releaseMediaOwnership(mediaId, "resize")
            else if (inputCoordinator.mode === "resize" && inputCoordinator.ownerId === mediaId)
                inputCoordinator.endResize(mediaId)
        }
        resizeEnded(mediaId)
    }

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
        var transform = interactionController && interactionController.liveTransforms
            ? interactionController.liveTransforms[mediaId] : null
        if (transform) {
            return { mediaId: mediaId, sceneX: transform.x, sceneY: transform.y,
                     sceneW: transform.width * transform.scale,
                     sceneH: transform.height * transform.scale }
        }
        var liveMedia = mediaEntryById(mediaId)
        var usesLiveResize = !!interactionController
                          && !!entry
                          && !!interactionController.liveResizeActive
                          && (interactionController.liveResizeMediaId || "") === mediaId
        var usesLiveAltResize = !!interactionController
                          && !!entry
                          && !!interactionController.liveAltResizeActive
                          && (interactionController.liveAltResizeMediaId || "") === mediaId
        var sceneX = usesLiveAltResize
                   ? (interactionController.liveAltResizeX || 0)
                   : (usesLiveResize
                       ? (interactionController.liveResizeX || 0)
                       : (liveMedia ? (liveMedia.x || 0) : (entry ? (entry.x || 0) : 0)))
        var sceneY = usesLiveAltResize
                   ? (interactionController.liveAltResizeY || 0)
                   : (usesLiveResize
                       ? (interactionController.liveResizeY || 0)
                       : (liveMedia ? (liveMedia.y || 0) : (entry ? (entry.y || 0) : 0)))
        var sceneW = usesLiveAltResize
                   ? Math.max(1, (interactionController.liveAltResizeWidth || 1)
                                 * (interactionController.liveAltResizeScale || 1.0))
                   : (usesLiveResize
                       ? Math.max(1, (liveMedia ? (liveMedia.width || 1) : (entry ? (entry.width || 1) : 1))
                                      * (interactionController.liveResizeScale || 1.0))
                       : (liveMedia
                           ? Math.max(1, (liveMedia.width || 1) * (liveMedia.scale || 1.0))
                           : Math.max(1, entry ? (entry.width || 1) : 1)))
        var sceneH = usesLiveAltResize
                   ? Math.max(1, (interactionController.liveAltResizeHeight || 1)
                                 * (interactionController.liveAltResizeScale || 1.0))
                   : (usesLiveResize
                       ? Math.max(1, (liveMedia ? (liveMedia.height || 1) : (entry ? (entry.height || 1) : 1))
                                      * (interactionController.liveResizeScale || 1.0))
                       : (liveMedia
                           ? Math.max(1, (liveMedia.height || 1) * (liveMedia.scale || 1.0))
                           : Math.max(1, entry ? (entry.height || 1) : 1)))

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
        if (!editingEnabled) return null
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
                // Keep the full hit target outside the item, while capping its
                // inward reach to one quarter of a small item. The eight
                // handles therefore never consume its central body: even a
                // tiny selected text remains draggable and editable.
                var innerRadiusX = Math.min(radius, rect.width * 0.25)
                var innerRadiusY = Math.min(radius, rect.height * 0.25)
                var leftRadius = handleDef.ux === 0.0 ? radius : innerRadiusX
                var rightRadius = handleDef.ux === 1.0 ? radius : innerRadiusX
                var topRadius = handleDef.uy === 0.0 ? radius : innerRadiusY
                var bottomRadius = handleDef.uy === 1.0 ? radius : innerRadiusY
                if (viewX >= cx - leftRadius && viewX <= cx + rightRadius
                        && viewY >= cy - topRadius && viewY <= cy + bottomRadius) {
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

    function resizeCursorForHandle(handleId) {
        switch (handleId) {
        case "top-left":
        case "bottom-right":
            return Qt.SizeFDiagCursor
        case "top-right":
        case "bottom-left":
            return Qt.SizeBDiagCursor
        case "top-mid":
        case "bottom-mid":
            return Qt.SizeVerCursor
        case "left-mid":
        case "right-mid":
            return Qt.SizeHorCursor
        default:
            return Qt.ArrowCursor
        }
    }

    readonly property string effectiveResizeHandleId: interacting
                                                  ? activeResizeHandleId
                                                  : hoveredHandleId
    readonly property int effectiveResizeCursorShape: resizeCursorForHandle(effectiveResizeHandleId)

    HoverHandler {
        id: globalHover
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        cursorShape: root.effectiveResizeCursorShape
    }

    Item {
        id: resizeInputSurface
        anchors.fill: parent
        // Qt evaluates contains() before granting a native grab. A remembered
        // hover must not enable a full-canvas drag handler after deselection.
        containmentMask: QtObject {
            function contains(p: point): bool {
                var hit = root.hitTestHandle(p.x, p.y)
                return hit !== null
                    && !!root.inputCoordinator
                    && root.inputCoordinator.canStartResize(
                        globalResizeDrag.active, hit.mediaId)
            }
        }
    }

    DragHandler {
        id: globalResizeDrag
        parent: resizeInputSurface
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        cursorShape: root.effectiveResizeCursorShape
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        // Keep observation stable for the whole native gesture. Binding
        // enabled to `active || coordinator.isIdle()` could disable the
        // handler on release while the coordinator was still in resize mode,
        // before onActiveChanged(false) had cleared that mode.
        enabled: root.editingEnabled && !!root.inputCoordinator
        dragThreshold: 0

        onActiveChanged: {
            if (active) {
                var pressPoint = globalResizeDrag.centroid.scenePressPosition
                var localPressPoint = root.mapFromItem(null, pressPoint.x, pressPoint.y)
                var pressHit = root.hitTestHandle(localPressPoint.x, localPressPoint.y)
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

                var resizeGranted = !!root.inputCoordinator
                    && root.inputCoordinator.tryBeginResize(root.activeResizeMediaId)

                if (!resizeGranted) {
                    root.interacting = false
                    root.activeResizeMediaId = ""
                    root.activeResizeHandleId = ""
                    return
                }

                root.interacting = true
            } else {
                root.finishResizeSession(false)
            }
        }

        onCanceled: {
            root.finishResizeSession(true)
        }

        onGrabChanged: function(transition, point) {
            if (transition === PointerDevice.CancelGrabExclusive)
                root.finishResizeSession(true)
            else if (transition === PointerDevice.UngrabExclusive)
                root.finishResizeSession(false)
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
            var mods = globalResizeDrag.centroid.modifiers
            var snapEnabled = (mods & Qt.ShiftModifier) !== 0
            var altEnabled  = (mods & Qt.AltModifier)   !== 0
            root.resizeRequested(root.activeResizeMediaId,
                                 root.activeResizeHandleId,
                                 centerSceneX,
                                 centerSceneY,
                                 snapEnabled,
                                 altEnabled)
        }

    }

    Repeater {
        // Repeater owns sibling stacking: reparent it, not its delegates.
        parent: root.contentItem ? root.contentItem : root
        model: root.selectionModel

        delegate: Item {
            id: chrome
            objectName: "selectionChromeVisual"
            property var entry: modelData
            readonly property var geometry: root.resolveEntryGeometry(entry)
            readonly property real sceneX: geometry.sceneX
            readonly property real sceneY: geometry.sceneY
            readonly property real sceneW: geometry.sceneW
            readonly property real sceneH: geometry.sceneH
            readonly property real _viewScale: root.contentItem ? root.contentItem.scale : 1.0
            readonly property bool hasLiveTransform: !!entry && !!interactionController
                && !!interactionController.liveTransforms && !!interactionController.liveTransforms[entry.mediaId]
            readonly property bool beingDragged: !hasLiveTransform && !!entry && root.draggedMediaId !== "" && root.draggedMediaId === entry.mediaId
            // snapDragActive does NOT require beingDragged.
            // liveDragMediaId (draggedMediaId) is cleared BEFORE mediaMoveEnded is signalled,
            // so beingDragged becomes false before the snap freeze is lifted. The freeze
            // (liveSnapDragActive) stays active until onMediaChanged fires and clears it.
            // During that window we must keep the chrome at the snapped position, which is
            // done by applying the snap offset independently of beingDragged.
            readonly property bool snapDragActive: !hasLiveTransform && !!interactionController
                                                   && !!interactionController.liveSnapDragActive
                                                   && interactionController.liveSnapDragMediaId === (entry ? entry.mediaId : "")
            // When snap is active: derive position offset from snapped scene position delta.
            // liveSnapDragX/Y and sceneX are both in canvas QML units (scene * sceneUnitScale).
            readonly property real effectiveDragOffsetX: snapDragActive
                ? (interactionController.liveSnapDragX - sceneX) * _viewScale
                : root.dragOffsetViewX
            readonly property real effectiveDragOffsetY: snapDragActive
                ? (interactionController.liveSnapDragY - sceneY) * _viewScale
                : root.dragOffsetViewY

            enabled: !!entry
            visible: !!entry
            // Delegates are deliberately reparented to contentRoot, so this z
            // is compared directly with mediaDropPreview.z (98000). Selection
            // borders and handles must remain the topmost scene-space visual
            // during the preview-to-media handoff.
            z: 98500

            // Apply offset when dragging OR when the snap freeze is active.
            // The snap freeze outlives the drag by design (cleared by onMediaChanged),
            // so gating on beingDragged alone would cause a 1-frame jump to the stale
            // model sceneX/Y before selectionChromeModel is updated by C++.
            x: sceneX + ((beingDragged || snapDragActive) ? (effectiveDragOffsetX / _viewScale) : 0)
            y: sceneY + ((beingDragged || snapDragActive) ? (effectiveDragOffsetY / _viewScale) : 0)
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
