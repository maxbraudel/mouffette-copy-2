import QtQuick 2.15

Item {
    id: interaction
    anchors.fill: parent

    property var rootController: null
    property var inputLayer: null
    property var selectionChrome: null
    property var viewport: null
    property var delegateItem: null
    property var mediaContentItem: null

    signal requestSnapFreezeCleanup()

    readonly property bool active: mediaDrag.active
    readonly property string activeMoveMediaId: mediaDrag.activeMoveMediaId
    readonly property bool countedAsActive: mediaDrag.countedAsActive

    function contentPointFromScene(sceneX, sceneY) {
        return viewport.contentRootItem.mapFromItem(null, sceneX, sceneY)
    }

    function releaseOrphanedDrag() {
        if (!mediaDrag)
            return
        if (!mediaDrag.active && mediaDrag.activeMoveMediaId === "" && !delegateItem.localDragging)
            return

        var orphanMediaId = mediaDrag.activeMoveMediaId
        if (!orphanMediaId || orphanMediaId.length === 0)
            orphanMediaId = delegateItem.currentMediaId

        if (mediaDrag.countedAsActive) {
            rootController.activeMediaDragCount = Math.max(0, rootController.activeMediaDragCount - 1)
            mediaDrag.countedAsActive = false
        }

        delegateItem.localDragging = false
        rootController.liveDragMediaId = ""
        rootController.liveDragViewOffsetX = 0.0
        rootController.liveDragViewOffsetY = 0.0

        if (inputLayer && inputLayer.inputCoordinator) {
            inputLayer.inputCoordinator.forceReset("media-delegate-destroyed")
        }
    }

    TapHandler {
        id: mediaDoubleClick
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        enabled: !!delegateItem.media
                 && !delegateItem.overlayHovered
                 && !(mediaContentItem && mediaContentItem.editing === true)
                 && selectionChrome.hoveredHandleId === ""
                 && (!!inputLayer
                     && !!inputLayer.inputCoordinator
                     && inputLayer.inputCoordinator.ownerAllowsMedia(delegateItem.currentMediaId, false))

        onTapped: function(eventPoint) {
            if (tapCount !== 2)
                return
            var item = mediaContentItem
            if (!item || typeof item.fireDoubleClick !== "function")
                return
            var additive = (eventPoint.modifiers & Qt.ShiftModifier) !== 0
            item.fireDoubleClick(additive)
        }
    }

    PointHandler {
        id: mediaPressSelect
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        enabled: !!delegateItem.media
                 && !delegateItem.overlayHovered
                 && !(mediaContentItem && mediaContentItem.editing === true)
                 && selectionChrome.hoveredHandleId === ""
                 && (!!inputLayer
                     && !!inputLayer.inputCoordinator
                     && inputLayer.inputCoordinator.ownerAllowsMedia(delegateItem.currentMediaId, mediaPressSelect.active))

        function selectNow(modifiers) {
            var mediaId = delegateItem.currentMediaId
            console.log("[DBG][selectNow] mediaId=", mediaId, "inputLayer=", !!inputLayer, "coordinator=", !!(inputLayer && inputLayer.inputCoordinator), "interactionCtrl=", !!(inputLayer && inputLayer.interactionController))
            if (!mediaId || mediaId.length === 0)
                return

            var additive = (modifiers & Qt.ShiftModifier) !== 0
            inputLayer.inputCoordinator.noteMediaPrimaryPress(mediaId, additive)
        }

        onActiveChanged: {
            if (!active)
                return

            console.log("[DBG][mediaPressSelect] onActiveChanged active=true, mediaId=", delegateItem ? delegateItem.currentMediaId : "NO_DELEGATE", "enabled=", mediaPressSelect.enabled)

            var modifiers = Qt.application.keyboardModifiers
            if (mediaPressSelect.point && mediaPressSelect.point.modifiers !== undefined)
                modifiers = mediaPressSelect.point.modifiers

            var pressScenePoint = mediaPressSelect.point
                ? mediaPressSelect.point.scenePosition
                : mediaDrag.centroid.scenePressPosition
            var pressContentPoint = contentPointFromScene(pressScenePoint.x, pressScenePoint.y)
            mediaDrag.pressPointerSceneX = pressContentPoint.x
            mediaDrag.pressPointerSceneY = pressContentPoint.y
            mediaDrag.pressPointerViewX = pressScenePoint.x
            mediaDrag.pressPointerViewY = pressScenePoint.y
            mediaDrag.pressMediaSceneX = delegateItem.localX
            mediaDrag.pressMediaSceneY = delegateItem.localY
            mediaDrag.pressAnchorValid = true

            selectNow(modifiers)
        }
    }

    DragHandler {
        id: mediaDrag
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        enabled: !delegateItem.overlayHovered
                 && (!!inputLayer
                     && !!inputLayer.inputCoordinator
                     && inputLayer.inputCoordinator.ownerAllowsMedia(delegateItem.currentMediaId, mediaDrag.active))
                 && rootController.canStartMediaMove(delegateItem.media,
                                                     mediaContentItem,
                                                     mediaDrag.active,
                                                     delegateItem.currentMediaId)
        dragThreshold: 4

        property real startX: 0.0
        property real startY: 0.0
        property real pressMediaSceneX: 0.0
        property real pressMediaSceneY: 0.0
        property real dragOriginSceneX: 0.0
        property real dragOriginSceneY: 0.0
        property real pressPointerSceneX: 0.0
        property real pressPointerSceneY: 0.0
        property real dragOriginViewX: 0.0
        property real dragOriginViewY: 0.0
        property real pressPointerViewX: 0.0
        property real pressPointerViewY: 0.0
        property bool pressAnchorValid: false
        property string activeMoveMediaId: ""
        property bool countedAsActive: false

        onActiveChanged: {
            if (active) {
                activeMoveMediaId = delegateItem.currentMediaId
                var moveGranted = inputLayer.inputCoordinator.tryBeginMove(activeMoveMediaId)
                if (!moveGranted) {
                    activeMoveMediaId = ""
                    delegateItem.localDragging = false
                    rootController.liveDragMediaId = ""
                    rootController.liveDragViewOffsetX = 0.0
                    rootController.liveDragViewOffsetY = 0.0
                    countedAsActive = false
                    return
                }
                if (!countedAsActive) {
                    rootController.activeMediaDragCount += 1
                    countedAsActive = true
                }

                var pressSceneX = pressPointerSceneX
                var pressSceneY = pressPointerSceneY
                var pressViewX = pressPointerViewX
                var pressViewY = pressPointerViewY
                if (!pressAnchorValid) {
                    var pressPoint = mediaDrag.centroid.scenePressPosition
                    pressSceneX = pressPoint.x
                    pressSceneY = pressPoint.y
                    pressViewX = pressPoint.x
                    pressViewY = pressPoint.y
                    var pressContentPoint = contentPointFromScene(pressSceneX, pressSceneY)
                    pressPointerSceneX = pressContentPoint.x
                    pressPointerSceneY = pressContentPoint.y
                    pressPointerViewX = pressViewX
                    pressPointerViewY = pressViewY
                    pressMediaSceneX = delegateItem.localX
                    pressMediaSceneY = delegateItem.localY
                    pressAnchorValid = true
                }

                dragOriginSceneX = pressMediaSceneX
                dragOriginSceneY = pressMediaSceneY
                startX = dragOriginSceneX
                startY = dragOriginSceneY
                dragOriginViewX = pressViewX
                dragOriginViewY = pressViewY
                delegateItem.localDragging = true
                rootController.liveDragMediaId = activeMoveMediaId
                var snapAtStart = (mediaDrag.centroid.modifiers & Qt.ShiftModifier) !== 0
                rootController.mediaMoveStarted(activeMoveMediaId,
                                                delegateItem.localX,
                                                delegateItem.localY,
                                                snapAtStart)
            } else {
                var finalMediaId = activeMoveMediaId
                if (countedAsActive) {
                    rootController.activeMediaDragCount = Math.max(0, rootController.activeMediaDragCount - 1)
                    countedAsActive = false
                }
                delegateItem.localDragging = false
                pressAnchorValid = false
                dragOriginSceneX = 0.0
                dragOriginSceneY = 0.0
                pressPointerSceneX = 0.0
                pressPointerSceneY = 0.0
                dragOriginViewX = 0.0
                dragOriginViewY = 0.0
                pressPointerViewX = 0.0
                pressPointerViewY = 0.0
                rootController.liveDragViewOffsetX = 0.0
                rootController.liveDragViewOffsetY = 0.0
                inputLayer.inputCoordinator.endMove(finalMediaId)
                rootController.liveDragMediaId = ""
                activeMoveMediaId = ""

                if (finalMediaId !== "") {
                    var snapAtEnd = (mediaDrag.centroid.modifiers & Qt.ShiftModifier) !== 0
                    rootController.mediaMoveEnded(finalMediaId,
                                                  delegateItem.effectiveLocalX,
                                                  delegateItem.effectiveLocalY,
                                                  snapAtEnd)
                }
                if (typeof rootController !== "undefined" && rootController.liveSnapDragActive
                        && rootController.liveSnapDragMediaId === finalMediaId) {
                    requestSnapFreezeCleanup()
                }
            }
        }

        onTranslationChanged: {
            if (!active || !delegateItem.localDragging)
                return

            delegateItem.localX = pressMediaSceneX + mediaDrag.translation.x
            delegateItem.localY = pressMediaSceneY + mediaDrag.translation.y
            if (activeMoveMediaId !== "") {
                var snapNow = (mediaDrag.centroid.modifiers & Qt.ShiftModifier) !== 0
                rootController.mediaMoveUpdated(activeMoveMediaId,
                                                delegateItem.localX,
                                                delegateItem.localY,
                                                snapNow)
            }
            if (delegateItem.usesSnapDrag) {
                delegateItem.localX = rootController.liveSnapDragX
                delegateItem.localY = rootController.liveSnapDragY
            }
            rootController.liveDragViewOffsetX = mediaDrag.translation.x * rootController.viewScale
            rootController.liveDragViewOffsetY = mediaDrag.translation.y * rootController.viewScale
        }

        onCanceled: {
            if (countedAsActive) {
                rootController.activeMediaDragCount = Math.max(0, rootController.activeMediaDragCount - 1)
                countedAsActive = false
            }
            delegateItem.localDragging = false
            pressAnchorValid = false
            dragOriginSceneX = 0.0
            dragOriginSceneY = 0.0
            pressPointerSceneX = 0.0
            pressPointerSceneY = 0.0
            dragOriginViewX = 0.0
            dragOriginViewY = 0.0
            pressPointerViewX = 0.0
            pressPointerViewY = 0.0
            rootController.liveDragMediaId = ""
            rootController.liveDragViewOffsetX = 0.0
            rootController.liveDragViewOffsetY = 0.0
            if (rootController.liveSnapDragActive
                    && rootController.liveSnapDragMediaId === activeMoveMediaId) {
                requestSnapFreezeCleanup()
            }
            inputLayer.inputCoordinator.endMove(activeMoveMediaId)
            activeMoveMediaId = ""
        }
    }
}
