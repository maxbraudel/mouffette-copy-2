import QtQuick 2.15

Item {
    id: inputLayer

    property bool useInputCoordinator: true
    property var interactionController: null
    property bool textToolActive: false
    property var mediaModel: []
    property Item contentItem: null
    property bool selectionHandlePriorityActive: false
    property string selectionHandleHoveredMediaId: ""
    property string liveDragMediaId: ""
    readonly property alias inputCoordinator: coordinator
    default property alias layerChildren: layerRoot.data

    signal textCreateRequested(real viewX, real viewY)

    QtObject {
        id: coordinator

        property string mode: "idle"
        property string ownerId: ""
        property string pressTargetKind: "unknown" // unknown | handle | media | canvas
        property string pressTargetMediaId: ""
        property string lastPrimaryPressKind: "unknown" // unknown | handle | media | canvas
        property string lastPrimaryPressMediaId: ""
        property bool primaryGestureActive: false
        property string primaryOwnerKind: "none" // none | handle | media | canvas
        property string primaryOwnerMediaId: ""

        function resetPrimaryOwner() {
            primaryGestureActive = false
            primaryOwnerKind = "none"
            primaryOwnerMediaId = ""
        }

        function resetPressTarget() {
            pressTargetKind = "unknown"
            pressTargetMediaId = ""
        }

        function resetLastPrimaryPressTarget() {
            lastPrimaryPressKind = "unknown"
            lastPrimaryPressMediaId = ""
        }

        function assertInvariants(stage) {
            if (mode === "idle" && ownerId !== "") {
                console.warn("[QuickCanvas][InputCoordinator][Invariant] idle mode with owner", stage, ownerId)
            }
            if (mode !== "idle" && mode !== "pan" && ownerId === "") {
                console.warn("[QuickCanvas][InputCoordinator][Invariant] non-canvas active mode without owner", stage, mode)
            }
            if (primaryGestureActive && primaryOwnerKind === "none") {
                console.warn("[QuickCanvas][InputCoordinator][Invariant] active gesture without owner kind", stage)
            }
            if (!primaryGestureActive && primaryOwnerKind !== "none") {
                console.warn("[QuickCanvas][InputCoordinator][Invariant] inactive gesture with owner kind", stage, primaryOwnerKind)
            }
        }

        function mediaIdAtPoint(viewX, viewY) {
            if (!inputLayer.contentItem)
                return ""

            var currentItem = inputLayer.contentItem
            var localPoint = currentItem.mapFromItem(inputLayer, viewX, viewY)
            var mediaId = ""

            while (currentItem) {
                var child = currentItem.childAt(localPoint.x, localPoint.y)
                if (!child)
                    break

                if (child.currentMediaId !== undefined && child.currentMediaId)
                    mediaId = child.currentMediaId
                else if (child.mediaId !== undefined && child.mediaId)
                    mediaId = child.mediaId

                localPoint = child.mapFromItem(currentItem, localPoint.x, localPoint.y)
                currentItem = child
            }

            return mediaId || ""
        }

        function beginPrimaryGesture(viewX, viewY, hoveredHandleId, hoveredHandleMediaId) {
            primaryGestureActive = true
            resetLastPrimaryPressTarget()

            var handleId = hoveredHandleId || ""
            console.log("[DBG][beginPrimaryGesture] viewX=", viewX, "viewY=", viewY, "handleId=", handleId)
            if (inputLayer.selectionHandlePriorityActive || handleId !== "") {
                primaryOwnerKind = "handle"
                primaryOwnerMediaId = hoveredHandleMediaId || ""
                pressTargetKind = "handle"
                pressTargetMediaId = primaryOwnerMediaId
                return primaryOwnerKind
            }

            var hitMediaId = mediaIdAtPoint(viewX, viewY)
            console.log("[DBG][beginPrimaryGesture] hitMediaId=", hitMediaId)
            if (hitMediaId !== "") {
                primaryOwnerKind = "media"
                primaryOwnerMediaId = hitMediaId
                pressTargetKind = "media"
                pressTargetMediaId = hitMediaId
                return primaryOwnerKind
            }

            primaryOwnerKind = "canvas"
            primaryOwnerMediaId = ""
            pressTargetKind = "canvas"
            pressTargetMediaId = ""
            assertInvariants("beginPrimaryGesture")
            return primaryOwnerKind
        }

        function endPrimaryGesture() {
            lastPrimaryPressKind = pressTargetKind
            lastPrimaryPressMediaId = pressTargetMediaId
            resetPrimaryOwner()
            resetPressTarget()
            assertInvariants("endPrimaryGesture")
        }

        function ownerAllowsMedia(mediaId, active) {
            if (active)
                return true
            if (!primaryGestureActive)
                return true
            if (primaryOwnerKind !== "media")
                return false
            return primaryOwnerMediaId === "" || primaryOwnerMediaId === (mediaId || "")
        }

        function ownerAllowsCanvasPan(active) {
            if (active)
                return true
            if (!primaryGestureActive)
                return true
            return primaryOwnerKind === "canvas"
        }

        function ownerAllowsEmptyTap() {
            if (primaryGestureActive)
                return primaryOwnerKind === "canvas"
            // Tap is evaluated on release, after primary gesture has ended.
            // Use the snapshot of the most recent press origin.
            return lastPrimaryPressKind === "canvas"
        }

        function isIdle() {
            return mode === "idle"
        }

        function canStart(requestedMode, requestedOwnerId) {
            var owner = requestedOwnerId || ""
            return mode === "idle"
                || (mode === requestedMode && ownerId === owner)
        }

        function beginMode(requestedMode, requestedOwnerId) {
            var owner = requestedOwnerId || ""
            if (!canStart(requestedMode, owner)) {
                console.warn("[QuickCanvas][InputCoordinator] denied begin",
                             "requestedMode=", requestedMode,
                             "requestedOwner=", owner,
                             "currentMode=", mode,
                             "currentOwner=", ownerId)
                return false
            }
            mode = requestedMode
            ownerId = owner
            assertInvariants("beginMode")
            return true
        }

        function endMode(expectedMode, expectedOwnerId) {
            var owner = expectedOwnerId || ""
            if (mode !== expectedMode) {
                console.warn("[QuickCanvas][InputCoordinator] ignored end due to mode mismatch",
                             "expectedMode=", expectedMode,
                             "currentMode=", mode,
                             "owner=", owner,
                             "currentOwner=", ownerId)
                return
            }
            if (ownerId !== "" && owner !== "" && ownerId !== owner) {
                console.warn("[QuickCanvas][InputCoordinator] ignored end due to owner mismatch",
                             "expectedMode=", expectedMode,
                             "expectedOwner=", owner,
                             "currentOwner=", ownerId)
                return
            }
            mode = "idle"
            ownerId = ""
            assertInvariants("endMode")
        }

        function forceReset(reason) {
            if (mode !== "idle" || ownerId !== "") {
                console.warn("[QuickCanvas][InputCoordinator] forced reset",
                             "reason=", reason || "unknown",
                             "mode=", mode,
                             "owner=", ownerId)
            }
            mode = "idle"
            ownerId = ""
            resetPressTarget()
            resetLastPrimaryPressTarget()
            assertInvariants("forceReset")
        }

        function isPointInsideMedia(viewX, viewY) {
            return mediaIdAtPoint(viewX, viewY) !== ""
        }

        function noteMediaPrimaryPress(mediaId, additive) {
            if (!mediaId)
                return false
            if (inputLayer.interactionController) {
                inputLayer.interactionController.requestMediaSelection(mediaId, !!additive)
            }
            return true
        }

        function describeMoveBlock(media, contentItem, dragActive, mediaId) {
            if (!media)
                return "no-media"
            if (contentItem && contentItem.editing === true)
                return "content-editing"
            if (inputLayer.textToolActive)
                return "text-tool-active"
            if (!ownerAllowsMedia(mediaId, dragActive))
                return "gesture-owner-not-media"
            // Block drag if a resize is actively in progress, OR if ANY selected item's handle
            // is hovered — this prevents occluding media items from stealing press events when
            // the pointer is over a selected item's resize handle zone (even if that item has
            // lower z-order than the item whose DragHandler would otherwise fire).
            if (inputLayer.selectionHandlePriorityActive)
                return "selection-handle-priority"
            var hoveredId = inputLayer.selectionHandleHoveredMediaId || ""
            if (hoveredId !== "")
                return "selection-handle-hovered"
            if (dragActive)
                return ""
            if (!canStart("move", mediaId))
                return "coordinator-busy"
            return ""
        }

        function canStartMove(media, contentItem, dragActive, mediaId) {
            return describeMoveBlock(media, contentItem, dragActive, mediaId) === ""
        }

        function tryBeginMove(mediaId) {
            if (!mediaId)
                return false
            return beginMode("move", mediaId)
        }

        function endMove(mediaId) {
            endMode("move", mediaId)
        }

        function canEnablePan(panActive) {
            return ownerAllowsCanvasPan(panActive)
                && (panActive || (isIdle()
                && !inputLayer.textToolActive
                && !inputLayer.selectionHandlePriorityActive
                && inputLayer.liveDragMediaId === ""))
        }

        function tryBeginPanAt(viewX, viewY) {
            if (!canEnablePan(false))
                return false
            if (isPointInsideMedia(viewX, viewY))
                return false
            return beginMode("pan", "canvas")
        }

        function endPan() {
            endMode("pan", "canvas")
        }

        function canStartResize(active, mediaId) {
            if (active)
                return true
            if (!isIdle())
                return false
            if (pressTargetKind === "handle") {
                return pressTargetMediaId === "" || pressTargetMediaId === mediaId
            }
            return true
        }

        function tryBeginResize(mediaId) {
            pressTargetKind = "handle"
            pressTargetMediaId = mediaId || ""
            return beginMode("resize", mediaId)
        }

        function endResize(mediaId) {
            endMode("resize", mediaId)
            resetPressTarget()
            resetLastPrimaryPressTarget()
        }

        function canStartTextToolTap() {
            return isIdle()
                && inputLayer.textToolActive
                && ownerAllowsCanvasPan(false)
                && !inputLayer.selectionHandlePriorityActive
                && inputLayer.liveDragMediaId === ""
        }

        function tryBeginTextCreateAt(viewX, viewY) {
            if (!canStartTextToolTap())
                return false
            if (isPointInsideMedia(viewX, viewY))
                return false
            if (!beginMode("text", "canvas"))
                return false
            inputLayer.textCreateRequested(viewX, viewY)
            endMode("text", "canvas")
            return true
        }
    }

    Item {
        id: layerRoot
        anchors.fill: parent
    }
}
