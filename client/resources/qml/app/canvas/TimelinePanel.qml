import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import QtQuick.Window
import Mouffette.App
import "../components"

FocusScope {
    id: root
    objectName: "sceneTimeline"
    required property var session
    property real bottomCornerRadius: 0
    readonly property var timeline: session ? session.timeline : null
    readonly property bool editable: !!timeline && timeline.editable
    readonly property real maximumMs: timeline ? timeline.maxDurationMs : 180000
    property real viewDurationMs: timeline ? Math.min(maximumMs, timeline.initialViewDurationMs) : 15000
    readonly property real pixelsPerMs: Math.max(1, trackViewport.width - 24) / Math.max(1, viewDurationMs)
    readonly property real rulerHeight: timeline ? timeline.rulerHeightPx : 28
    readonly property real clipHeight: timeline ? timeline.clipTrackHeightPx : 64
    readonly property real keyHeight: Math.max(30, trackViewport.height - rulerHeight - clipHeight)
    readonly property real slotMs: 1000 / (timeline ? timeline.slotsPerSecond : 30)
    readonly property int maximumFrame: Math.round(maximumMs / slotMs)
    readonly property int frameDigits: String(maximumFrame).length
    readonly property int minuteDigits: Math.max(2, String(Math.floor(Math.round(maximumMs) / 60000)).length)
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
        return String(Math.floor(ms / 60000)).padStart(minuteDigits, "0") + ":"
             + String(Math.floor(ms / 1000) % 60).padStart(2, "0") + "."
             + String(ms % 1000).padStart(3, "0")
    }
    function formatFrame(frame) {
        return "#" + String(Math.max(0, Math.round(frame))).padStart(frameDigits, "0")
    }
    function focusTrack() { root.forceActiveFocus() }
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
    Rectangle {
        anchors.fill: parent
        color: Theme.overlayBackground
        bottomLeftRadius: root.bottomCornerRadius
        bottomRightRadius: root.bottomCornerRadius
    }

    component TimelineButton: AppButton {
        focusPolicy: Qt.NoFocus
    }
    component TimelineEditButton: TimelineButton {
        iconOnly: editBar.compactButtons
    }
    function buttonRowTextWidth(row) {
        var width = 0
        var count = 0
        for (var i = 0; i < row.children.length; ++i) {
            var child = row.children[i]
            if (!child.visible) continue
            width += child instanceof AppButton ? child.textWidth : child.implicitWidth
            ++count
        }
        return width + Math.max(0, count - 1) * row.spacing
    }
    component KeyframeDiamond: Rectangle {
        width: root.timeline ? root.timeline.keyframeSizePx : 10
        height: width
        rotation: 45
        color: Theme.overlayText
        border.color: Theme.overlayBackground
    }
    TextMetrics {
        id: readoutMetrics
        font.family: Theme.monospaceFontFamily
        font.pixelSize: Theme.controlFontSize
        text: root.formatTime(root.maximumMs) + root.formatFrame(root.maximumFrame)
    }
    component TimelineReadout: Item {
        id: readout
        required property real timeMs
        required property int frame
        required property string description
        property alias timeObjectName: timeLabel.objectName
        property alias frameObjectName: frameLabel.objectName
        readonly property int gap: 6
        readonly property real labelSize: Math.min(Theme.controlFontSize,
            Theme.controlFontSize * Math.max(1, width - gap * 2 - 1) / Math.max(1, readoutMetrics.width))
        implicitWidth: readoutMetrics.width + gap * 2 + 1
        implicitHeight: Theme.controlHeight
        width: implicitWidth; height: implicitHeight
        Accessible.role: Accessible.StaticText
        Accessible.name: description + ": " + timeLabel.text + ", " + frameLabel.text

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: readout.gap
            Text {
                id: timeLabel
                height: readout.height
                text: root.formatTime(readout.timeMs)
                color: Theme.mutedText
                font.family: Theme.monospaceFontFamily
                font.pixelSize: readout.labelSize
                verticalAlignment: Text.AlignVCenter
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 1; height: 16
                color: Theme.border
            }
            Text {
                id: frameLabel
                height: readout.height
                text: root.formatFrame(readout.frame)
                color: Theme.mutedText
                font.family: Theme.monospaceFontFamily
                font.pixelSize: readout.labelSize
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
    Item {
        id: transportViewport
        objectName: "timelineTransportBar"
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 8
        height: Theme.controlHeight
        readonly property real gap: 6
        readonly property bool compactButtons: width < playback.textWidth + currentReadout.implicitWidth * 2 + 24
        readonly property real sideWidth: Math.max(0, (width - playback.width) / 2 - 12)

        TimelineReadout {
            id: currentReadout
            objectName: "timelineCurrentReadout"
            anchors.left: parent.left
            width: Math.min(implicitWidth, transportViewport.sideWidth)
            timeObjectName: "timelineCurrentTime"
            frameObjectName: "timelineCurrentFrame"
            description: "Current position"
            timeMs: root.timeline ? root.timeline.positionMs : 0
            frame: root.timeline ? root.timeline.positionSlot : 0
        }
        TimelineReadout {
            objectName: "timelineMaximumReadout"
            anchors.right: parent.right
            width: Math.min(implicitWidth, transportViewport.sideWidth)
            timeObjectName: "timelineMaximumTime"
            frameObjectName: "timelineMaximumFrame"
            description: "Maximum duration"
            timeMs: root.maximumMs
            frame: root.maximumFrame
        }
        Row {
            id: playback
            objectName: "timelinePlaybackControls"
            anchors.horizontalCenter: parent.horizontalCenter
            height: parent.height
            spacing: transportViewport.gap
            readonly property real textWidth: startButton.textWidth + playButton.textWidth
                + endButton.textWidth + spacing * 2
                + (remoteStopButton.visible ? remoteStopButton.textWidth + spacing : 0)
            TimelineButton {
                id: startButton
                objectName: "timelineGoToStart"
                text: "Start"
                iconSource: "qrc:/icons/icons/timeline/start.svg"
                iconOnly: transportViewport.compactButtons
                Accessible.name: "Return to start"
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.goToStart() }
            }
            TimelineButton {
                id: playButton
                objectName: "timelinePlayPause"
                text: root.timeline && root.timeline.playing ? "Pause" : "Play"
                textVariants: ["Play", "Pause"]
                iconSource: root.timeline && root.timeline.playing
                    ? "qrc:/icons/icons/pause.svg" : "qrc:/icons/icons/play.svg"
                iconOnly: transportViewport.compactButtons
                primary: true
                checked: !!root.timeline && root.timeline.playing
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.togglePlayback() }
            }
            TimelineButton {
                id: endButton
                objectName: "timelineGoToEnd"
                text: "End"
                iconSource: "qrc:/icons/icons/timeline/end.svg"
                iconOnly: transportViewport.compactButtons
                Accessible.name: "Go to scene end"
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.goToEnd() }
            }
            TimelineButton {
                id: remoteStopButton
                objectName: "timelineStopRemote"
                text: "Stop remote scene"
                iconSource: "qrc:/icons/icons/stop.svg"
                iconOnly: transportViewport.compactButtons
                destructive: true
                visible: !!root.timeline && root.timeline.remoteActive
                enabled: !!root.session && !!root.session.remoteSceneActionEnabled
                onClicked: root.session.toggleRemoteScene()
            }
        }
    }
    Rectangle {
        id: transportSeparator
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: transportViewport.bottom; anchors.topMargin: 8
        height: 1
        color: Theme.border
    }
    Item {
        id: editBar
        objectName: "timelineEditBar"
        // Measure labels independently of the current mode so resizing cannot oscillate.
        readonly property bool compactButtons: width < actionViewport.contentPadding * 2
            + root.buttonRowTextWidth(actions) + actions.spacing + root.buttonRowTextWidth(layoutActions)
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: transportSeparator.bottom
        anchors.topMargin: 8
        height: Theme.controlHeight
        Flickable {
            id: actionViewport
            objectName: "timelineEditActions"
            readonly property int contentPadding: 8
            anchors.fill: parent
            clip: true
            // Compact buttons share one scrollable row if their icons still do not fit.
            contentWidth: Math.max(width, contentPadding * 2 + actions.width + actions.spacing + layoutActions.width)
            contentHeight: height
            boundsBehavior: Flickable.StopAtBounds
            interactive: false
            Row {
                id: actions
                x: actionViewport.contentPadding
                spacing: 6
                TimelineEditButton {
                    objectName: "timelinePlaceKeyframe"
                    text: root.timeline && root.timeline.hasKeyframeAtPosition ? "Update keyframe" : "Place keyframe"
                    textVariants: ["Update keyframe", "Place keyframe"]
                    iconSource: root.timeline && root.timeline.hasKeyframeAtPosition
                        ? "qrc:/icons/icons/timeline/keyframe-update.svg" : "qrc:/icons/icons/timeline/keyframe-add.svg"
                    enabled: root.editable && root.timeline.canCapture
                    onClicked: { root.focusTrack(); root.timeline.placeKeyframe() }
                }
                TimelineEditButton {
                    objectName: "timelineSplitClip"
                    text: "Split clip"
                    iconSource: "qrc:/icons/icons/timeline/split.svg"
                    visible: !!root.timeline && root.timeline.primaryIsVideo
                    enabled: root.editable && root.timeline.canSplit
                    onClicked: { root.focusTrack(); root.timeline.splitClip() }
                }
                TimelineEditButton {
                    objectName: "timelineInsertClip"
                    text: "Insert full video"
                    iconSource: "qrc:/icons/icons/timeline/insert.svg"
                    visible: !!root.timeline && root.timeline.primaryIsVideo
                    enabled: root.editable
                    onClicked: { root.focusTrack(); root.timeline.insertFullClip() }
                }
                TimelineEditButton {
                    objectName: "timelineCopy"
                    text: "Copy"
                    iconSource: "qrc:/icons/icons/timeline/copy.svg"
                    enabled: root.editable && (root.timeline.selectedKeyframeId !== "" || root.timeline.selectedClipId !== "")
                    onClicked: { root.focusTrack(); root.timeline.copySelected() }
                }
                TimelineEditButton {
                    objectName: "timelinePaste"
                    text: "Paste"
                    iconSource: "qrc:/icons/icons/timeline/paste.svg"
                    enabled: root.editable && root.timeline.canPaste
                    onClicked: { root.focusTrack(); root.timeline.paste() }
                }
                TimelineEditButton {
                    objectName: "timelineDelete"
                    text: "Delete"
                    iconSource: "qrc:/icons/icons/delete.svg"
                    destructive: true
                    enabled: root.editable && (root.timeline.selectedKeyframeId !== "" || root.timeline.selectedClipId !== "")
                    onClicked: { root.focusTrack(); root.timeline.deleteSelected() }
                }
                TimelineEditButton {
                    objectName: "timelinePlaceStop"
                    text: root.timeline && root.timeline.stopTimeMs >= 0 ? "Move Stop here" : "Place Stop"
                    textVariants: ["Move Stop here", "Place Stop"]
                    iconSource: "qrc:/icons/icons/timeline/stop-add.svg"
                    enabled: root.editable
                    onClicked: { root.focusTrack(); root.timeline.placeStop() }
                }
                TimelineEditButton {
                    objectName: "timelineRemoveStop"
                    text: "Remove Stop"
                    iconSource: "qrc:/icons/icons/timeline/stop-remove.svg"
                    destructive: true
                    visible: !!root.timeline && root.timeline.stopTimeMs >= 0
                    enabled: root.editable
                    onClicked: { root.focusTrack(); root.timeline.removeStop() }
                }
                Text {
                    height: Theme.controlHeight
                    visible: !!root.timeline && root.timeline.hasDraft
                    text: "Unsaved draft · place a keyframe to keep changes"
                    color: Theme.warningText; font.pixelSize: Theme.controlFontSize
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Row {
                id: layoutActions
                objectName: "timelineLayoutControls"
                anchors.right: parent.right
                anchors.rightMargin: actionViewport.contentPadding
                height: Theme.controlHeight
                spacing: transportViewport.gap
                TimelineEditButton {
                    id: zoomOutButton
                    objectName: "timelineZoomOut"
                    text: "Zoom out"
                    iconSource: "qrc:/icons/icons/timeline/zoom-out.svg"
                    enabled: !!root.timeline
                    onClicked: root.zoom(2)
                }
                TimelineEditButton {
                    id: zoomInButton
                    objectName: "timelineZoomIn"
                    text: "Zoom in"
                    iconSource: "qrc:/icons/icons/timeline/zoom-in.svg"
                    enabled: !!root.timeline
                    onClicked: root.zoom(0.5)
                }
                TimelineEditButton {
                    id: fitButton
                    objectName: "timelineFitDuration"
                    text: "Fit duration"
                    iconSource: "qrc:/icons/icons/timeline/fit.svg"
                    enabled: !!root.timeline
                    onClicked: { root.viewDurationMs = root.maximumMs; trackViewport.contentX = 0 }
                }
            }
        }
        MouseArea {
            anchors.fill: actionViewport
            acceptedButtons: Qt.NoButton
            cursorShape: undefined
            scrollGestureEnabled: true
            onWheel: wheel => {
                var precise = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0
                var dx = precise ? wheel.pixelDelta.x : wheel.angleDelta.x / 8
                var dy = precise ? wheel.pixelDelta.y : wheel.angleDelta.y / 8
                var delta = (Math.abs(dx) > Math.abs(dy) ? dx : dy) * (precise ? 1 : 3)
                actionViewport.contentX = Math.max(0, Math.min(actionViewport.contentWidth - actionViewport.width,
                    actionViewport.contentX - delta))
                wheel.accepted = true
            }
        }
    }
    Rectangle {
        id: tracksSeparator
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: editBar.bottom; anchors.topMargin: 8
        height: 1
        color: Theme.border
    }
    Flickable {
        id: trackViewport
        objectName: "timelineTracks"
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: tracksSeparator.bottom; anchors.bottom: parent.bottom
        clip: true
        contentWidth: Math.max(width, root.maximumMs * root.pixelsPerMs + 24)
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        interactive: false
        ScrollBar.horizontal: ScrollBar {
            policy: ScrollBar.AlwaysOn
            z: 1
            background: null
        }
        Item {
            id: timeContent
            width: trackViewport.contentWidth; height: trackViewport.height
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
