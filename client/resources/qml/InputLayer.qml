import QtQuick 2.15

Item {
    id: inputLayer

    // Temporary rollout switch:
    //  - default: coordinator enabled
    //  - pass --legacy-input-arbitration to disable coordinator paths
    property bool useInputCoordinator: Qt.application.arguments.indexOf("--legacy-input-arbitration") === -1
    property var interactionController: null
    property bool textToolActive: false
    property var mediaModel: []
    property Item contentItem: null
    property bool selectionHandlePriorityActive: false
    property string selectionHandleHoveredMediaId: ""
    property string liveDragMediaId: ""
    property bool inputDebugEnabled: !!interactionController && !!interactionController.inputDebugEnabled
    readonly property alias inputCoordinator: coordinator
    default property alias layerChildren: layerRoot.data

    signal textCreateRequested(real viewX, real viewY)

    function debugLog(eventName,
                      arg1,
                      arg2,
                      arg3,
                      arg4,
                      arg5,
                      arg6) {
        if (!inputDebugEnabled)
            return
        console.log("[QuickCanvas][InputCoordinator]",
                    eventName,
                    arg1 === undefined ? "" : arg1,
                    arg2 === undefined ? "" : arg2,
                    arg3 === undefined ? "" : arg3,
                    arg4 === undefined ? "" : arg4,
                    arg5 === undefined ? "" : arg5,
                    arg6 === undefined ? "" : arg6)
    }

    QtObject {
        id: coordinator

        property string mode: "idle"
        property string ownerId: ""
        property string pressTargetKind: "unknown" // unknown | handle
        property string pressTargetMediaId: ""

        function resetPressTarget() {
            pressTargetKind = "unknown"
            pressTargetMediaId = ""
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
            var previousMode = mode
            var previousOwner = ownerId
            mode = requestedMode
            ownerId = owner
            inputLayer.debugLog("begin",
                                "previousMode=", previousMode,
                                "previousOwner=", previousOwner,
                                "nextMode=", mode,
                                "nextOwner=", ownerId)
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
            var previousMode = mode
            var previousOwner = ownerId
            mode = "idle"
            ownerId = ""
            inputLayer.debugLog("end",
                                "previousMode=", previousMode,
                                "previousOwner=", previousOwner,
                                "nextMode=", mode,
                                "nextOwner=", ownerId)
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
            inputLayer.debugLog("force-reset",
                                "reason=", reason || "unknown",
                                "nextMode=", mode)
        }

        function isPointInsideMedia(viewX, viewY) {
            if (!inputLayer.contentItem || !inputLayer.mediaModel)
                return false

            var viewScale = inputLayer.contentItem.scale || 1.0
            var contentX = inputLayer.contentItem.x || 0.0
            var contentY = inputLayer.contentItem.y || 0.0

            for (var i = inputLayer.mediaModel.length - 1; i >= 0; --i) {
                var entry = inputLayer.mediaModel[i]
                if (!entry)
                    continue

                var mediaScale = entry.scale || 1.0
                var sceneW = Math.max(1, (entry.width || 1) * mediaScale)
                var sceneH = Math.max(1, (entry.height || 1) * mediaScale)
                var left = contentX + (entry.x || 0) * viewScale
                var top = contentY + (entry.y || 0) * viewScale
                var width = sceneW * viewScale
                var height = sceneH * viewScale
                if (viewX >= left && viewX <= (left + width)
                        && viewY >= top && viewY <= (top + height)) {
                    return true
                }
            }
            return false
        }

        function noteMediaPrimaryPress(mediaId, additive) {
            if (!mediaId)
                return false
            inputLayer.debugLog("primary-press",
                                "mediaId=", mediaId,
                                "additive=", !!additive,
                                "mode=", mode,
                                "owner=", ownerId)
            if (inputLayer.interactionController) {
                inputLayer.interactionController.requestMediaSelection(mediaId, !!additive)
            }
            return true
        }

        function describeMoveBlock(media, contentItem, dragActive, mediaId) {
            if (!media)
                return "no-media"
            if (media.textEditable)
                return "media-text-editable"
            if (contentItem && contentItem.editing === true)
                return "content-editing"
            if (inputLayer.textToolActive)
                return "text-tool-active"
            // Only block drag if a resize is actively in progress (interacting),
            // OR if the hovered handle belongs to THIS specific item.
            // Never block drag on a different item just because another item's handle is hovered.
            if (inputLayer.selectionHandlePriorityActive)
                return "selection-handle-priority"
            var hoveredId = inputLayer.selectionHandleHoveredMediaId || ""
            if (hoveredId !== "" && hoveredId === mediaId)
                return "selection-handle-hovered-on-this-item"
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
            return panActive || (isIdle()
                && !inputLayer.textToolActive
                && !inputLayer.selectionHandlePriorityActive
                && inputLayer.liveDragMediaId === "")
        }

        function tryBeginPanAt(viewX, viewY) {
            if (!canEnablePan(false)) {
                inputLayer.debugLog("pan-begin-blocked",
                                    "reason=coordinator-busy-or-tool-constraint",
                                    "mode=", mode,
                                    "owner=", ownerId,
                                    "view=", viewX + "," + viewY)
                return false
            }
            if (isPointInsideMedia(viewX, viewY)) {
                inputLayer.debugLog("pan-begin-blocked",
                                    "reason=point-inside-media",
                                    "view=", viewX + "," + viewY)
                return false
            }
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
        }

        function canStartTextToolTap() {
            return isIdle()
                && inputLayer.textToolActive
                && !inputLayer.selectionHandlePriorityActive
                && inputLayer.liveDragMediaId === ""
        }

        function tryBeginTextCreateAt(viewX, viewY) {
            if (!canStartTextToolTap()) {
                inputLayer.debugLog("text-create-blocked",
                                    "reason=coordinator-busy-or-tool-constraint",
                                    "mode=", mode,
                                    "owner=", ownerId,
                                    "view=", viewX + "," + viewY)
                return false
            }
            if (isPointInsideMedia(viewX, viewY)) {
                inputLayer.debugLog("text-create-blocked",
                                    "reason=point-inside-media",
                                    "view=", viewX + "," + viewY)
                return false
            }
            if (!beginMode("text", "canvas")) {
                inputLayer.debugLog("text-create-blocked",
                                    "reason=failed-begin-mode",
                                    "view=", viewX + "," + viewY)
                return false
            }
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
