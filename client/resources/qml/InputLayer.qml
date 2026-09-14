import QtQuick
Item {
    id: inputLayer

    property var interactionController: null
    property bool textToolActive: false
    property bool selectionHandlePriorityActive: false
    property string liveDragMediaId: ""
    readonly property alias inputCoordinator: coordinator
    default property alias layerChildren: layerRoot.data

    signal textCreateRequested(real viewX, real viewY)

    QtObject {
        id: coordinator
        objectName: "canvasInputCoordinator"

        property string mode: "idle"
        property string ownerId: ""
        property bool primaryGestureActive: false
        property string primaryOwnerKind: "none" // none | handle | media | canvas
        property string primaryOwnerMediaId: ""
        property bool primarySelectionDispatched: false
        readonly property string pressTargetKind: primaryGestureActive ? primaryOwnerKind : "unknown"
        readonly property string pressTargetMediaId: primaryOwnerMediaId

        function resetPrimaryOwner() {
            primaryGestureActive = false
            primaryOwnerKind = "none"
            primaryOwnerMediaId = ""
            primarySelectionDispatched = false
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
            if (!inputLayer.interactionController
                    || typeof inputLayer.interactionController.mediaIdAtPoint !== "function")
                return ""
            // CanvasRoot resolves the actual, topmost visual delegate. Do not
            // guess from decorative children or a stale geometry snapshot.
            return inputLayer.interactionController.mediaIdAtPoint(viewX, viewY) || ""
        }

        function beginPrimaryGesture(viewX, viewY, pressedHandleId, pressedHandleMediaId) {
            resetPrimaryOwner()
            primaryGestureActive = true

            // The caller performs an exact handle hit test for this press;
            // hover feedback must never determine gesture ownership.
            var handleId = pressedHandleId || ""
            if (handleId !== "") {
                primaryOwnerKind = "handle"
                primaryOwnerMediaId = pressedHandleMediaId || ""
                return primaryOwnerKind
            }

            var hitMediaId = mediaIdAtPoint(viewX, viewY)
            if (hitMediaId !== "") {
                primaryOwnerKind = "media"
                primaryOwnerMediaId = hitMediaId
                return primaryOwnerKind
            }

            primaryOwnerKind = "canvas"
            primaryOwnerMediaId = ""
            assertInvariants("beginPrimaryGesture")
            return primaryOwnerKind
        }

        function endPrimaryGesture() {
            resetPrimaryOwner()
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

        function claimMediaPress(mediaId) {
            // Delegates consume the press decision; they cannot replace it.
            // This also prevents an underlying delegate or handle/body overlap
            // from selecting a second item during the same pointer event.
            return !!mediaId
                && primaryGestureActive
                && primaryOwnerKind === "media"
                && primaryOwnerMediaId === mediaId
        }

        function canObserveMediaAtScenePoint(mediaId, sceneX, sceneY) {
            if (!mediaId || !ownerAllowsMedia(mediaId, false))
                return false
            var viewPoint = inputLayer.mapFromItem(null, sceneX, sceneY)
            return mediaIdAtPoint(viewPoint.x, viewPoint.y) === mediaId
        }

        function canActivateMediaAtScenePoint(mediaId, sceneX, sceneY) {
            return isIdle() && canObserveMediaAtScenePoint(mediaId, sceneX, sceneY)
        }

        function releaseMediaOwnership(mediaId, expectedMode) {
            // The logical mode and the native press have different lifetimes.
            // A delegate or resize session can disappear while the global
            // PointHandler still owns the physical press; keep that ownership
            // until the router observes release/cancel so another handler
            // cannot steal the remainder of the same gesture.
            if (!mediaId)
                return
            if (ownerId === mediaId
                    && (!expectedMode || mode === expectedMode)) {
                mode = "idle"
                ownerId = ""
            }
        }

        function ownerAllowsCanvasPan(active) {
            if (active)
                return true
            if (!primaryGestureActive)
                return true
            return primaryOwnerKind === "canvas"
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
            resetPrimaryOwner()
            assertInvariants("forceReset")
        }

        function isPointInsideMedia(viewX, viewY) {
            return mediaIdAtPoint(viewX, viewY) !== ""
        }

        function noteMediaPrimaryPress(mediaId, additive) {
            if (!claimMediaPress(mediaId))
                return false
            if (primarySelectionDispatched)
                return true
            primarySelectionDispatched = true

            if (inputLayer.interactionController) {
                inputLayer.interactionController.requestMediaSelection(mediaId, !!additive)
            }
            return true
        }

        function tryBeginMove(mediaId) {
            if (!claimMediaPress(mediaId))
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
            return !primaryGestureActive
                || (primaryOwnerKind === "handle" && primaryOwnerMediaId === mediaId)
        }

        function tryBeginResize(mediaId) {
            if (!mediaId || !primaryGestureActive
                    || primaryOwnerKind !== "handle" || primaryOwnerMediaId !== mediaId)
                return false
            return beginMode("resize", mediaId)
        }

        function endResize(mediaId) {
            endMode("resize", mediaId)
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
            try {
                inputLayer.textCreateRequested(viewX, viewY)
            } finally {
                endMode("text", "canvas")
            }
            return true
        }
    }

    Item {
        id: layerRoot
        anchors.fill: parent
    }
}
