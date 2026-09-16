import QtQuick
import Mouffette.App as AppStyle
import Mouffette.Canvas
BaseMediaItem {
    id: root
    property string textContent: ""
    property string horizontalAlignment: "center"
    property string verticalAlignment: "center"
    property bool fitToTextEnabled: true
    property string fontFamily: "Impact"
    property int fontPixelSize: 22
    property int fontWeight: 400
    property bool fontItalic: false
    property bool fontUnderline: false
    property bool fontUppercase: false
    property color textColor: "#FFFFFFFF"
    property real outlineWidthPx: 0.0
    property color outlineColor: "#FF000000"
    property bool highlightEnabled: false
    property color highlightColor: "#00000000"
    property bool textEditable: false
    property QtObject editingSession: null
    // A hosted editor derives its state solely from the canvas session. The
    // local flag supports standalone previews/tests without a canvas host.
    property bool standaloneEditing: false
    readonly property bool editing: editingSession
        ? editingSession.activeEditor === root : standaloneEditing
    signal textCommitRequested(string mediaId, string text)
    signal textLiveUpdateRequested(string mediaId, string text)

    clip: true
    pointerEnabled: !root.editing
    doubleClickEnabled: true

    // Radius of the border around the glyph, in scene-space pixels.
    readonly property real outlinePixels: Math.max(0, root.outlineWidthPx)
    // The model reserves this same safety margin in fit-to-text mode. It must
    // also surround the content, otherwise left/right/top/bottom borders clip.
    readonly property real textInset: 4 + (outlinePixels > 0 ? Math.ceil(outlinePixels) + 1 : 0)

    function commitAndStopEditing() {
        if (!root.editing) return
        if (root.editingSession) {
            root.editingSession.finish(root)
        } else {
            var text = textDisplayNode.text
            root.standaloneEditing = false
            root.textCommitRequested(root.mediaId, text)
        }
    }

    function currentText() { return textDisplayNode.text }

    function selectAllText() { textDisplayNode.selectAll() }

    function placeCursorAtScenePoint(sceneX, sceneY) {
        var localPoint = textDisplayNode.mapFromItem(null, sceneX, sceneY)
        var position = textDisplayNode.positionAt(localPoint.x, localPoint.y)
        textDisplayNode.select(position, position)
    }

    onPrimaryDoubleClicked: function(mediaId, additive, hasScenePosition, sceneX, sceneY) {
        if (!root.textEditable)
            return
        root.selectRequested(mediaId, additive)
        if (root.editingSession)
            root.editingSession.begin(root)
        else
            root.standaloneEditing = true
        if (root.editing && hasScenePosition)
            root.placeCursorAtScenePoint(sceneX, sceneY)
    }

    onEditingChanged: {
        if (root.editing) {
            textDisplayNode.forceActiveFocus()
        } else {
            textDisplayNode.select(0, 0)
            textDisplayNode.focus = false
        }
    }

    Component.onDestruction: {
        if (root.editingSession)
            root.editingSession.abandon(root)
    }

    // ── Single TextEdit for both display and edit mode ──────────────────────
    // Using one document eliminates every class of display↔edit sync bug:
    // font metrics, line heights, contentHeight, topPadding, and wrapMode
    // are all computed once by a single QTextDocumentLayout instance.
    //
    // Display mode: readOnly:true, enabled:false (no event capture),
    //               cursorVisible:false. Text is kept in sync with
    //               root.textContent via the Binding below.
    // Edit mode:    readOnly:false, enabled:true, cursorVisible:true.
    //               The same document remains intact while its model binding
    //               is suspended; entering edit mode is not a text mutation.
    TextEdit {
        id: textDisplayNode
        z: 1
        anchors.fill: parent
        anchors.margins: root.textInset
        readOnly:           !root.editing
        enabled:            root.editing   // false in display mode → no event capture
        activeFocusOnTab:   false          // focus managed imperatively
        cursorVisible:      root.editing
        selectByMouse:      false          // custom MouseArea handles selection
        topPadding: {
            var extra = Math.max(0, height - contentHeight)
            if (root.verticalAlignment === "top")    return 0
            if (root.verticalAlignment === "bottom") return extra
            return extra * 0.5
        }
        leftPadding:   0
        rightPadding:  0
        bottomPadding: 0
        color: root.textColor
        selectionColor: AppStyle.Theme.selectionBackground
        selectedTextColor: AppStyle.Theme.selectionText
        textFormat: TextEdit.PlainText
        wrapMode: root.fitToTextEnabled ? TextEdit.NoWrap : TextEdit.Wrap
        renderType: Text.QtRendering
        horizontalAlignment: root.horizontalAlignment === "left"
            ? Text.AlignLeft
            : (root.horizontalAlignment === "right" ? Text.AlignRight : Text.AlignHCenter)
        font.family:          root.fontFamily
        font.pixelSize:       Math.max(1, root.fontPixelSize)
        font.weight:          root.fontWeight
        font.italic:          root.fontItalic
        font.underline:       root.fontUnderline
        font.capitalization:  root.fontUppercase ? Font.AllUppercase : Font.MixedCase
        font.kerning:         true
        font.preferShaping:   true
        font.hintingPreference: Font.PreferNoHinting

        // Installs the trailing-space alignment fix once on this document.
        // A single document means the fix is applied exactly once and covers
        // both display and edit modes.
        Component.onCompleted: TextEditHelper.applyIncludeTrailingSpaces(textDisplayNode)

        Keys.onPressed: function(event) {
            var isEnter = (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            var hasCommitModifier = (event.modifiers & Qt.ControlModifier) || (event.modifiers & Qt.MetaModifier)
            if (isEnter && hasCommitModifier) {
                root.commitAndStopEditing()
                event.accepted = true
            }
        }

        onTextChanged: {
            if (root.editing) {
                root.textLiveUpdateRequested(root.mediaId, textDisplayNode.text)
            }
        }
    }

    // Keep text in sync with the committed model value while not editing.
    // The Binding is inactive (when:false) during edit mode so user keystrokes
    // are not overwritten by model updates that arrive via live-update round-trips.
    Binding {
        target:   textDisplayNode
        property: "text"
        value:    root.textContent || ""
        when:     !root.editing
        // The default restore mode would restore the pre-binding empty value
        // on entry and could publish that empty document as a live user edit.
        restoreMode: Binding.RestoreNone
    }

    // Cursor placement and drag-select.
    // selectByMouse:false on textDisplayNode avoids Qt6 coordinate-mapping
    // issues in scaled/transformed viewports; this area provides equivalent
    // behaviour with explicit positionAt() calls.
    MouseArea {
        id: textEditMouseArea
        anchors.fill: textDisplayNode
        visible:         root.editing
        z:               2   // above textDisplayNode (z:1)
        acceptedButtons: Qt.LeftButton
        property int pressCharPos: 0

        onPressed: function(mouse) {
            textDisplayNode.forceActiveFocus()
            // textEditMouseArea and textDisplayNode share the same coordinate
            // space (same parent, same anchors), so no mapToItem is needed.
            var pos = textDisplayNode.positionAt(mouse.x, mouse.y)
            pressCharPos = pos
            textDisplayNode.cursorPosition = pos
            mouse.accepted = true
        }

        onPositionChanged: function(mouse) {
            if (pressed) {
                var pos = textDisplayNode.positionAt(mouse.x, mouse.y)
                textDisplayNode.select(pressCharPos, pos)
            }
        }
    }

    Rectangle {
        id: textBackground
        // Follow the same live document as the fill and border. The committed
        // model text may be stale while the user is typing or deleting.
        visible: root.highlightEnabled
                 && (textDisplayNode.length > 0 || textDisplayNode.preeditText.length > 0)
        color: root.highlightColor
        radius: 2

        readonly property real contentMargin: root.textInset
        readonly property real highlightPadding: 2
        readonly property real contentWidth: Math.max(0, root.width - contentMargin * 2)
        readonly property real contentHeight: Math.max(0, root.height - contentMargin * 2)
        // contentWidth/Height on TextEdit is equivalent to paintedWidth/Height on Text.
        readonly property real paintedW: Math.max(0, Math.min(contentWidth, textDisplayNode.contentWidth))
        readonly property real paintedH: Math.max(0, Math.min(contentHeight, textDisplayNode.contentHeight))

        width: paintedW + highlightPadding * 2
        height: paintedH + highlightPadding * 2

        x: {
            if (root.horizontalAlignment === "left")
                return contentMargin - highlightPadding
            if (root.horizontalAlignment === "right")
                return contentMargin + (contentWidth - paintedW) - highlightPadding
            return contentMargin + (contentWidth - paintedW) * 0.5 - highlightPadding
        }
        y: {
            if (root.verticalAlignment === "top")
                return contentMargin - highlightPadding
            if (root.verticalAlignment === "bottom")
                return contentMargin + (contentHeight - paintedH) - highlightPadding
            return contentMargin + (contentHeight - paintedH) * 0.5 - highlightPadding
        }
    }

    // The border reads the exact document, resolved fonts and positions of the
    // editor. Cached glyph masks use Qt's ordinary image nodes and texture atlas.
    TextOutlineItem {
        id: outlineRenderer
        anchors.fill: parent
        source: textDisplayNode
        outlinePixels: root.outlineColor.a > 0 ? root.outlinePixels : 0
        color: Qt.rgba(root.outlineColor.r, root.outlineColor.g, root.outlineColor.b, 1)
        opacity: root.outlineColor.a > 0 ? 1 : 0
    }

    // Apply translucent alpha once, including overlapping glyphs. An explicit
    // viewport-sized source avoids an enormous document-sized layer texture.
    // Its destination matches its source crop; layer.sourceRect alone would
    // stretch the cropped image back over the full document.
    ShaderEffectSource {
        visible: root.outlineColor.a > 0 && root.outlineColor.a < 1
                 && root.outlinePixels > 0
                 && outlineRenderer.renderedRect.width > 0
                 && outlineRenderer.renderedRect.height > 0
        sourceItem: visible ? outlineRenderer : null
        hideSource: true
        sourceRect: outlineRenderer.renderedRect
        x: sourceRect.x
        y: sourceRect.y
        width: sourceRect.width
        height: sourceRect.height
        textureSize: outlineRenderer.renderedPixelSize
        opacity: root.outlineColor.a
        smooth: true
    }
}
