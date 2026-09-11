import QtQuick 2.15

Item {
    id: interaction
    anchors.fill: parent

    property var rootController: null
    property var coordinatorRef: null
    property bool textToolActive: false
    property bool selectionInteracting: false
    property bool selectionHandlePriorityActive: false
    property var contentRootRef: null
    property var delegateItem: null
    property var mediaContentItem: null

    readonly property var activeCoordinator: coordinatorRef

    containmentMask: QtObject {
        function contains(p: point): bool {
            // A custom mask replaces Qt's default rectangle check. Reject
            // distant delegates before mapping/scanning the canvas picker.
            if (p.x < 0 || p.y < 0 || p.x >= interaction.width || p.y >= interaction.height)
                return false
            if (!interaction.activeCoordinator || !interaction.delegateItem)
                return false
            var scenePoint = interaction.mapToItem(null, p.x, p.y)
            return interaction.activeCoordinator.canObserveMediaAtScenePoint(
                interaction.delegateItem.currentMediaId, scenePoint.x, scenePoint.y)
        }
    }

    signal requestSnapFreezeCleanup()

    readonly property bool active: mediaDrag.active
    readonly property string activeMoveMediaId: mediaDrag.activeMoveMediaId
    readonly property bool countedAsActive: mediaDrag.countedAsActive
    readonly property bool mediaPressSelectEnabledState: !!rootController && !!delegateItem && !!delegateItem.media
                                                     && !delegateItem.overlayHovered
                                                     && !(mediaContentItem && mediaContentItem.editing === true)
                                                     && !!activeCoordinator
    readonly property bool mediaDragEnabledState: !!rootController && !!delegateItem && !!delegateItem.media
                                            && !delegateItem.overlayHovered
                                            && !(mediaContentItem && mediaContentItem.editing === true)
                                            && !textToolActive
                                            && !selectionInteracting
                                            && !!activeCoordinator

    function contentPointFromScene(sceneX, sceneY) {
        if (!contentRootRef)
            return Qt.point(sceneX, sceneY)
        return contentRootRef.mapFromItem(null, sceneX, sceneY)
    }

    function releaseOrphanedDrag() {
        if (!mediaDrag || !delegateItem || !rootController)
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
        if (rootController.liveDragMediaId === orphanMediaId) {
            rootController.liveDragMediaId = ""
            rootController.liveDragViewOffsetX = 0.0
            rootController.liveDragViewOffsetY = 0.0
        }

        if (activeCoordinator) {
            activeCoordinator.releaseMediaOwnership(orphanMediaId)
        }
    }

    TapHandler {
        id: mediaDoubleClick
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        enabled: interaction.mediaPressSelectEnabledState

        onTapped: function(eventPoint) {
            if (tapCount !== 2 || !activeCoordinator || !delegateItem)
                return
            if (!activeCoordinator.canActivateMediaAtScenePoint(delegateItem.currentMediaId,
                                                                 eventPoint.scenePosition.x,
                                                                 eventPoint.scenePosition.y))
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
        // Keep native event observation stable. Ownership is decided once by
        // the canvas's press observer, then checked when handling the event.
        enabled: interaction.mediaPressSelectEnabledState

        function selectNow(modifiers) {
            if (!delegateItem || !activeCoordinator)
                return
            var mediaId = delegateItem.currentMediaId
            if (!mediaId || mediaId.length === 0)
                return
            var additive = (modifiers & Qt.ShiftModifier) !== 0
            activeCoordinator.noteMediaPrimaryPress(mediaId, additive)
        }

        onActiveChanged: {
            if (!active)
                return

            if (!activeCoordinator || !delegateItem || !rootController)
                return

            if (!activeCoordinator.claimMediaPress(delegateItem.currentMediaId))
                return

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
        // Observe native presses without toggling enabled on a previous press's
        // owner. The coordinator grants the move only to this gesture's target.
        enabled: interaction.mediaDragEnabledState

        dragThreshold: 4

        property real pressMediaSceneX: 0.0
        property real pressMediaSceneY: 0.0
        property real pressPointerSceneX: 0.0
        property real pressPointerSceneY: 0.0
        property real pressPointerViewX: 0.0
        property real pressPointerViewY: 0.0
        property bool pressAnchorValid: false
        property string activeMoveMediaId: ""
        property bool countedAsActive: false

        onActiveChanged: {
            if (!delegateItem || !rootController)
                return
            if (active) {
                // The containment mask normally excludes these presses before
                // Qt takes a grab. If state changed since the press, a denied
                // move must still leave another session's shared state intact.
                if (!activeCoordinator || selectionInteracting || selectionHandlePriorityActive) {
                    activeMoveMediaId = ""
                    delegateItem.localDragging = false
                    countedAsActive = false
                    return
                }

                var requestedMediaId = delegateItem.currentMediaId
                if (!activeCoordinator.tryBeginMove(requestedMediaId)) {
                    activeMoveMediaId = ""
                    delegateItem.localDragging = false
                    countedAsActive = false
                    return
                }
                activeMoveMediaId = requestedMediaId
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

                delegateItem.localDragging = true
                rootController.liveDragMediaId = activeMoveMediaId
                var snapAtStart = (mediaDrag.centroid.modifiers & Qt.ShiftModifier) !== 0
                rootController.mediaMoveStarted(activeMoveMediaId,
                                                delegateItem.localX,
                                                delegateItem.localY,
                                                snapAtStart)
            } else {
                var finalMediaId = activeMoveMediaId
                // A handler that did not acquire a move session owns no shared
                // drag state. Native cancellation may notify us more than once.
                if (finalMediaId === "") {
                    pressAnchorValid = false
                    return
                }
                if (countedAsActive) {
                    rootController.activeMediaDragCount = Math.max(0, rootController.activeMediaDragCount - 1)
                    countedAsActive = false
                }
                delegateItem.localDragging = false
                pressAnchorValid = false
                pressPointerSceneX = 0.0
                pressPointerSceneY = 0.0
                pressPointerViewX = 0.0
                pressPointerViewY = 0.0
                rootController.liveDragViewOffsetX = 0.0
                rootController.liveDragViewOffsetY = 0.0
                if (activeCoordinator) {
                    activeCoordinator.endMove(finalMediaId)
                }
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
            if (!active || !delegateItem || !rootController || !delegateItem.localDragging)
                return

            var currentViewPoint = mediaDrag.centroid.scenePosition
            var currentContentPoint = contentPointFromScene(currentViewPoint.x, currentViewPoint.y)
            var deltaSceneX = currentContentPoint.x - pressPointerSceneX
            var deltaSceneY = currentContentPoint.y - pressPointerSceneY
            var deltaViewX = currentViewPoint.x - pressPointerViewX
            var deltaViewY = currentViewPoint.y - pressPointerViewY

            delegateItem.localX = pressMediaSceneX + deltaSceneX
            delegateItem.localY = pressMediaSceneY + deltaSceneY
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
            rootController.liveDragViewOffsetX = deltaViewX
            rootController.liveDragViewOffsetY = deltaViewY
        }

        onCanceled: {
            if (activeMoveMediaId === "" || !delegateItem || !rootController)
                return
            if (countedAsActive) {
                rootController.activeMediaDragCount = Math.max(0, rootController.activeMediaDragCount - 1)
                countedAsActive = false
            }
            delegateItem.localDragging = false
            pressAnchorValid = false
            pressPointerSceneX = 0.0
            pressPointerSceneY = 0.0
            pressPointerViewX = 0.0
            pressPointerViewY = 0.0
            rootController.liveDragMediaId = ""
            rootController.liveDragViewOffsetX = 0.0
            rootController.liveDragViewOffsetY = 0.0
            if (rootController.liveSnapDragActive
                    && rootController.liveSnapDragMediaId === activeMoveMediaId) {
                requestSnapFreezeCleanup()
            }
            if (activeCoordinator) {
                activeCoordinator.endMove(activeMoveMediaId)
            }
            activeMoveMediaId = ""
        }
    }
}
