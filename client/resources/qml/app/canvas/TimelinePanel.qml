import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import QtQuick.Window
import Mouffette.App

FocusScope {
    id: root
    objectName: "sceneTimeline"
    required property var session
    readonly property var timeline: session ? session.timeline : null
    readonly property bool editable: !!timeline && timeline.editable
    readonly property real maximumMs: timeline ? timeline.maxDurationMs : 180000
    property real viewDurationMs: timeline ? Math.min(maximumMs, timeline.initialViewDurationMs) : 15000
    readonly property real pixelsPerMs: Math.max(1, trackViewport.width - 24) / Math.max(1, viewDurationMs)
    readonly property real rulerHeight: timeline ? timeline.rulerHeightPx : 28
    readonly property real clipHeight: timeline ? timeline.clipTrackHeightPx : 64
    readonly property real keyHeight: Math.max(30, trackViewport.height - rulerHeight - clipHeight - 16)
    readonly property real slotMs: 1000 / (timeline ? timeline.slotsPerSecond : 30)
    // Visual grouping only: authoring always retains the project slot size.
    readonly property int gridStride: Math.max(1, Math.ceil(8 / (slotMs * pixelsPerMs)))
    readonly property real gridStep: gridStride * slotMs
    readonly property real tickStep: Math.max(1, Math.ceil(80 / (gridStep * pixelsPerMs))) * gridStep
    property bool shiftHeld: false
    property var activeDrag: null
    property real snapGuideMs: -1
    property string snapGuideLabel: ""
    readonly property bool textInputFocused: {
        var item = root.Window.window ? root.Window.window.activeFocusItem : null
        while (item) {
            if (item instanceof TextInput || item instanceof TextEdit) return true
            item = item.parent
        }
        return false
    }
    implicitHeight: timeline ? timeline.timelineHeightPx : 240

    function formatTime(ms) {
        ms = Math.max(0, Math.round(ms))
        return String(Math.floor(ms / 60000)).padStart(2, "0") + ":"
             + String(Math.floor(ms / 1000) % 60).padStart(2, "0") + "."
             + String(ms % 1000).padStart(3, "0")
    }
    function parseTime(text) {
        var parts = text.trim().split(":")
        if (parts.length > 2 || !/^\d+(?::\d{1,2})?(?:\.\d{1,3})?$/.test(text.trim())) return NaN
        return Math.round((parts.length === 2 ? Number(parts[0]) * 60 + Number(parts[1]) : Number(parts[0])) * 1000)
    }
    function focusTrack() { timeField.focus = false; root.forceActiveFocus() }
    function clampTime(ms) { return timeline ? timeline.gridTime(ms) : 0 }
    function snapTime(ms, excludeId, duration) {
        snapGuideMs = -1
        snapGuideLabel = ""
        if (!shiftHeld || !timeline) return clampTime(ms)
        var result = timeline.snapTime(ms, pixelsPerMs, excludeId || "", duration || 0)
        if (result.snapped) {
            snapGuideMs = result.targetTimeMs
            snapGuideLabel = result.mediaName
        }
        return clampTime(result.timeMs)
    }
    function endDrag() { activeDrag = null; snapGuideMs = -1; snapGuideLabel = "" }
    function scrollTo(x) {
        trackViewport.contentX = Math.max(0, Math.min(trackViewport.contentWidth - trackViewport.width, x))
    }
    function zoomAround(factor, timeMs, anchorX) {
        viewDurationMs = Math.max(100, Math.min(maximumMs, viewDurationMs * factor))
        scrollTo(timeMs * pixelsPerMs + 12 - anchorX)
    }
    function zoom(factor) {
        var timeMs = timeline ? timeline.positionMs : 0
        var headX = timeMs * pixelsPerMs + 12 - trackViewport.contentX
        zoomAround(factor, timeMs,
            headX >= 0 && headX <= trackViewport.width ? headX : trackViewport.width / 2)
    }
    function handleWheel(wheel) {
        if (!timeline || activeDrag) { wheel.accepted = true; return }
        var precise = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0
        var dx = precise ? wheel.pixelDelta.x : wheel.angleDelta.x / 8
        var dy = precise ? wheel.pixelDelta.y : wheel.angleDelta.y / 8
        if ((wheel.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0) {
            var delta = wheel.inverted ? -dy : dy
            var trackpad = precise || wheel.phase !== Qt.NoScrollPhase
                || (wheel.device && wheel.device.type === PointerDevice.TouchPad)
            if (delta !== 0)
                zoom(trackpad ? Math.exp(-delta * 0.003) : Math.pow(1.0015, -delta * 5))
        } else {
            // Either swipe axis scrolls time; the dominant axis avoids doubling diagonals.
            scrollTo(trackViewport.contentX - (Math.abs(dx) > Math.abs(dy) ? dx : dy) * (precise ? 1 : 3))
        }
        wheel.accepted = true
    }
    function followHead() {
        if (!timeline || (!timeline.playing && !timeline.remoteActive)) return
        var head = timeline.positionMs * pixelsPerMs + 12
        if (head < trackViewport.contentX || head > trackViewport.contentX + trackViewport.width - 30)
            trackViewport.contentX = Math.max(0, Math.min(trackViewport.contentWidth - trackViewport.width,
                head - trackViewport.width * 0.2))
    }
    onShiftHeldChanged: if (activeDrag) activeDrag.refreshPreview()
    onTimelineChanged: {
        viewDurationMs = timeline ? Math.min(maximumMs, timeline.initialViewDurationMs) : 15000
        trackViewport.contentX = 0
        endDrag()
    }
    Keys.onPressed: event => { if (event.key === Qt.Key_Shift) { shiftHeld = true; event.accepted = true } }
    Keys.onReleased: event => { if (event.key === Qt.Key_Shift) { shiftHeld = false; event.accepted = true } }
    Connections {
        target: root.timeline
        function onTransportChanged() { root.followHead() }
    }
    Rectangle { anchors.fill: parent; color: Theme.overlayBackground; border.color: Theme.overlayBorder }

    component TimelineButton: Button {
        id: button
        implicitHeight: 28
        implicitWidth: Math.max(28, contentItem.implicitWidth + 16)
        padding: 6
        font.pixelSize: 12
        focusPolicy: Qt.NoFocus
        Accessible.name: text
        contentItem: Text {
            text: button.text
            color: button.enabled ? Theme.overlayText : Theme.overlayDisabledText
            font: button.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 4
            color: button.down ? Theme.overlayPressed : button.hovered ? Theme.overlayHover : "transparent"
            border.color: button.enabled ? Theme.overlayBorder : "transparent"
        }
    }
    component KeyframeDiamond: Rectangle {
        width: root.timeline ? root.timeline.keyframeSizePx : 10
        height: width
        rotation: 45
        color: Theme.overlayText
        border.color: Theme.overlayBackground
    }
    Flickable {
        id: transportViewport
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 6
        height: 30
        contentWidth: transport.width
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Row {
            id: transport
            spacing: 6
            TimelineButton {
                objectName: "timelineGoToStart"
                text: "|◀"
                Accessible.name: "Return to start"
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.goToStart() }
            }
            TimelineButton {
                objectName: "timelinePlayPause"
                text: root.timeline && root.timeline.playing ? "Pause" : "Play"
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.togglePlayback() }
            }
            TimelineButton {
                objectName: "timelineGoToEnd"
                text: "▶|"
                Accessible.name: "Go to scene end"
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.goToEnd() }
            }
            TextField {
                id: timeField
                objectName: "timelineTimeField"
                width: 108; height: 28
                color: Theme.overlayText
                font.family: "monospace"
                font.pixelSize: 12
                selectByMouse: true
                enabled: !!root.timeline && !root.timeline.remoteActive
                Accessible.name: "Timeline position, minutes:seconds.milliseconds"
                text: root.formatTime(root.timeline ? root.timeline.positionMs : 0)
                background: Rectangle { color: Theme.overlayBackground; radius: 4; border.color: timeField.activeFocus ? Theme.focusBorder : Theme.overlayBorder }
                onEditingFinished: {
                    var ms = root.parseTime(text)
                    if (isFinite(ms) && root.timeline) root.timeline.seek(root.clampTime(ms))
                    text = Qt.binding(function() { return root.formatTime(root.timeline ? root.timeline.positionMs : 0) })
                }
            }
            Text {
                height: 28
                objectName: "timelineSlotLabel"
                text: "#" + (root.timeline ? root.timeline.positionSlot : 0) + " · "
                    + (root.timeline ? root.timeline.slotsPerSecond : 30) + " slots/s  / "
                    + root.formatTime(root.timeline ? root.timeline.effectiveEndMs : root.maximumMs)
                color: Theme.overlayDisabledText; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter
            }
            TimelineButton { objectName: "timelineZoomOut"; text: "−"; Accessible.name: "Zoom out timeline"; onClicked: root.zoom(2) }
            TimelineButton { objectName: "timelineZoomIn"; text: "+"; Accessible.name: "Zoom in timeline"; onClicked: root.zoom(0.5) }
            TimelineButton { text: "Fit duration"; onClicked: { root.viewDurationMs = root.maximumMs; trackViewport.contentX = 0 } }
            TimelineButton {
                objectName: "timelinePlaceStop"
                text: root.timeline && root.timeline.stopTimeMs >= 0 ? "Move Stop here" : "Place Stop"
                enabled: root.editable
                onClicked: { root.focusTrack(); root.timeline.placeStop() }
            }
            TimelineButton {
                objectName: "timelineRemoveStop"
                text: "Remove Stop"
                visible: !!root.timeline && root.timeline.stopTimeMs >= 0
                enabled: root.editable
                onClicked: { root.focusTrack(); root.timeline.removeStop() }
            }
            TimelineButton {
                objectName: "timelineStopRemote"
                text: "Stop Remote Scene"
                visible: !!root.timeline && root.timeline.remoteActive
                enabled: !!root.session && root.session.remoteSceneActionEnabled
                onClicked: root.session.toggleRemoteScene()
            }
        }
    }
    Flickable {
        id: actionViewport
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: transportViewport.bottom
        anchors.leftMargin: 6; anchors.rightMargin: 6; anchors.topMargin: 4
        height: 30; clip: true
        contentWidth: actions.width; contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        Row {
            id: actions
            spacing: 6
            Text {
                width: Math.min(170, implicitWidth); height: 28
                text: root.timeline && root.timeline.primaryMediaName ? root.timeline.primaryMediaName : "Select a media"
                color: Theme.overlayText; font.pixelSize: 12; font.bold: true
                verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
            }
            TimelineButton {
                objectName: "timelinePlaceKeyframe"
                text: root.timeline && root.timeline.hasKeyframeAtPosition ? "Update keyframe" : "Place keyframe"
                enabled: root.editable && root.timeline.canCapture
                onClicked: { root.focusTrack(); root.timeline.placeKeyframe() }
            }
            TimelineButton {
                objectName: "timelineDelete"
                text: "Delete"
                enabled: root.editable && (root.timeline.selectedKeyframeId !== "" || root.timeline.selectedClipId !== "")
                onClicked: { root.focusTrack(); root.timeline.deleteSelected() }
            }
            TimelineButton {
                text: "Copy"
                enabled: root.editable && (root.timeline.selectedKeyframeId !== "" || root.timeline.selectedClipId !== "")
                onClicked: { root.focusTrack(); root.timeline.copySelected() }
            }
            TimelineButton {
                text: "Paste"
                enabled: root.editable && root.timeline.canPaste
                onClicked: { root.focusTrack(); root.timeline.paste() }
            }
            TimelineButton {
                objectName: "timelineSplitClip"
                text: "Split clip"
                visible: !!root.timeline && root.timeline.primaryIsVideo
                enabled: root.editable && root.timeline.canSplit
                onClicked: { root.focusTrack(); root.timeline.splitClip() }
            }
            TimelineButton {
                objectName: "timelineInsertClip"
                text: "Insert full video"
                visible: !!root.timeline && root.timeline.primaryIsVideo
                enabled: root.editable
                onClicked: { root.focusTrack(); root.timeline.insertFullClip() }
            }
            Text {
                height: 28
                visible: !!root.timeline && root.timeline.hasDraft
                text: "Unsaved draft · place a keyframe to keep changes"
                color: Theme.accent; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter
            }
        }
    }
    Flickable {
        id: trackViewport
        objectName: "timelineTracks"
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: actionViewport.bottom; anchors.topMargin: 4; anchors.bottom: parent.bottom
        clip: true
        contentWidth: Math.max(width, root.maximumMs * root.pixelsPerMs + 24)
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        interactive: false
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AlwaysOn }
        Item {
            id: timeContent
            width: trackViewport.contentWidth; height: trackViewport.height - 16
            Rectangle { width: parent.width; height: root.rulerHeight; color: Theme.overlaySelected }
            Rectangle { y: root.rulerHeight; width: parent.width; height: 1; color: Theme.overlayBorder }
            MouseArea {
                id: scrubber
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                enabled: !!root.timeline && !root.timeline.remoteActive
                onPressed: mouse => {
                    root.focusTrack()
                    root.timeline.clearSelection()
                    root.timeline.seek(root.clampTime((mouse.x - 12) / root.pixelsPerMs))
                }
                onPositionChanged: mouse => { if (pressed) root.timeline.seek(root.clampTime((mouse.x - 12) / root.pixelsPerMs)) }
            }
            Repeater {
                model: Math.ceil(trackViewport.width / (root.gridStep * root.pixelsPerMs)) + 2
                Rectangle {
                    required property int index
                    readonly property real timeMs: (Math.floor(Math.max(0, trackViewport.contentX - 12) / root.pixelsPerMs / root.gridStep) + index) * root.gridStep
                    objectName: "timelineGridLine"
                    x: 12 + timeMs * root.pixelsPerMs
                    y: root.rulerHeight
                    visible: timeMs <= root.maximumMs
                    width: 1; height: timeContent.height - y
                    color: Theme.overlayBorder; opacity: 0.45
                }
            }
            Repeater {
                model: Math.ceil(trackViewport.width / (root.tickStep * root.pixelsPerMs)) + 2
                Item {
                    required property int index
                    readonly property real timeMs: (Math.floor(Math.max(0, trackViewport.contentX - 12) / root.pixelsPerMs / root.tickStep) + index) * root.tickStep
                    x: 12 + timeMs * root.pixelsPerMs
                    visible: timeMs <= root.maximumMs
                    height: timeContent.height
                    Rectangle { y: root.rulerHeight - 7; width: 1; height: 7; color: Theme.overlayDisabledText }
                    Rectangle { y: root.rulerHeight; width: 1; height: parent.height - root.rulerHeight; color: Theme.overlayBorder; opacity: 0.35 }
                    Text { x: 4; y: 3; text: root.formatTime(parent.timeMs); font.pixelSize: 10; color: Theme.overlayText }
                }
            }
            Item {
                id: keyTrack
                objectName: "timelineKeyframeTrack"
                y: root.rulerHeight
                width: parent.width; height: root.keyHeight
                Text { x: trackViewport.contentX + 6; y: 2; text: "KEYFRAMES"; font.pixelSize: 9; color: Theme.overlayDisabledText }
                Repeater {
                    model: root.timeline ? root.timeline.otherKeyframes : []
                    KeyframeDiamond {
                        required property var modelData
                        objectName: "otherMediaKeyframe"
                        x: 12 + modelData.timeMs * root.pixelsPerMs - width / 2
                        y: (keyTrack.height - height) / 2
                        opacity: root.timeline ? root.timeline.otherKeyframeOpacity : 0.3
                    }
                }
                Repeater {
                    model: root.timeline ? root.timeline.keyframes : []
                    Item {
                        id: keyItem
                        required property var modelData
                        objectName: "timelineKeyframe"
                        property bool dragging: false
                        property real previewMs: 0
                        property real rawMs: 0
                        readonly property real shownMs: dragging ? previewMs : modelData.timeMs
                        function refreshPreview() { previewMs = root.snapTime(rawMs, modelData.id, 0) }
                        x: 12 + shownMs * root.pixelsPerMs - width / 2
                        y: (keyTrack.height - height) / 2
                        z: dragging ? 2 : 1
                        width: 24; height: 24
                        KeyframeDiamond {
                            objectName: "timelineKeyframeDiamond"
                            anchors.centerIn: parent
                            color: root.timeline && root.timeline.selectedKeyframeId === keyItem.modelData.id ? Theme.accent : Theme.overlayText
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: root.editable
                            cursorShape: Qt.SizeHorCursor
                            property real pressX: 0
                            property real initialMs: 0
                            onPressed: mouse => {
                                root.focusTrack(); root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                                root.timeline.selectKeyframe(keyItem.modelData.id)
                                initialMs = keyItem.modelData.timeMs
                                pressX = mapToItem(timeContent, mouse.x, mouse.y).x
                                keyItem.rawMs = initialMs; keyItem.previewMs = initialMs
                                keyItem.dragging = true; root.activeDrag = keyItem
                            }
                            onPositionChanged: mouse => {
                                if (!pressed) return
                                root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                                keyItem.rawMs = initialMs + (mapToItem(timeContent, mouse.x, mouse.y).x - pressX) / root.pixelsPerMs
                                keyItem.refreshPreview()
                            }
                            onReleased: {
                                var id = keyItem.modelData.id; var ms = keyItem.previewMs
                                keyItem.dragging = false; root.endDrag()
                                if (ms !== initialMs) root.timeline.moveKeyframe(id, ms)
                            }
                            onCanceled: { keyItem.dragging = false; root.endDrag() }
                        }
                        ToolTip.visible: keyHover.hovered
                        ToolTip.text: root.formatTime(shownMs)
                        HoverHandler { id: keyHover }
                    }
                }
            }
            Item {
                id: clipTrack
                objectName: "timelineClipTrack"
                y: keyTrack.y + keyTrack.height
                width: parent.width; height: root.clipHeight
                Rectangle { anchors.fill: parent; color: Theme.overlaySelected; opacity: 0.45 }
                Rectangle { width: parent.width; height: 1; color: Theme.overlayBorder }
                Text { x: trackViewport.contentX + 6; y: 2; text: "VIDEO CLIPS"; font.pixelSize: 9; color: Theme.overlayDisabledText }
                Repeater {
                    model: root.timeline ? root.timeline.otherClips : []
                    Rectangle {
                        required property var modelData
                        objectName: "otherMediaVideoClip"
                        x: 12 + modelData.startMs * root.pixelsPerMs
                        y: 17
                        width: Math.max(2, modelData.durationMs * root.pixelsPerMs)
                        height: Math.max(12, clipTrack.height - 22)
                        radius: 3
                        color: Theme.overlayText
                        opacity: root.timeline ? root.timeline.otherKeyframeOpacity : 0.3
                    }
                }
                Repeater {
                    model: root.timeline ? root.timeline.clips : []
                    Rectangle {
                        id: clipItem
                        required property var modelData
                        objectName: "timelineVideoClip"
                        property bool dragging: false
                        property int editEdge: 0
                        property real previewStart: 0
                        property real previewEnd: 0
                        property real rawMs: 0
                        property real initialStart: 0
                        property real initialEnd: 0
                        readonly property real shownStart: dragging ? previewStart : modelData.startMs
                        readonly property real shownEnd: dragging ? previewEnd : modelData.startMs + modelData.durationMs
                        function refreshPreview() {
                            if (editEdge === 0) {
                                previewStart = Math.max(0, Math.min(root.maximumMs - (initialEnd - initialStart),
                                    root.snapTime(rawMs, modelData.id, initialEnd - initialStart)))
                                previewEnd = previewStart + (initialEnd - initialStart)
                            } else if (editEdge < 0) {
                                previewStart = Math.max(0, initialStart - modelData.sourceInMs,
                                    Math.min(initialEnd - root.slotMs, root.snapTime(rawMs, modelData.id, 0)))
                                previewEnd = initialEnd
                            } else {
                                previewStart = initialStart
                                previewEnd = Math.max(initialStart + root.slotMs, Math.min(root.maximumMs,
                                    initialStart + modelData.sourceDurationMs - modelData.sourceInMs,
                                    root.snapTime(rawMs, modelData.id, 0)))
                            }
                        }
                        function beginEdit(edge, mouse, area) {
                            root.focusTrack(); root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                            root.timeline.selectClip(modelData.id)
                            initialStart = modelData.startMs; initialEnd = modelData.startMs + modelData.durationMs
                            previewStart = initialStart; previewEnd = initialEnd
                            editEdge = edge; rawMs = edge > 0 ? initialEnd : initialStart
                            area.pressX = area.mapToItem(timeContent, mouse.x, mouse.y).x
                            dragging = true; root.activeDrag = clipItem
                        }
                        function updateEdit(mouse, area) {
                            root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                            rawMs = (editEdge > 0 ? initialEnd : initialStart)
                                + (area.mapToItem(timeContent, mouse.x, mouse.y).x - area.pressX) / root.pixelsPerMs
                            refreshPreview()
                        }
                        function finishEdit() {
                            var id = modelData.id; var start = root.clampTime(previewStart); var end = root.clampTime(previewEnd); var edge = editEdge
                            dragging = false; root.endDrag()
                            if (start === initialStart && end === initialEnd) return
                            if (edge === 0) root.timeline.moveClip(id, start)
                            else root.timeline.trimClip(id, start, end)
                        }
                        x: 12 + shownStart * root.pixelsPerMs
                        y: 17
                        z: dragging ? 3 : root.timeline && root.timeline.selectedClipId === modelData.id ? 2 : 1
                        width: Math.max(2, (shownEnd - shownStart) * root.pixelsPerMs)
                        height: Math.max(12, clipTrack.height - 22)
                        radius: 3
                        color: root.timeline && root.timeline.selectedClipId === modelData.id ? Theme.accent : Theme.overlayHover
                        border.color: Theme.overlayText
                        clip: true
                        Rectangle {
                            objectName: "timelineClipPadding"
                            // Resizing left or moving preserves the source exit; trimming right changes it.
                            readonly property real paddingMs: Math.max(0, clipItem.modelData.sourceEndMs
                                - clipItem.modelData.actualSourceDurationMs + (clipItem.dragging && clipItem.editEdge > 0 ? clipItem.shownEnd - clipItem.initialEnd : 0))
                            visible: paddingMs > 0
                            anchors.right: parent.right
                            width: Math.min(parent.width, Math.max(2, paddingMs * root.pixelsPerMs))
                            height: parent.height
                            color: Theme.overlayText; opacity: 0.4
                            border.color: Theme.overlayBackground
                        }
                        ToolTip.visible: clipHover.hovered && modelData.paddingMs > 0
                        ToolTip.text: "Final frame held silently for " + modelData.paddingMs.toFixed(3) + " ms"
                        HoverHandler { id: clipHover }
                        Text {
                            anchors.fill: parent; anchors.margins: 8
                            text: root.formatTime(clipItem.modelData.sourceInMs) + " → " + root.formatTime(clipItem.modelData.sourceOutMs)
                            font.pixelSize: 10; color: Theme.overlayText; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                        }
                        MouseArea {
                            id: clipMove
                            anchors.fill: parent
                            enabled: root.editable
                            cursorShape: Qt.SizeAllCursor
                            property real pressX: 0
                            onPressed: mouse => clipItem.beginEdit(0, mouse, clipMove)
                            onPositionChanged: mouse => { if (pressed) clipItem.updateEdit(mouse, clipMove) }
                            onReleased: clipItem.finishEdit()
                            onCanceled: { clipItem.dragging = false; root.endDrag() }
                        }
                        Repeater {
                            model: [-1, 1]
                            MouseArea {
                                id: trimHandle
                                required property int modelData
                                objectName: modelData < 0 ? "timelineClipTrimStart" : "timelineClipTrimEnd"
                                x: modelData < 0 ? 0 : clipItem.width - width
                                width: Math.min(8, clipItem.width / 3); height: clipItem.height
                                enabled: root.editable
                                cursorShape: Qt.SizeHorCursor
                                property real pressX: 0
                                Rectangle { anchors.centerIn: parent; width: 2; height: Math.min(20, parent.height - 6); color: Theme.overlayText }
                                onPressed: mouse => clipItem.beginEdit(modelData, mouse, trimHandle)
                                onPositionChanged: mouse => { if (pressed) clipItem.updateEdit(mouse, trimHandle) }
                                onReleased: clipItem.finishEdit()
                                onCanceled: { clipItem.dragging = false; root.endDrag() }
                            }
                        }
                    }
                }
            }
            Rectangle {
                x: 12 + (root.timeline ? root.timeline.effectiveEndMs : root.maximumMs) * root.pixelsPerMs
                y: root.rulerHeight
                width: Math.max(0, timeContent.width - x); height: timeContent.height - y
                color: "#44000000"
            }
            Rectangle {
                objectName: "timelinePlayhead"
                x: 12 + (root.timeline ? root.timeline.positionMs : 0) * root.pixelsPerMs
                width: 1; height: timeContent.height
                color: Theme.accent
                Shape {
                    id: playheadCap
                    objectName: "timelinePlayheadCap"
                    x: (parent.width - width) / 2
                    anchors.top: parent.top
                    width: 12; height: 14
                    preferredRendererType: Shape.CurveRenderer
                    antialiasing: true
                    ShapePath {
                        strokeWidth: -1
                        fillColor: Theme.accent
                        startX: 0; startY: 0
                        PathLine { x: playheadCap.width; y: 0 }
                        PathLine { x: playheadCap.width; y: playheadCap.height - 5 }
                        PathLine { x: playheadCap.width / 2; y: playheadCap.height }
                        PathLine { x: 0; y: playheadCap.height - 5 }
                        PathLine { x: 0; y: 0 }
                    }
                }
            }
            Item {
                id: stopMarker
                objectName: "timelineStopMarker"
                visible: !!root.timeline && root.timeline.stopTimeMs >= 0
                property bool dragging: false
                property real previewMs: 0
                property real rawMs: 0
                function refreshPreview() { previewMs = root.snapTime(rawMs, "", 0) }
                x: 12 + (dragging ? previewMs : root.timeline ? root.timeline.stopTimeMs : 0) * root.pixelsPerMs - 8
                width: 16; height: timeContent.height
                Rectangle { x: 7.5; width: 1; height: parent.height; color: "#ee7979" }
                Rectangle { width: 14; height: Math.max(16, root.rulerHeight - 4); color: "#b74646"; radius: 2 }
                Text { y: 2; anchors.horizontalCenter: parent.horizontalCenter; text: "S"; color: "white"; font.pixelSize: 11 }
                MouseArea {
                    width: parent.width; height: root.rulerHeight
                    enabled: root.editable
                    cursorShape: Qt.SizeHorCursor
                    onPressed: mouse => {
                        root.focusTrack(); root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                        stopMarker.previewMs = root.timeline.stopTimeMs; stopMarker.rawMs = stopMarker.previewMs
                        stopMarker.dragging = true; root.activeDrag = stopMarker
                    }
                    onPositionChanged: mouse => {
                        if (!pressed) return
                        root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                        stopMarker.rawMs = (mapToItem(timeContent, mouse.x, mouse.y).x - 12) / root.pixelsPerMs
                        stopMarker.refreshPreview()
                    }
                    onReleased: {
                        var ms = stopMarker.previewMs
                        stopMarker.dragging = false; root.endDrag(); root.timeline.setStopTime(ms)
                    }
                    onCanceled: { stopMarker.dragging = false; root.endDrag() }
                }
            }
            Rectangle {
                visible: root.snapGuideMs >= 0
                x: 12 + root.snapGuideMs * root.pixelsPerMs
                width: 1; height: timeContent.height
                color: Theme.accent
                Text { x: 4; y: root.rulerHeight + 2; text: root.snapGuideLabel; color: Theme.accent; font.pixelSize: 11 }
            }
        }
    }
    MouseArea {
        id: trackInput
        anchors.fill: trackViewport
        acceptedButtons: Qt.NoButton
        cursorShape: undefined
        hoverEnabled: true
        scrollGestureEnabled: true
        onWheel: wheel => root.handleWheel(wheel)
    }
    Text {
        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4
        text: root.timeline ? root.timeline.errorText : ""
        visible: text.length > 0
        color: "#ee7979"; font.pixelSize: 11
        width: Math.min(implicitWidth, root.width - 12); elide: Text.ElideRight
    }
    component TimelineShortcut: Shortcut {
        enabled: root.activeFocus && !root.textInputFocused && root.editable
        context: Qt.WindowShortcut
        autoRepeat: false
    }
    component TimelineZoomShortcut: Shortcut {
        enabled: !!root.timeline && !root.textInputFocused && (root.activeFocus || trackInput.containsMouse)
        context: Qt.WindowShortcut
        autoRepeat: true
    }
    TimelineZoomShortcut {
        sequences: ["+", "=", "Ctrl++", "Ctrl+=", "Meta++", "Meta+="]
        onActivated: root.zoom(0.5)
    }
    TimelineZoomShortcut {
        sequences: ["-", "Ctrl+-", "Meta+-"]
        onActivated: root.zoom(2)
    }
    TimelineShortcut { sequence: "Left"; autoRepeat: true; onActivated: root.timeline.stepSlots(-1) }
    TimelineShortcut { sequence: "Right"; autoRepeat: true; onActivated: root.timeline.stepSlots(1) }
    TimelineShortcut {
        sequences: ["Delete", "Backspace", "Ctrl+Backspace"]
        onActivated: root.timeline.deleteSelected()
    }
    TimelineShortcut {
        sequences: Qt.platform.os === "osx" ? [StandardKey.Copy, "Meta+C"] : [StandardKey.Copy]
        onActivated: root.timeline.copySelected()
    }
    TimelineShortcut {
        sequences: Qt.platform.os === "osx" ? [StandardKey.Paste, "Meta+V"] : [StandardKey.Paste]
        onActivated: root.timeline.paste()
    }
}
