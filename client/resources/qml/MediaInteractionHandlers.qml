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

    signal requestSnapFreezeCleanup()

    readonly property bool active: mediaDrag.active
    readonly property string activeMoveMediaId: mediaDrag.activeMoveMediaId
    readonly property bool countedAsActive: mediaDrag.countedAsActive
    readonly property bool mediaPressSelectEnabledState: !!delegateItem.media
                                                     && !delegateItem.overlayHovered
                                                     && !(mediaContentItem && mediaContentItem.editing === true)
                                                     && !!activeCoordinator
    readonly property bool mediaDragEnabledState: !!delegateItem.media
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

        if (activeCoordinator) {
            activeCoordinator.forceReset("media-delegate-destroyed")
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
                 && !!activeCoordinator
                 && activeCoordinator.ownerAllowsMedia(delegateItem.currentMediaId, false)

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
        // NOTE: ownerAllowsMedia is intentionally NOT checked here.
        // mediaPressSelect is parented to the media delegate — if it fires, the
        // press is by definition on media. Gating on the coordinator here would
        // create a race with primaryGestureRouter (which fires first on higher-z
        // InputLayer) and can falsely block selection when mediaIdAtPoint fails.
        // Instead, onActiveChanged corrects the coordinator state itself.
        enabled: interaction.mediaPressSelectEnabledState

        function selectNow(modifiers) {
            var mediaId = delegateItem.currentMediaId
            if (!mediaId || mediaId.length === 0)
                return
            var additive = (modifiers & Qt.ShiftModifier) !== 0
            activeCoordinator.noteMediaPrimaryPress(mediaId, additive)
        }

        onActiveChanged: {
            if (!active)
                return

            if (!activeCoordinator)
                return

            // Ensure the coordinator correctly identifies this press as on-media.
            // primaryGestureRouter (on higher-z InputLayer) fires first; if its
            // mediaIdAtPoint traversal missed this item, primaryOwnerKind may be
            // "canvas".  Correct it here — mediaPressSelect firing IS proof the
            // press landed on this media delegate.
            var coord = activeCoordinator
            var myMediaId = delegateItem.currentMediaId
            if (myMediaId && myMediaId.length > 0) {
                if (!coord.primaryGestureActive)
                    coord.primaryGestureActive = true
                if (coord.primaryOwnerKind !== "media"
                        || (coord.primaryOwnerMediaId !== "" && coord.primaryOwnerMediaId !== myMediaId)) {
                    coord.primaryOwnerKind = "media"
                    coord.primaryOwnerMediaId = myMediaId
                    coord.pressTargetKind = "media"
                    coord.pressTargetMediaId = myMediaId
                }
            }

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
        // NOTE: ownerAllowsMedia / canStartMediaMove are intentionally NOT checked here.
        // DragHandlers get their passive grab at press-time, BEFORE beginPrimaryGesture
        // has run and set coordinator ownership. Gating on ownership here creates a
        // race that prevents mediaDrag from ever getting a passive grab, making drag
        // impossible whenever the coordinator starts from a "canvas" classification.
        // Instead, ownership correctness is enforced inside onActiveChanged (below),
        // which runs only after the drag threshold is crossed — by then, mediaPressSelect
        // has already corrected the coordinator. Block only conditions that are truly
        // incompatible with drag at press time:
        //   • overlayHovered  — pointer is on the media overlay UI, not the item body
        //   • editing         — text item in edit mode
        //   • textToolActive  — text-create tool active on canvas
        //   • selectionChrome.interacting — a resize gesture is already in progress
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
            if (active) {
                if (!activeCoordinator) {
                    activeMoveMediaId = ""
                    delegateItem.localDragging = false
                    rootController.liveDragMediaId = ""
                    rootController.liveDragViewOffsetX = 0.0
                    rootController.liveDragViewOffsetY = 0.0
                    countedAsActive = false
                    return
                }
                activeMoveMediaId = delegateItem.currentMediaId

                // Guard: if a resize gesture is already in progress (handle actively being dragged),
                // abort — the drag threshold was reached before the enabled binding could fire.
                if (selectionInteracting) {
                    activeMoveMediaId = ""
                    delegateItem.localDragging = false
                    rootController.liveDragMediaId = ""
                    rootController.liveDragViewOffsetX = 0.0
                    rootController.liveDragViewOffsetY = 0.0
                    countedAsActive = false
                    return
                }

                // Guard: if the press landed on a resize handle, let the resize handler
                // take it — do not start a body drag.
                if (selectionHandlePriorityActive) {
                    activeMoveMediaId = ""
                    delegateItem.localDragging = false
                    rootController.liveDragMediaId = ""
                    rootController.liveDragViewOffsetX = 0.0
                    rootController.liveDragViewOffsetY = 0.0
                    countedAsActive = false
                    return
                }

                // Correct ownership if primaryGestureRouter misclassified the press as
                // "canvas" (same pattern as mediaPressSelect.onActiveChanged).
                // By the time the drag threshold is crossed mediaPressSelect will have
                // already run, but just in case it hasn't (e.g. extremely fast flicks),
                // we self-correct here as well. We only correct when the press target
                // is definitively this media item.
                var coord = activeCoordinator
                if (coord.primaryOwnerKind !== "media"
                        || (coord.primaryOwnerMediaId !== "" && coord.primaryOwnerMediaId !== activeMoveMediaId)) {
                    coord.primaryOwnerKind    = "media"
                    coord.primaryOwnerMediaId = activeMoveMediaId
                    coord.pressTargetKind     = "media"
                    coord.pressTargetMediaId  = activeMoveMediaId
                    coord.primaryGestureActive = true
                }

                var moveGranted = coord.tryBeginMove(activeMoveMediaId)
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
            if (!active || !delegateItem.localDragging)
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
