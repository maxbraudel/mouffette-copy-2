import QtQuick
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

    function resetMovePressAnchor() {
        mediaDrag.pressAnchorValid = false
        mediaDrag.pressPointerSceneX = 0.0
        mediaDrag.pressPointerSceneY = 0.0
        mediaDrag.pressPointerViewX = 0.0
        mediaDrag.pressPointerViewY = 0.0
    }

    function finishMoveSession(expectedMediaId) {
        if (!mediaDrag || !delegateItem || !rootController)
            return false

        var orphanMediaId = mediaDrag.activeMoveMediaId
        if (!orphanMediaId)
            orphanMediaId = expectedMediaId || ""
        var ownsSharedState = orphanMediaId !== ""
            && (mediaDrag.activeMoveMediaId !== ""
                || mediaDrag.countedAsActive
                || delegateItem.localDragging
                || rootController.liveDragMediaId === orphanMediaId
                || (activeCoordinator
                    && activeCoordinator.mode === "move"
                    && activeCoordinator.ownerId === orphanMediaId))
        if (!ownsSharedState) {
            resetMovePressAnchor()
            return false
        }

        var finalX = delegateItem.effectiveLocalX
        var finalY = delegateItem.effectiveLocalY
        var snapAtEnd = (mediaDrag.centroid.modifiers & Qt.ShiftModifier) !== 0
        var needsSnapCleanup = rootController.liveSnapDragActive
            && rootController.liveSnapDragMediaId === orphanMediaId

        // Clear local ownership before the synchronous backend publication.
        // activeChanged, canceled, destruction and the root watchdog may all
        // converge here; only the first caller is allowed to finish the move.
        mediaDrag.activeMoveMediaId = ""

        if (mediaDrag.countedAsActive) {
            rootController.activeMediaDragCount = Math.max(0, rootController.activeMediaDragCount - 1)
            mediaDrag.countedAsActive = false
        } else if (rootController.liveDragMediaId === orphanMediaId
                   && rootController.activeMediaDragCount > 0) {
            // Recovery can observe the native active flag changing after the
            // per-handler counter callback was skipped.
            rootController.activeMediaDragCount = Math.max(0, rootController.activeMediaDragCount - 1)
        }

        delegateItem.localDragging = false
        resetMovePressAnchor()
        if (rootController.liveDragMediaId === orphanMediaId) {
            rootController.liveDragMediaId = ""
            rootController.liveDragViewOffsetX = 0.0
            rootController.liveDragViewOffsetY = 0.0
        }

        if (activeCoordinator) {
            if (activeCoordinator.mode === "move"
                    && activeCoordinator.ownerId === orphanMediaId)
                activeCoordinator.endMove(orphanMediaId)
            else
                activeCoordinator.releaseMediaOwnership(orphanMediaId, "move")
        }

        rootController.mediaMoveEnded(orphanMediaId, finalX, finalY, snapAtEnd)
        if (needsSnapCleanup)
            requestSnapFreezeCleanup()
        return true
    }

    function releaseOrphanedDrag() {
        finishMoveSession(delegateItem ? delegateItem.currentMediaId : "")
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
            var mediaId = delegateItem.currentMediaId
            var sceneX = eventPoint.scenePosition.x
            var sceneY = eventPoint.scenePosition.y
            if (!activeCoordinator.canActivateMediaAtScenePoint(mediaId, sceneX, sceneY))
                return
            var item = mediaContentItem
            if (!item || typeof item.fireDoubleClick !== "function")
                return
            var additive = (eventPoint.modifiers & Qt.ShiftModifier) !== 0
            // Selection publication crosses the C++ document projection. Make
            // it explicit, then enter the editor on the following event turn so
            // TextEditSession always observes the selected media.
            if (rootController
                    && typeof rootController.requestMediaSelection === "function")
                rootController.requestMediaSelection(mediaId, additive)
            Qt.callLater(function() {
                if (!delegateItem || delegateItem.currentMediaId !== mediaId)
                    return
                var currentItem = mediaContentItem
                if (currentItem && typeof currentItem.fireDoubleClick === "function")
                    currentItem.fireDoubleClick(additive, sceneX, sceneY)
            })
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
                interaction.finishMoveSession("")
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
            interaction.finishMoveSession("")
        }
    }
}
