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
    property bool debugInput: true
    readonly property alias inputCoordinator: coordinator
    default property alias layerChildren: layerRoot.data

    signal textCreateRequested(real viewX, real viewY)

    QtObject {
        id: coordinator

        function logInput() {
            if (!inputLayer.debugInput)
                return
            var args = ["[QuickCanvas][InputDebug][Coordinator]"]
            for (var i = 0; i < arguments.length; ++i)
                args.push(arguments[i])
            console.warn.apply(console, args)
        }

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

            function resolveMediaIdFromItemHierarchy(item) {
                var node = item
                while (node) {
                    if (node.currentMediaId !== undefined && node.currentMediaId)
                        return node.currentMediaId
                    if (node.mediaId !== undefined && node.mediaId)
                        return node.mediaId
                    node = node.parent
                }
                return ""
            }

            function mediaIdFromModelBounds(sceneX, sceneY) {
                if (!inputLayer.mediaModel || inputLayer.mediaModel.length === 0)
                    return ""

                var bestMediaId = ""
                var bestZ = -Infinity
                var bestIndex = -1

                for (var i = 0; i < inputLayer.mediaModel.length; ++i) {
                    var entry = inputLayer.mediaModel[i]
                    if (!entry || !entry.mediaId)
                        continue

                    var ex = entry.x || 0
                    var ey = entry.y || 0
                    var ew = Math.max(1, (entry.width || 1) * (entry.scale || 1.0))
                    var eh = Math.max(1, (entry.height || 1) * (entry.scale || 1.0))
                    if (sceneX < ex || sceneX > (ex + ew) || sceneY < ey || sceneY > (ey + eh))
                        continue

                    var z = entry.z !== undefined ? entry.z : 0
                    if (z > bestZ || (z === bestZ && i > bestIndex)) {
                        bestZ = z
                        bestIndex = i
                        bestMediaId = entry.mediaId
                    }
                }

                return bestMediaId
            }

            var currentItem = inputLayer.contentItem
            var scenePoint = currentItem.mapFromItem(inputLayer, viewX, viewY)
            var localPoint = Qt.point(scenePoint.x, scenePoint.y)
            var mediaId = ""

            while (currentItem) {
                var child = currentItem.childAt(localPoint.x, localPoint.y)
                if (!child)
                    break

                mediaId = resolveMediaIdFromItemHierarchy(child)
                if (mediaId !== "")
                    break

                localPoint = child.mapFromItem(currentItem, localPoint.x, localPoint.y)
                currentItem = child
            }

            if (mediaId === "") {
                mediaId = mediaIdFromModelBounds(scenePoint.x, scenePoint.y)
                if (mediaId !== "") {
                    logInput("mediaIdAtPoint.fallbackModel",
                             "scene=", scenePoint.x, scenePoint.y,
                             "resolved=", mediaId)
                }
            }

            var resolved = mediaId || ""
            logInput("mediaIdAtPoint", "view=", viewX, viewY, "resolved=", resolved)
            return resolved
        }

        function beginPrimaryGesture(viewX, viewY, hoveredHandleId, hoveredHandleMediaId) {
            primaryGestureActive = true
            resetLastPrimaryPressTarget()

            logInput("beginPrimaryGesture",
                     "view=", viewX, viewY,
                     "hoveredHandleId=", hoveredHandleId || "",
                     "hoveredHandleMediaId=", hoveredHandleMediaId || "",
                     "handlePriority=", inputLayer.selectionHandlePriorityActive,
                     "mode=", mode,
                     "owner=", ownerId)

            var handleId = hoveredHandleId || ""
            if (inputLayer.selectionHandlePriorityActive || handleId !== "") {
                primaryOwnerKind = "handle"
                primaryOwnerMediaId = hoveredHandleMediaId || ""
                pressTargetKind = "handle"
                pressTargetMediaId = primaryOwnerMediaId
                logInput("beginPrimaryGesture.owner", "handle", "media=", primaryOwnerMediaId)
                return primaryOwnerKind
            }

            var hitMediaId = mediaIdAtPoint(viewX, viewY)
            if (hitMediaId !== "") {
                primaryOwnerKind = "media"
                primaryOwnerMediaId = hitMediaId
                pressTargetKind = "media"
                pressTargetMediaId = hitMediaId
                logInput("beginPrimaryGesture.owner", "media", "media=", hitMediaId)
                return primaryOwnerKind
            }

            primaryOwnerKind = "canvas"
            primaryOwnerMediaId = ""
            pressTargetKind = "canvas"
            pressTargetMediaId = ""
            logInput("beginPrimaryGesture.owner", "canvas")
            assertInvariants("beginPrimaryGesture")
            return primaryOwnerKind
        }

        function endPrimaryGesture() {
            logInput("endPrimaryGesture",
                     "pressTarget=", pressTargetKind,
                     "pressMedia=", pressTargetMediaId,
                     "mode=", mode,
                     "owner=", ownerId)
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
            if (primaryOwnerKind !== "media") {
                logInput("ownerAllowsMedia.blocked", "reason=owner-kind", "ownerKind=", primaryOwnerKind, "mediaId=", mediaId || "")
                return false
            }
            var allowed = primaryOwnerMediaId === "" || primaryOwnerMediaId === (mediaId || "")
            if (!allowed) {
                logInput("ownerAllowsMedia.blocked", "reason=owner-media-mismatch",
                         "primaryOwnerMediaId=", primaryOwnerMediaId,
                         "mediaId=", mediaId || "")
            }
            return allowed
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
            var allowed = lastPrimaryPressKind === "canvas"
            if (!allowed) {
                logInput("ownerAllowsEmptyTap.blocked",
                         "lastPrimaryPressKind=", lastPrimaryPressKind,
                         "lastPrimaryPressMediaId=", lastPrimaryPressMediaId)
            }
            return allowed
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

            logInput("noteMediaPrimaryPress",
                     "mediaId=", mediaId,
                     "additive=", !!additive,
                     "primaryOwnerKind=", primaryOwnerKind,
                     "primaryOwnerMediaId=", primaryOwnerMediaId,
                     "mode=", mode,
                     "ownerId=", ownerId)

            pressTargetKind = "media"
            pressTargetMediaId = mediaId
            lastPrimaryPressKind = "media"
            lastPrimaryPressMediaId = mediaId

            if (inputLayer.interactionController) {
                inputLayer.interactionController.requestMediaSelection(mediaId, !!additive)
            } else {
                logInput("noteMediaPrimaryPress.missingController", "mediaId=", mediaId)
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
            var granted = beginMode("move", mediaId)
            logInput("tryBeginMove", "mediaId=", mediaId, "granted=", granted)
            return granted
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
            var granted = beginMode("pan", "canvas")
            logInput("tryBeginPanAt", "view=", viewX, viewY, "granted=", granted)
            return granted
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
            var granted = beginMode("resize", mediaId)
            logInput("tryBeginResize", "mediaId=", mediaId || "", "granted=", granted)
            return granted
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
