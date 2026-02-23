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
    property string liveDragMediaId: ""
    readonly property alias inputCoordinator: coordinator
    default property alias layerChildren: layerRoot.data

    signal textCreateRequested(real viewX, real viewY)

    QtObject {
        id: coordinator

        property string mode: "idle"
        property string ownerId: ""
        property string pressTargetKind: "unknown" // unknown | background | media | handle
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
            mode = requestedMode
            ownerId = owner
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
            pressTargetKind = "media"
            pressTargetMediaId = mediaId
            if (inputLayer.interactionController) {
                inputLayer.interactionController.requestMediaSelection(mediaId, !!additive)
            }
            return true
        }

        function canStartMove(media, contentItem, dragActive, mediaId) {
            if (!media)
                return false
            if (!media.selected)
                return false
            if (media.textEditable)
                return false
            if (contentItem && contentItem.editing === true)
                return false
            if (inputLayer.textToolActive)
                return false
            if (inputLayer.selectionHandlePriorityActive)
                return false
            if (dragActive)
                return true
            if (pressTargetKind !== "media" || pressTargetMediaId !== mediaId)
                return false
            return canStart("move", mediaId)
        }

        function tryBeginMove(mediaId) {
            if (!mediaId)
                return false
            return beginMode("move", mediaId)
        }

        function endMove(mediaId) {
            endMode("move", mediaId)
            resetPressTarget()
        }

        function canEnablePan(panActive) {
            return panActive || (isIdle()
                && !inputLayer.textToolActive
                && !inputLayer.selectionHandlePriorityActive
                && inputLayer.liveDragMediaId === "")
        }

        function tryBeginPanAt(viewX, viewY) {
            if (!canEnablePan(false))
                return false
            if (isPointInsideMedia(viewX, viewY)) {
                pressTargetKind = "media"
                pressTargetMediaId = ""
                return false
            }
            pressTargetKind = "background"
            pressTargetMediaId = ""
            return beginMode("pan", "canvas")
        }

        function endPan() {
            endMode("pan", "canvas")
            resetPressTarget()
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
            if (!canStartTextToolTap())
                return false
            if (isPointInsideMedia(viewX, viewY)) {
                pressTargetKind = "media"
                pressTargetMediaId = ""
                return false
            }
            pressTargetKind = "background"
            pressTargetMediaId = ""
            if (!beginMode("text", "canvas"))
                return false
            inputLayer.textCreateRequested(viewX, viewY)
            endMode("text", "canvas")
            resetPressTarget()
            return true
        }
    }

    Item {
        id: layerRoot
        anchors.fill: parent
    }
}
