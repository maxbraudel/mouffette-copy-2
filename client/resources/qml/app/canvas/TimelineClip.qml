import QtQuick
import QtQuick.Controls
import Mouffette.App

Rectangle {
    id: clipItem
    required property var modelData
    required property var panel
    required property Item timeContent
    required property real trackHeight
    property bool interactive: true
    objectName: interactive ? "timelineClip" : "otherMediaClip"
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
            previewStart = Math.max(0, Math.min(panel.maximumMs - (initialEnd - initialStart),
                panel.snapTime(rawMs, modelData.id, initialEnd - initialStart)))
            previewEnd = previewStart + (initialEnd - initialStart)
        } else if (editEdge < 0) {
            previewStart = Math.max(0,
                Math.min(initialEnd - panel.slotMs, panel.snapTime(rawMs, modelData.id, 0)))
            previewEnd = initialEnd
        } else {
            previewStart = initialStart
            previewEnd = Math.max(initialStart + panel.slotMs, Math.min(panel.maximumMs,
                panel.snapTime(rawMs, modelData.id, 0)))
        }
    }
    function beginEdit(edge, mouse, area) {
        panel.focusTrack(); panel.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
        panel.timeline.selectClip(modelData.id)
        initialStart = modelData.startMs; initialEnd = modelData.startMs + modelData.durationMs
        previewStart = initialStart; previewEnd = initialEnd
        editEdge = edge; rawMs = edge > 0 ? initialEnd : initialStart
        area.pressX = area.mapToItem(timeContent, mouse.x, mouse.y).x
        dragging = true; panel.activeDrag = clipItem
    }
    function updateEdit(mouse, area) {
        panel.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
        rawMs = (editEdge > 0 ? initialEnd : initialStart)
            + (area.mapToItem(timeContent, mouse.x, mouse.y).x - area.pressX) / panel.pixelsPerMs
        refreshPreview()
    }
    function finishEdit() {
        var id = modelData.id; var start = panel.clampTime(previewStart); var end = panel.clampTime(previewEnd); var edge = editEdge
        dragging = false; panel.endDrag()
        if (start === initialStart && end === initialEnd) return
        if (edge === 0) panel.timeline.moveClip(id, start)
        else panel.timeline.trimClip(id, start, end)
    }
    x: 12 + shownStart * panel.pixelsPerMs
    y: 17
    z: dragging ? 3 : interactive && panel.timeline && panel.timeline.selectedClipId === modelData.id ? 2 : 1
    width: Math.max(2, (shownEnd - shownStart) * panel.pixelsPerMs)
    height: Math.max(12, trackHeight - 22)
    radius: 3
    color: interactive && panel.timeline && panel.timeline.selectedClipId === modelData.id
        ? Theme.controlSelectionBackground : interactive ? Theme.overlayHover : Theme.overlayPressed
    border.width: interactive ? 1 : 0
    border.color: Theme.overlayText
    clip: true
    readonly property real shownSourceIn: modelData.sourceInMs
        + (dragging && editEdge < 0 ? shownStart - initialStart : 0)
    readonly property real shownDuration: shownEnd - shownStart
    readonly property real leadingHoldMs: modelData.isVideo
        ? Math.min(shownDuration, Math.max(0, -shownSourceIn)) : 0
    readonly property real trailingHoldMs: modelData.isVideo
        ? Math.min(shownDuration, Math.max(0, shownSourceIn + shownDuration - modelData.actualSourceDurationMs)) : 0
    HoldRegion {
        objectName: "timelineClipLeadingHold"
        width: clipItem.leadingHoldMs * panel.pixelsPerMs
    }
    HoldRegion {
        objectName: "timelineClipTrailingHold"
        x: clipItem.width - width
        width: clipItem.trailingHoldMs * panel.pixelsPerMs
    }
    ToolTip.visible: clipHover.hovered
    ToolTip.text: modelData.mediaName
        + (leadingHoldMs > 0 ? "\nFirst frame held silently for " + leadingHoldMs.toFixed(3) + " ms" : "")
        + (trailingHoldMs > 0 ? "\nLast frame held silently for " + trailingHoldMs.toFixed(3) + " ms" : "")
    HoverHandler { id: clipHover }
    Text {
        anchors.fill: parent; anchors.margins: 8
        visible: clipItem.interactive
        text: panel.formatTime(clipItem.shownStart) + " → " + panel.formatTime(clipItem.shownEnd)
        font.pixelSize: 10; color: Theme.overlayText; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    component HoldRegion: Rectangle {
        height: clipItem.height
        visible: width > 0
        color: Theme.overlayText
        opacity: 0.12
    }
    MouseArea {
        id: clipMove
        anchors.fill: parent
        enabled: clipItem.interactive && panel.editable
        cursorShape: Qt.SizeAllCursor
        property real pressX: 0
        onPressed: mouse => clipItem.beginEdit(0, mouse, clipMove)
        onPositionChanged: mouse => { if (pressed) clipItem.updateEdit(mouse, clipMove) }
        onReleased: clipItem.finishEdit()
        onCanceled: { clipItem.dragging = false; panel.endDrag() }
    }
    Repeater {
        model: [-1, 1]
        MouseArea {
            id: trimHandle
            required property int modelData
            objectName: modelData < 0 ? "timelineClipTrimStart" : "timelineClipTrimEnd"
            x: modelData < 0 ? 0 : clipItem.width - width
            width: Math.min(8, clipItem.width / 3); height: clipItem.height
            visible: clipItem.interactive
            enabled: clipItem.interactive && panel.editable
            cursorShape: Qt.SizeHorCursor
            property real pressX: 0
            Rectangle { anchors.centerIn: parent; width: 2; height: Math.min(20, parent.height - 6); color: Theme.overlayText }
            onPressed: mouse => clipItem.beginEdit(modelData, mouse, trimHandle)
            onPositionChanged: mouse => { if (pressed) clipItem.updateEdit(mouse, trimHandle) }
            onReleased: clipItem.finishEdit()
            onCanceled: { clipItem.dragging = false; panel.endDrag() }
        }
    }
}
